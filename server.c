// gcc server.c -o server -pthread


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 12345
#define BUFFER_SIZE 256
#define MAX_CLIENTS 100

int client_sockets[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t lock;

void *handle_client(void *arg) {
    int client_fd = *(int *)arg;
    char buffer[BUFFER_SIZE];

    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client_fd, buffer, BUFFER_SIZE, 0);
        if (bytes_received <= 0) {
            printf("[서버] 클라이언트 %d 연결 종료\n", client_fd);
            close(client_fd);
            pthread_mutex_lock(&lock);
            for (int i = 0; i < client_count; ++i) {
                if (client_sockets[i] == client_fd) {
                    client_sockets[i] = client_sockets[client_count - 1];
                    client_count--;
                    break;
                }
            }
            pthread_mutex_unlock(&lock);
            return NULL;
        }

        printf("[서버] Received from %d: %s\n", client_fd, buffer); // 로깅 추가

        // 🔄 모든 클라이언트에게 브로드캐스트
        pthread_mutex_lock(&lock);
        for (int i = 0; i < client_count; ++i) {
            if (client_sockets[i] != client_fd) {
                int sent_bytes = send(client_sockets[i], buffer, strlen(buffer), 0);
                if (sent_bytes < 0) {
                    perror("[서버] send error");
                }
            }
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[서버] socket creation failed");
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[서버] bind failed");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("[서버] listen failed");
        close(server_fd);
        return 1;
    }

    printf("[서버] 그림 공유 서버가 실행 중입니다...\n");

    pthread_mutex_init(&lock, NULL);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("[서버] accept failed");
            continue;
        }
        pthread_mutex_lock(&lock);
        if (client_count < MAX_CLIENTS) {
            client_sockets[client_count++] = client_fd;
            printf("[서버] 새 클라이언트 연결: %d\n", client_fd);
            pthread_t thread;
            if (pthread_create(&thread, NULL, handle_client, &client_fd) != 0) {
                perror("[서버] 스레드 생성 실패");
                close(client_fd);
                client_count--;
            } else {
                pthread_detach(thread); // 스레드 메모리 자동 해제
            }
        } else {
            printf("[서버] 최대 클라이언트 수 초과. 연결 거부: %d\n", client_fd);
            close(client_fd);
        }
        pthread_mutex_unlock(&lock);
    }

    close(server_fd);
    pthread_mutex_destroy(&lock);
    return 0;
}
