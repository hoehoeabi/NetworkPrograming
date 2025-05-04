#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 12345
#define MAX_CLIENTS 10
#define BUFFER_SIZE 4096 

int clients[MAX_CLIENTS];
int client_count = 0;
char answer[50] = "사과";  // 정답 (임의 설정)

// 클라이언트 메시지 브로드캐스트 함수
void broadcast(char *message, int sender) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i] != sender) {
            send(clients[i], message, strlen(message), 0);
        }
    }
}

// 클라이언트 핸들러 스레드
void *handle_client(void *arg) {
    int client_fd = *(int *)arg;
    char buffer[BUFFER_SIZE];

    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client_fd, buffer, BUFFER_SIZE, 0);
        if (bytes_received <= 0) {
            break;
        }

        printf("클라이언트 메시지: %s\n", buffer);

        // 정답 확인
        if (strcmp(buffer, answer) == 0) {
            char msg[BUFFER_SIZE];
            sprintf(msg, "정답이 맞았습니다! (%s)\n", buffer);
            broadcast(msg, client_fd);
        } else {
            broadcast(buffer, client_fd);
        }
    }

    // 클라이언트 연결 해제
    close(client_fd);
    return NULL;
}

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_size = sizeof(client_addr);

    // 1. 소켓 생성
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("소켓 생성 실패");
        exit(1);
    }

    // 2. 서버 주소 설정
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // 3. 바인딩
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("바인딩 실패");
        exit(1);
    }

    // 4. 연결 대기
    listen(server_fd, MAX_CLIENTS);
    printf("멀티플레이어 그림 맞추기 서버 실행 중...\n");

    while (client_count < MAX_CLIENTS) {
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_size);
        if (client_fd < 0) {
            perror("클라이언트 연결 실패");
            exit(1);
        }

        // 클라이언트 저장 및 스레드 생성
        clients[client_count++] = client_fd;
        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, &client_fd);
    }

    close(server_fd);
    return 0;
}

