#ifdef RUNTIME
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>      // read, write
#include <string.h>
#include <errno.h>
#include <dlfcn.h>       // dlsym, RTLD_NEXT
#include <sys/types.h>
#include <sys/socket.h>  // getsockopt, SOL_SOCKET, SO_TYPE
#include <time.h>        // clock_gettime

// ===== 설정 =====
static int  g_interval_ms = 250;     // 로그 주기(ms)
static char g_prefix[64]  = "";      // 로그 접두사

// ===== 원함수 포인터 =====
static ssize_t (*real_read)(int, void*, size_t)         = NULL;
static ssize_t (*real_write)(int, const void*, size_t)  = NULL;

// ===== 추적 대상: 단 하나의 소켓 FD =====
typedef struct {
  int          tracked_fd;     // 추적 중인 소켓 FD (-1이면 미설정)
  uint64_t     in_bytes;       // 누적 수신
  uint64_t     out_bytes;      // 누적 송신
  struct timespec t0;          // 시작 시각
  struct timespec last_report; // 마지막 보고 시각
  int          active;         // t0 설정 여부
} one_stat_t;

static one_stat_t S = { .tracked_fd = -1, .active = 0 };

// ===== 유틸 =====
static inline void now(struct timespec* ts){ clock_gettime(CLOCK_MONOTONIC, ts); }
static inline double secdiff(const struct timespec* a, const struct timespec* b){
  return (a->tv_sec - b->tv_sec) + (a->tv_nsec - b->tv_nsec)/1e9;
}
static inline long ms_since(const struct timespec* a, const struct timespec* b){
  return (long)(secdiff(a,b) * 1000.0);
}
static int is_socket_fd(int fd){
  int type; socklen_t len = sizeof(type);
  return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0;
}
static void safe_log(const char* s, size_t n){
  if (!real_write) return;
  (void)real_write(STDERR_FILENO, s, n); // 재귀 방지: 원 write로 직접 출력
}
static void maybe_pick_fd_and_start(int fd){
  if (S.tracked_fd != -1) return;            // 이미 선택됨
  if (!is_socket_fd(fd))   return;            // 소켓이 아니면 무시
  S.tracked_fd = fd;                          // 이 소켓을 추적
  S.in_bytes = S.out_bytes = 0;
  memset(&S.last_report, 0, sizeof(S.last_report));
  now(&S.t0);
  S.active = 1;
}
static void maybe_report(void){
  if (!S.active || S.tracked_fd == -1) return;
  struct timespec t; now(&t);
  if ((S.last_report.tv_sec || S.last_report.tv_nsec) &&
      ms_since(&t, &S.last_report) < g_interval_ms) return;
  S.last_report = t;

  double el = secdiff(&t, &S.t0); if (el <= 0) el = 1e-9;
  double inMB  = S.in_bytes  / (1024.0*1024.0);
  double outMB = S.out_bytes / (1024.0*1024.0);

  char line[256];
  int n = snprintf(line, sizeof(line),
      "%s[fd=%d] %.2fs  IN: %.2f MB (%.2f MB/s)  OUT: %.2f MB (%.2f MB/s)\n",
      g_prefix, S.tracked_fd, el,
      inMB,  inMB/el,
      outMB, outMB/el);
  if (n > 0) safe_log(line, (size_t)n);
}

// ===== 초기화/해제 =====
static void init_once(void){
  const char* p = getenv("NETPROF_INTERVAL_MS");
  if (p && *p) { int v = atoi(p); if (v > 0) g_interval_ms = v; }
  const char* pref = getenv("NETPROF_PREFIX");
  if (pref && *pref) snprintf(g_prefix, sizeof(g_prefix), "%s", pref);

  char line[128];
  int n = snprintf(line, sizeof(line),
      "%snetprof(min): interval=%dms active\n", g_prefix, g_interval_ms);
  if (n > 0) safe_log(line, (size_t)n);
}

__attribute__((constructor))
static void ctor(void){
  char *err;
  real_read  = dlsym(RTLD_NEXT, "read");  
  if ((err = dlerror()) != NULL)
  { 
    fputs(err, stderr); exit(1); 
  }
  real_write = dlsym(RTLD_NEXT, "write"); 
  
  if ((err = dlerror()) != NULL)
  { 
    fputs(err, stderr); exit(1); 
  }
  init_once();
}

__attribute__((destructor))
static void dtor(void){
  if (!S.active || S.tracked_fd == -1) return;
  struct timespec t; now(&t);
  double el = secdiff(&t, &S.t0); if (el <= 0) el = 1e-9;
  double inMB  = S.in_bytes  / (1024.0*1024.0);
  double outMB = S.out_bytes / (1024.0*1024.0);
  char line[256];
  int n = snprintf(line, sizeof(line),
      "%s[fd=%d] FINAL  T=%.2fs  IN: %.2f MB (%.2f MB/s)  OUT: %.2f MB (%.2f MB/s)\n",
      g_prefix, S.tracked_fd, el,
      inMB,  inMB/el,
      outMB, outMB/el);
  if (n > 0) safe_log(line, (size_t)n);
}

// ===== 후킹 함수 =====  
ssize_t read(int fd, void* buf, size_t count){
  if (!real_read){ 
    char *err; 
    real_read = dlsym(RTLD_NEXT, "read"); 
    if ((err = dlerror()) != NULL)
    { 
      fputs(err, stderr); errno = EIO; return -1; 
    } 
  }
  ssize_t n = real_read(fd, buf, count);
  if (n > 0){
    if (S.tracked_fd == -1) maybe_pick_fd_and_start(fd);
    if (fd == S.tracked_fd){
      S.in_bytes += (uint64_t)n;
      maybe_report();
    }
  }
  return n;
}

ssize_t write(int fd, const void* buf, size_t count){
  if (!real_write){ char *err; real_write = dlsym(RTLD_NEXT, "write"); 
    if ((err = dlerror()) != NULL)
    { 
      fputs(err, stderr); errno = EIO; return -1;
    } 
  }
  ssize_t n = real_write(fd, buf, count);
  if (n > 0){
    if (S.tracked_fd == -1) maybe_pick_fd_and_start(fd);
    if (fd == S.tracked_fd){
      S.out_bytes += (uint64_t)n;
      maybe_report();
    }
  }
  return n;
}
#endif /* RUNTIME */
