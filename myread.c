#ifdef RUNTIME
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>

/* read wrapper function */
ssize_t read(int fd, void *buf, size_t count)
{
    // 원래 read 함수의 주소를 담을 함수 포인터
    ssize_t (*readp)(int, void *, size_t);
    char *error;

    // 상태 저장 변수
    static int z_detected = 0;   // Z를 한 번이라도 감지했는가?
    static int z_not_found_printed = 0; // Z 없음 출력 했는가?

    // 실제 read 주소 획득
    readp = dlsym(RTLD_NEXT, "read");
    if ((error = dlerror()) != NULL) {
        fputs(error, stderr);
        exit(1);
    }

    // read 호출
    ssize_t n = readp(fd, buf, count);

    // 읽은 데이터가 있을 경우 검사
    if (n > 0) {
        char *p = memchr(buf, 'Z', n);

        if (p && !z_detected) {
            // 처음으로 Z를 발견한 경우에만 출력
            fprintf(stderr, "[interpose] read(fd=%d, count=%zu) → %zd bytes: 'Z' 최초 감지!\n", fd, count, n);
            char *end = (char *)buf + n;
            while (p && p < end) {
                fprintf(stderr, "  → 'Z' 위치: %p (오프셋: %ld)\n", p, p - (char *)buf);
                p = memchr(p + 1, 'Z', end - p - 1);
            }
            z_detected = 1;  // 이후에는 감지해도 출력하지 않음
        }

        if (!p && !z_not_found_printed && !z_detected) {
            // Z가 없고, 아직 "Z 없음"을 출력한 적 없다면 한 번만 출력
            fprintf(stderr, "[interpose] read(fd=%d, count=%zu) → %zd bytes: Z 없음\n", fd, count, n);
            z_not_found_printed = 1;
        }
    }

    return n;
}
#endif
