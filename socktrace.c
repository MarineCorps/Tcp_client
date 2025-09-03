// build:  gcc -shared -fPIC -O2 -pthread -ldl -o libsocktrace.so socktrace.c
// run  :  LD_PRELOAD=./libsocktrace.so ./server   (또는 ./client)
// env  :  SOCKTRACE_PREFIX="[trace] "  // (선택) 로그 앞에 붙일 접두사

#define _GNU_SOURCE
#include <dlfcn.h>       // dlsym, RTLD_NEXT
#include <sys/socket.h>  // socket API, getsockopt, SOL_SOCKET, SO_TYPE
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>  // sockaddr_in
#include <arpa/inet.h>   // inet_ntop
#include <pthread.h>     // pthread_mutex_* (멀티스레드 보호)
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>      // read, write
#include <stdio.h>
#include <time.h>        // clock_gettime

#define FD_SETSIZE 1024  // 데모용 FD 메타 테이블 크기(간단하게)


// ---------- 원래 libc 심볼 포인터들 (후킹에서 원함수 호출용) ----------
static ssize_t (*real_read)(int, void*, size_t);
static ssize_t (*real_write)(int, const void*, size_t);
static ssize_t (*real_send)(int, const void*, size_t, int);
static ssize_t (*real_recv)(int, void*, size_t, int);
static int     (*real_connect)(int, const struct sockaddr*, socklen_t);
static int     (*real_accept)(int, struct sockaddr*, socklen_t*);

// 전역 락: FD 메타 데이터 갱신 및 로그 출력의 경쟁상태를 막기 위함
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------- FD별 메타데이터(로컬/피어 주소, 패밀리 등) ----------
typedef struct {
    int inited;                         // 이 FD에 대해 초기화했는지
    int is_socket;                      // 소켓 FD인지 여부(SO_TYPE으로 판별)
    int family;                         // 주소 패밀리(AF_INET/AF_INET6)
    struct sockaddr_storage local;      // getsockname 결과(로컬)
    struct sockaddr_storage peer;       // getpeername 결과(상대)
    socklen_t               local_len;
    socklen_t               peer_len;
} fd_meta;

static fd_meta M[FD_SETSIZE];           // 데모용 고정 테이블
static char g_prefix[64] = "";          // 로그 접두사

// ---------- 유틸: 재귀 방지 write ----------
static inline void safe_write(const char* s, size_t n){
    // printf는 내부에서 write를 호출 → 재귀 위험.
    // 원 write(=real_write)로 stderr에 직접 쓰기
    if (!real_write) return;
    (void)real_write(STDERR_FILENO, s, n);
}

// printf 스타일 포맷 로깅(접두사 + vsnprintf + safe_write)
static void logf(const char* fmt, ...){
    char buf[512];
    int n = 0;
    if (g_prefix[0]) n = snprintf(buf, sizeof(buf), "%s", g_prefix);
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(buf+n, sizeof(buf)-n, fmt, ap);
    va_end(ap);
    if (n>0) safe_write(buf, (size_t)n);
}

// MONOTONIC 시간(초) — 벽시계 변경 영향 없음
static double now_s(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec/1e9;
}

// 소켓 FD 여부 판별(SO_TYPE 조회 성공 시 소켓)
static int is_socket_fd(int fd){
    int t; socklen_t l=sizeof(t);
    return getsockopt(fd, SOL_SOCKET, SO_TYPE, &t, &l) == 0;
}

// 로컬/피어 주소 갱신 (연결 후/수신/송신 시 최신화)
static void refresh_endpoints(int fd){
    if (fd<0 || fd>=FD_SETSIZE) return;
    fd_meta* m = &M[fd];
    if (!m->is_socket) return;
    m->local_len = sizeof(m->local);
    m->peer_len  = sizeof(m->peer);
    getsockname(fd, (struct sockaddr*)&m->local, &m->local_len);
    getpeername (fd, (struct sockaddr*)&m->peer,  &m->peer_len);
}

// FD 메타 초기화(최초 접근 시 1회)
static void ensure_fd(int fd){
    if (fd<0 || fd>=FD_SETSIZE) return;
    fd_meta* m = &M[fd];
    if (!m->inited){
        memset(m, 0, sizeof(*m));
        m->inited   = 1;
        m->is_socket= is_socket_fd(fd);
        if (m->is_socket) {
            m->local_len = sizeof(m->local);
            m->peer_len  = sizeof(m->peer);
            getsockname(fd, (struct sockaddr*)&m->local, &m->local_len);
            getpeername (fd, (struct sockaddr*)&m->peer,  &m->peer_len);
            if (m->local.ss_family) m->family = m->local.ss_family;
            else if (m->peer.ss_family) m->family = m->peer.ss_family;
        }
    }
}

// 주소 출력용 포맷터(IPv4/IPv6 지원)
static void fmt_addr(char* out, size_t cap, const struct sockaddr_storage* ss){
    out[0]='\0';
    if (!ss || ss->ss_family==0) { snprintf(out, cap, "?"); return; }
    if (ss->ss_family == AF_INET){
        const struct sockaddr_in* a = (const struct sockaddr_in*)ss;
        char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        snprintf(out, cap, "%s:%u", ip, (unsigned)ntohs(a->sin_port));
    } else if (ss->ss_family == AF_INET6){
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)ss;
        char ip[INET6_ADDRSTRLEN]; inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
        snprintf(out, cap, "[%s]:%u", ip, (unsigned)ntohs(a->sin6_port));
    } else {
        snprintf(out, cap, "fam=%d", ss->ss_family);
    }
}

