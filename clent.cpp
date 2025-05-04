#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 6000
#define BUF_SIZE 256
//깃허브 연결 테스트
int main() {
    int sock;
    struct sockaddr_in serv_addr;
    char buffer[BUF_SIZE];

    sock = socket(AF_INET, SOCK_STREAM, 0);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    while (1) {
        int len = recv(sock, buffer, BUF_SIZE, 0);
        if (len <= 0) break;

        printf("Client 수신: %s\n", buffer);

        if (strcmp(buffer, "exit") == 0)
            break;
    }

    close(sock);
    return 0;
}
