// gcc client2.c -o client2 `sdl2-config --cflags --libs` -pthread

// 클라이언트 (client.c)
#include "SDL2/SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 12345
#define SERVER_IP "127.0.0.1"
#define WINDOW_WIDTH 600
#define WINDOW_HEIGHT 600
#define BUFFER_SIZE 256
#define RENDER_DELAY 16

SDL_Color colors[] = {
    {0, 0, 0, 255},     // Black
    {255, 0, 0, 255},   // Red
    {0, 255, 0, 255},   // Green
    {0, 0, 255, 255},   // Blue
    {255, 255, 255, 255} // White
};

int current_color_index = 0;
int pen_size = 3;

int client_fd;
SDL_Renderer *renderer;
SDL_Texture *canvas;
pthread_mutex_t lock;
SDL_bool need_redraw = SDL_FALSE;

// 수신한 그림 데이터를 저장하는 구조체
typedef struct {
    int x1, y1, x2, y2, color_index, size;
} DrawData;

// 동적 배열로 수신한 그림 데이터 저장
DrawData *received_lines = NULL;
int received_lines_count = 0;
int received_lines_capacity = 10; // 초기 용량
pthread_mutex_t received_lines_lock;

void draw_button(SDL_Renderer *renderer, int x, int y, int size, SDL_Color color) {
    SDL_Rect rect = {x, y, size, size};
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
    SDL_RenderFillRect(renderer, &rect);
}

int check_button_click(int x, int y, int button_x, int button_y, int size) {
    return (x >= button_x && x <= button_x + size && y >= button_y && y <= button_y + size);
}

void clear_canvas() {
    SDL_SetRenderTarget(renderer, canvas);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderTarget(renderer, NULL);
}

void add_received_line(int x1, int y1, int x2, int y2, int color_index, int size) {
    pthread_mutex_lock(&received_lines_lock);
    if (received_lines_count >= received_lines_capacity) {
        received_lines_capacity *= 2;
        received_lines = realloc(received_lines, received_lines_capacity * sizeof(DrawData));
        if (received_lines == NULL) {
            perror("[클라이언트 오류] received_lines 메모리 재할당 실패");
            exit(1);
        }
    }
    received_lines[received_lines_count].x1 = x1;
    received_lines[received_lines_count].y1 = y1;
    received_lines[received_lines_count].x2 = x2;
    received_lines[received_lines_count].y2 = y2;
    received_lines[received_lines_count].color_index = color_index;
    received_lines[received_lines_count].size = size;
    received_lines_count++;
    pthread_mutex_unlock(&received_lines_lock);
    need_redraw = SDL_TRUE; // 새로운 데이터가 도착했으므로 다시 그리기 요청
}

