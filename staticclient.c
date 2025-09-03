#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8888);
    inet_pton(AF_INET, "115.145.211.117", &server_addr.sin_addr);

    connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    printf("클라이언트: 서버에 연결됨\n");

    char send_buf[1024], recv_buf[4096];

    while (1) {
        printf("입력 > ");
        fgets(send_buf, sizeof(send_buf), stdin);
        send_buf[strcspn(send_buf, "\n")] = '\0';

        write(sock, send_buf, strlen(send_buf));

        if (strncmp(send_buf, "exit", 4) == 0)
            break;

        int mb = atoi(send_buf);
        if (mb <= 0 || mb > 100) {
            printf("클라이언트: 잘못된 요청 크기\n");
            continue;
        }

        int size = mb * 1024 * 1024; //원하는 크기 (MB 단위)
        printf("클라이언트: %d MB 요청\n", mb);
        char *data = malloc(size); // 원하는 데이터크기만큼의 메모리 할당
        int received = 0;

        while (received < size) {
            int n = read(sock, data + received, size - received);
            if (n <= 0) break;
            received += n;
        }

        printf("클라이언트: 총 %d 바이트 수신 완료\n", received);
        printf("앞부분: %.16s\n", data);  // 더미 데이터 확인용
        free(data);
    }

    close(sock);
    return 0;
}
//read는 커널의 TCP 수신버퍼에서
//데이터를 읽어오는 함수로, 실제로는 커널에서 TCP 세그먼트를 처리하여 
//수신 버퍼에 저장된 데이터를 사용자 공간으로 복사합니다.