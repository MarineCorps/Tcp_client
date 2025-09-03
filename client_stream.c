
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>

#define BLOCK_SIZE 4096  // 블록 단위로 데이터 수신

int main() {
    // 1. 소켓 생성
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    // 2. 서버 주소 설정
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8888);
    inet_pton(AF_INET, "115.145.211.117", &server_addr.sin_addr);  // 서버 IP 설정

    // 3. 서버에 연결 요청
    // 3. 서버에 연결 요청
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("서버 연결 실패");
        close(sock);
        exit(1);  // 또는 return 1;
    }
    printf("클라이언트: 서버에 연결됨\n");


    char send_buf[1024];  // 사용자 요청 입력 버퍼

    while (1) {
        printf("입력 > ");
        fgets(send_buf, sizeof(send_buf), stdin);
        send_buf[strcspn(send_buf, "\n")] = '\0';  // 개행 제거

        if (strncmp(send_buf, "exit", 4) == 0) {
            write(sock, send_buf, strlen(send_buf));
            break;
        }

        // 요청 크기 해석
        int mb = atoi(send_buf);
        if (mb <= 0 || mb > 100) {
            printf("클라이언트: 잘못된 요청\n");
            continue;
        }

        int size = mb * 1024 * 1024;  // 바이트 단위로 변환
        char block[BLOCK_SIZE];       // 수신용 블록 버퍼
        int received = 0;

        printf("클라이언트: %d MB 요청\n", mb);

        // 4. 파일 이름 생성
        // 파일 이름은 요청 크기에 따라 다르게 설정
        char filename[64];
        snprintf(filename, sizeof(filename), "received_%dMB.bin", mb);  // 파일
        // 5. 저장할 파일 열기
        FILE *fp = fopen(filename, "wb"); 
        //wb(write binary) 모드로 파일 열기, 파일이 없으면 새로 생성, 있으면 덮어쓰기
        //ab(append) 모드 기존 파일이있으면 끝에추가
        if (!fp) {
            perror("파일 열기 실패");
            break;
        }

        // 6. 다운로드 시간 측정 (요청 → 수신 완료까지)
        struct timeval start, end;
        gettimeofday(&start, NULL);  // 요청 직전 시간 측정
        write(sock, send_buf, strlen(send_buf)); // 요청 전송

        // 7. 블록 단위로 수신하여 파일에 저장
        while (received < size) {
            int n = read(sock, block, BLOCK_SIZE);
            if (n <= 0) break;
            fwrite(block, 1, n, fp);  // 받은 만큼만 저장
            received += n;
        }

        gettimeofday(&end, NULL);  // 다운로드 완료 시간 측정
        fclose(fp);  // 파일 닫기

        //  경과 시간 계산  tv_sec: 초 단위, tv_usec: 마이크로초 단위
        // 초단위와 마이크로초 단위를 합산하여 전체 경과 시간을 계산
        double elapsed = (end.tv_sec - start.tv_sec)
                       + (end.tv_usec - start.tv_usec) / 1000000.0;

        //  평균 속도 계산
        double mb_received = received / (1024.0 * 1024.0); // 받은 데이터량 (MB 단위)
        double speed = mb_received / elapsed; //평균속도(MB/s 단위)

        //  결과 출력
        printf(" 다운로드 완료: %.2f MB (%d 바이트)\n", mb_received, received);
        printf("⏱ 소요 시간: %.6f 초\n", elapsed);
        printf(" 평균 속도: %.2f MB/s\n", speed);
    }

    close(sock);  // 소켓 종료
    return 0;
}