// 라이브러리 로드 시 초기화(생성자)
__attribute__((constructor))
static void init(void){
    // 원 심볼 획득(후킹 내부에서 원래 함수 부르기 위함)
    real_read    = dlsym(RTLD_NEXT, "read");
    real_write   = dlsym(RTLD_NEXT, "write");
    real_send    = dlsym(RTLD_NEXT, "send");
    real_recv    = dlsym(RTLD_NEXT, "recv");
    real_connect = dlsym(RTLD_NEXT, "connect");
    real_accept  = dlsym(RTLD_NEXT, "accept");

    // 로그 접두사 환경변수
    const char* p = getenv("SOCKTRACE_PREFIX");
    if (p && *p) snprintf(g_prefix, sizeof(g_prefix), "%s", p);

    logf("socktrace: loaded (prefix=%s)\n", g_prefix[0]?g_prefix:"<none>");
}

// 언로드 시(소멸자)
__attribute__((destructor))
static void fini(void){
    logf("socktrace: bye\n");
}

// 5-튜플(로컬/피어) 한 줄로 출력
static void print_5tuple(int fd, const char* tag){
    char la[96], pa[96];
    fmt_addr(la, sizeof(la), &M[fd].local);
    fmt_addr(pa, sizeof(pa), &M[fd].peer);
    logf("%s fd=%d  local=%s  peer=%s\n", tag, fd, la, pa);
}

// -------------------- 후킹 함수들 --------------------

// connect: 클라이언트가 서버로 접속 시도
int connect(int fd, const struct sockaddr* addr, socklen_t len){
    if (!real_connect) real_connect = dlsym(RTLD_NEXT, "connect");
    int r = real_connect(fd, addr, len);
    pthread_mutex_lock(&g_lock);
    ensure_fd(fd);
    if (r==0 && M[fd].is_socket){
        refresh_endpoints(fd);          // 연결 성공 후 최신 주소 반영
        print_5tuple(fd, "[connect ok]");
    } else if (r<0) {
        logf("[connect err] fd=%d errno=%d\n", fd, errno);
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

// accept: 서버가 새 연결 수락
int accept(int fd, struct sockaddr* addr, socklen_t* len){
    if (!real_accept) real_accept = dlsym(RTLD_NEXT, "accept");
    int cfd = real_accept(fd, addr, len);
    if (cfd>=0){
        pthread_mutex_lock(&g_lock);
        ensure_fd(cfd);                 // 새 FD 메타 준비
        refresh_endpoints(cfd);         // 로컬/피어 주소 기록
        print_5tuple(cfd, "[accept ok]");
        pthread_mutex_unlock(&g_lock);
    }
    return cfd;
}

// send: TCP/UDP 송신 (flags는 그대로 전달)
ssize_t send(int fd, const void* buf, size_t cnt, int flags){
    if (!real_send) real_send = dlsym(RTLD_NEXT, "send");
    double t = now_s();
    ssize_t n = real_send(fd, buf, cnt, flags);
    if (n>=0){
        pthread_mutex_lock(&g_lock);
        ensure_fd(fd);
        if (M[fd].is_socket){
            refresh_endpoints(fd);
            char pa[96]; fmt_addr(pa, sizeof(pa), &M[fd].peer);
            logf("[send]  t=%.6f fd=%d bytes=%zd peer=%s\n", t, fd, n, pa);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return n;
}

// recv: TCP/UDP 수신
ssize_t recv(int fd, void* buf, size_t cnt, int flags){
    if (!real_recv) real_recv = dlsym(RTLD_NEXT, "recv");
    double t = now_s();
    ssize_t n = real_recv(fd, buf, cnt, flags);
    if (n>=0){
        pthread_mutex_lock(&g_lock);
        ensure_fd(fd);
        if (M[fd].is_socket){
            refresh_endpoints(fd);
            char pa[96]; fmt_addr(pa, sizeof(pa), &M[fd].peer);
            logf("[recv]  t=%.6f fd=%d bytes=%zd peer=%s\n", t, fd, n, pa);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return n;
}

// read: 소켓인지 확인하고 소켓이면 로깅(read를 쓰는 앱 호환)
ssize_t read(int fd, void* buf, size_t cnt){
    if (!real_read) real_read = dlsym(RTLD_NEXT, "read");
    double t = now_s();
    ssize_t n = real_read(fd, buf, cnt);
    if (n>=0){
        pthread_mutex_lock(&g_lock);
        ensure_fd(fd);
        if (M[fd].is_socket){
            refresh_endpoints(fd);
            char pa[96]; fmt_addr(pa, sizeof(pa), &M[fd].peer);
            logf("[read]  t=%.6f fd=%d bytes=%zd peer=%s\n", t, fd, n, pa);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return n;
}

// write: 소켓이면 로깅(write를 쓰는 앱 호환)
ssize_t write(int fd, const void* buf, size_t cnt){
    if (!real_write) real_write = dlsym(RTLD_NEXT, "write");
    double t = now_s();
    ssize_t n = real_write(fd, buf, cnt);
    if (n>=0){
        pthread_mutex_lock(&g_lock);
        ensure_fd(fd);
        if (M[fd].is_socket){
            refresh_endpoints(fd);
            char pa[96]; fmt_addr(pa, sizeof(pa), &M[fd].peer);
            logf("[write] t=%.6f fd=%d bytes=%zd peer=%s\n", t, fd, n, pa);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return n;
}
