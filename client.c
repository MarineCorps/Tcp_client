#include <stdio.h>          // 표준 입출력 함수 (printf, fgets 등)
#include <string.h>         // 문자열 처리 함수 (strlen, strncmp, memset 등)
#include <unistd.h>         // POSIX 시스템 호출 함수 (read, write, close)
#include <arpa/inet.h>      // 네트워크 관련 함수 (sockaddr_in, inet_pton, htons 등)

int main() {
    // 1. 소켓 생성
    // AF_INET : IPv4 주소 체계
    // SOCK_STREAM : TCP (연결 지향 스트림 소켓)
    // 0 : 프로토콜 자동 선택 (TCP로 자동 설정)
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    // 2. 서버 주소 구조체 설정
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;          // 주소 체계: IPv4
    server_addr.sin_port = htons(8888);        // 포트 번호를 네트워크 바이트 순서로 변환 (big endian)

    // 3. 문자열 IP 주소를 네트워크용 이진 주소로 변환
    // 실제 서버의 IP 주소로 대체 필요 (예: "192.168.0.10")
    inet_pton(AF_INET, "115.145.211.117", &server_addr.sin_addr);

    // 4. 서버에 연결 요청 (3-way handshake)
    connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    printf("클라이언트: 서버에 연결됨\n");

    // 5. 송수신 버퍼 선언
    char send_buf[1024];    // 사용자 입력 저장용
    char recv_buf[1024];    // 서버 응답 저장용

    // 6. 메시지 송수신 루프
    while (1) {
        // 사용자에게 입력 요청
        printf("입력 > ");

        // 사용자로부터 한 줄 입력 받음 (공백 포함, 최대 1023글자)
        fgets(send_buf, sizeof(send_buf), stdin);

        // fgets는 개행문자(\n)를 포함하므로, 이를 제거
        send_buf[strcspn(send_buf, "\n")] = '\0';

        // 입력한 메시지를 서버로 전송
        write(sock, send_buf, strlen(send_buf));

        // 종료 명령 처리
        if (strncmp(send_buf, "exit", 4) == 0)
            break;  // "exit" 입력 시 루프 탈출 → 연결 종료

        // 서버 응답 수신
        memset(recv_buf, 0, sizeof(recv_buf));  // 이전 데이터 초기화
        read(sock, recv_buf, sizeof(recv_buf)); // 서버로부터 응답 수신
        printf("서버 응답 > %s\n", recv_buf);    // 응답 출력
    }

    // 7. 연결 종료
    close(sock);   // 클라이언트 소켓 종료 (TCP FIN 패킷 전송)
    return 0;
}