void *recv_data(void *arg) {
    char buffer[BUFFER_SIZE];
    char *token;
    char *saveptr;
    int bytes_received;

    while ((bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        printf("[클라이언트 수신] %s", buffer);

        token = strtok_r(buffer, "\n", &saveptr);
        while (token != NULL) {
            int x1, y1, x2, y2, color_index, size;
            int parsed = sscanf(token, "%d,%d,%d,%d,%d,%d", &x1, &y1, &x2, &y2, &color_index, &size);

            if (parsed == 6) {
                printf("[클라이언트 파싱] 수신: x1=%d, y1=%d, x2=%d, y2=%d, color=%d, size=%d\n", x1, y1, x2, y2, color_index, size);
                if (color_index >= 0 && color_index < sizeof(colors) / sizeof(colors[0])) {
                    add_received_line(x1, y1, x2, y2, color_index, size);
                } else {
                    fprintf(stderr, "[클라이언트 오류] 잘못된 색상 인덱스 수신: %d\n", color_index);
                }
            } else if (strcmp(token, "CLEAR") == 0) {
                pthread_mutex_lock(&lock);
                clear_canvas();
                pthread_mutex_unlock(&lock);
                need_redraw = SDL_TRUE;
            } else if (parsed > 0) {
                fprintf(stderr, "[클라이언트 오류] 잘못된 데이터 형식 수신: %s\n", token);
            }
            token = strtok_r(NULL, "\n", &saveptr);
        }
    }
    return NULL;
}

void redraw_canvas() {
    SDL_SetRenderTarget(renderer, canvas);
    pthread_mutex_lock(&received_lines_lock);
    for (int i = 0; i < received_lines_count; ++i) {
        SDL_SetRenderDrawColor(renderer, colors[received_lines[i].color_index].r, colors[received_lines[i].color_index].g, colors[received_lines[i].color_index].b, 255);
        for (int j = 0; j < received_lines[i].size; ++j) {
            SDL_RenderDrawLine(renderer, received_lines[i].x1 + j, received_lines[i].y1, received_lines[i].x2 + j, received_lines[i].y2);
            SDL_RenderDrawLine(renderer, received_lines[i].x1, received_lines[i].y1 + j, received_lines[i].x2, received_lines[i].y2 + j);
        }
    }
    pthread_mutex_unlock(&received_lines_lock);
    SDL_SetRenderTarget(renderer, NULL);
}

int main() {
    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window *window = SDL_CreateWindow("🎨 그림을 맞춰봐!!", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    canvas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, WINDOW_WIDTH, WINDOW_HEIGHT);
    SDL_SetRenderTarget(renderer, canvas);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderTarget(renderer, NULL);
    SDL_RenderPresent(renderer);

    client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd == -1) {
        perror("클라이언트 소켓 생성 실패");
        exit(1);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    if (connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("서버 연결 실패");
        close(client_fd);
        exit(1);
    }

    pthread_t recv_thread;
    pthread_mutex_init(&lock, NULL);
    pthread_mutex_init(&received_lines_lock, NULL);
    pthread_create(&recv_thread, NULL, recv_data, NULL);

    // 동적 배열 초기화
    received_lines = malloc(received_lines_capacity * sizeof(DrawData));
    if (received_lines == NULL) {
        perror("[클라이언트 오류] received_lines 초기 메모리 할당 실패");
        exit(1);
    }
    received_lines_count = 0;

    SDL_Event event;
    int running = 1;
    int drawing = 0;
    int last_x = -1, last_y = -1;
    char buffer[BUFFER_SIZE];

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                int x, y;
                SDL_GetMouseState(&x, &y);

                for (int i = 0; i < 5; i++) {
                    if (check_button_click(x, y, 10 + i * 40, 10, 30)) {
                        current_color_index = i;
                        printf("[클라이언트 UI] 색상 변경: %d\n", current_color_index);
                    }
                }

                if (check_button_click(x, y, 10, 50, 30)) {
                    pen_size = 2;
                    printf("[클라이언트 UI] 펜 굵기 변경: %d\n", pen_size);
                }
                if (check_button_click(x, y, 50, 50, 30)) {
                    pen_size = 5;
                    printf("[클라이언트 UI] 펜 굵기 변경: %d\n", pen_size);
                }
                if (check_button_click(x, y, 90, 50, 30)) {
                    pen_size = 10;
                    printf("[클라이언트 UI] 펜 굵기 변경: %d\n", pen_size);
                }

                if (check_button_click(x, y, 10, 90, 80)) {
                    snprintf(buffer, BUFFER_SIZE, "CLEAR\n");
                    send(client_fd, buffer, strlen(buffer), 0);
                    printf("[클라이언트 송신] CLEAR\n");
                    pthread_mutex_lock(&lock);
                    clear_canvas();
                    pthread_mutex_unlock(&lock);
                    need_redraw = SDL_TRUE;
                }

                drawing = 1;
                last_x = x;
                last_y = y;
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                drawing = 0;
                last_x = -1;
                last_y = -1;
            } else if (event.type == SDL_MOUSEMOTION && drawing) {
                int x, y;
                SDL_GetMouseState(&x, &y);

                pthread_mutex_lock(&lock);
                SDL_SetRenderTarget(renderer, canvas);
                SDL_SetRenderDrawColor(renderer, colors[current_color_index].r, colors[current_color_index].g, colors[current_color_index].b, 255);
                for (int i = 0; i < pen_size; ++i) {
                    SDL_RenderDrawLine(renderer, last_x + i, last_y, x + i, y);
                    SDL_RenderDrawLine(renderer, last_x, last_y + i, x, y + i);
                }
                SDL_SetRenderTarget(renderer, NULL);
                need_redraw = SDL_TRUE;
                pthread_mutex_unlock(&lock);

                snprintf(buffer, BUFFER_SIZE, "%d,%d,%d,%d,%d,%d\n", last_x, last_y, x, y, current_color_index, pen_size);
                send(client_fd, buffer, strlen(buffer), 0);
                printf("[클라이언트 송신] %d,%d,%d,%d,%d,%d\n", last_x, last_y, x, y, current_color_index, pen_size);

                last_x = x;
                last_y = y;
            }
        }

        SDL_SetRenderTarget(renderer, NULL);
        SDL_RenderCopy(renderer, canvas, NULL, NULL);

        for (int i = 0; i < 5; i++) {
            draw_button(renderer, 10 + i * 40, 10, 30, colors[i]);
        }
        draw_button(renderer, 10, 50, 30, (SDL_Color){0, 0, 0, 255});
        draw_button(renderer, 50, 50, 30, (SDL_Color){128, 128, 128, 255});
        draw_button(renderer, 90, 50, 30, (SDL_Color){192, 192, 192, 255});
        SDL_Rect clear_button = {10, 90, 80, 30};
        SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
        SDL_RenderFillRect(renderer, &clear_button);

        if (need_redraw) {
            pthread_mutex_lock(&lock);
            redraw_canvas(); // 수신한 데이터를 기반으로 캔버스 다시 그리기
            SDL_RenderPresent(renderer);
            need_redraw = SDL_FALSE;
            pthread_mutex_unlock(&lock);
        } else {
            SDL_Delay(RENDER_DELAY);
        }
    }

    close(client_fd);
    SDL_DestroyTexture(canvas);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    pthread_mutex_destroy(&lock);
    pthread_mutex_destroy(&received_lines_lock);
    free(received_lines); // 동적 배열 메모리 해제
    return 0;
}
