// gcc client2.c -o client2 `sdl2-config --cflags --libs` -pthread

#include "SDL2/SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <math.h> // For sqrt in thicker line drawing if needed

#define PORT 12345
#define SERVER_IP "127.0.0.1"
#define WINDOW_WIDTH 600
#define WINDOW_HEIGHT 600
#define BUFFER_SIZE 256

// 메시지 타입 정의
#define MSG_TYPE_DOWN 1
#define MSG_TYPE_MOVE 0
#define MSG_TYPE_CLEAR_SCREEN 2 // CLEAR 메시지 타입을 숫자로 정의 (선택 사항)

SDL_Color colors[] = {
    {0, 0, 0, 255},     // Black
    {255, 0, 0, 255},   // Red
    {0, 255, 0, 255},   // Green
    {0, 0, 255, 255},   // Blue
    {255, 255, 255, 255} // White (Eraser)
};

int current_color_index = 0;
int pen_size = 3;

int client_fd;
SDL_Renderer *renderer;
SDL_Texture *canvas_texture; // 그림을 그릴 텍스처

// UI를 다시 그릴 필요가 있는지 나타내는 플래그
int needs_ui_redraw = 1;


void draw_thick_line(SDL_Renderer *target_renderer, int x1, int y1, int x2, int y2, int thickness, SDL_Color color) {
    SDL_SetRenderDrawColor(target_renderer, color.r, color.g, color.b, color.a);
    // 매우 간단한 두꺼운 선 그리기: 여러 개의 평행선 또는 사각형 채우기
    // SDL_gfx와 같은 라이브러리를 사용하면 더 나은 품질의 두꺼운 선을 그릴 수 있습니다.
    // 여기서는 간단하게 여러 SDL_RenderDrawLine을 사용하거나, 각 점에 원을 그립니다.

    int dx = x2 - x1;
    int dy = y2 - y1;
    double distance = sqrt(dx*dx + dy*dy);
    if (distance == 0) distance = 1; // 0으로 나누기 방지

    // 각 점에 원(사각형)을 그리는 방식
    for (int i = 0; i < distance; ++i) {
        double t = (double)i / distance;
        int cx = x1 + (int)(t * dx);
        int cy = y1 + (int)(t * dy);
        SDL_Rect fillRect = { cx - thickness / 2, cy - thickness / 2, thickness, thickness };
        SDL_RenderFillRect(target_renderer, &fillRect);
    }
    // 마지막 점에도 그림
    SDL_Rect fillRect = { x2 - thickness / 2, y2 - thickness / 2, thickness, thickness };
    SDL_RenderFillRect(target_renderer, &fillRect);
}


void draw_point_on_renderer(SDL_Renderer* target_renderer, int x, int y, int size, SDL_Color color) {
    SDL_SetRenderDrawColor(target_renderer, color.r, color.g, color.b, 255);
    SDL_Rect rect = {x - size / 2, y - size / 2, size, size};
    SDL_RenderFillRect(target_renderer, &rect);
}

void draw_button(SDL_Renderer *local_renderer, int x, int y, int size, SDL_Color color) {
    SDL_Rect rect = {x, y, size, size};
    SDL_SetRenderDrawColor(local_renderer, color.r, color.g, color.b, 255);
    SDL_RenderFillRect(local_renderer, &rect);
}

int check_button_click(int x, int y, int button_x, int button_y, int size) {
    return (x >= button_x && x <= button_x + size && y >= button_y && y <= button_y + size);
}

// UI 그리기 함수
void draw_ui(SDL_Renderer *local_renderer) {
    // 색상 버튼 그리기
    for (int i = 0; i < 5; i++) {
        draw_button(local_renderer, 10 + i * 40, 10, 30, colors[i]);
    }

    // 펜 크기 버튼 (예시 색상, 실제로는 선택된 크기를 시각적으로 표시하는 것이 좋음)
    draw_button(local_renderer, 10, 50, 30, (SDL_Color){200, 200, 200, 255}); // Small
    draw_button(local_renderer, 50, 50, 30, (SDL_Color){150, 150, 150, 255}); // Medium
    draw_button(local_renderer, 90, 50, 30, (SDL_Color){100, 100, 100, 255}); // Large

    // 전체 지우기 버튼
    SDL_Rect clear_button_rect = {10, 90, 80, 30};
    SDL_SetRenderDrawColor(local_renderer, 220, 220, 220, 255);
    SDL_RenderFillRect(local_renderer, &clear_button_rect);
    // 여기에 "지우기" 텍스트를 추가할 수 있습니다.
}


void *recv_data(void *arg) {
    char buffer[BUFFER_SIZE];
    int remote_last_x = -1, remote_last_y = -1; // 서버로부터 받은 좌표를 위한 last_x, last_y

    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client_fd, buffer, BUFFER_SIZE -1, 0); // -1 for null terminator
        if (bytes_received <= 0) {
            printf("서버로부터 연결 끊김 또는 오류.\n");
            // TODO: 여기서 프로그램 종료 또는 재연결 로직을 넣을 수 있습니다.
            // running = 0; // main 스레드의 running을 직접 제어하기는 어려움. 콜백이나 플래그 사용
            break;
        }
        buffer[bytes_received] = '\0'; // Null terminate received data

        // "CLEAR" 메시지 처리
        if (strcmp(buffer, "CLEAR") == 0) {
            SDL_SetRenderTarget(renderer, canvas_texture); // 캔버스 텍스처에 그리기 설정
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255); // 흰색으로 지우기
            SDL_RenderClear(renderer);
            SDL_SetRenderTarget(renderer, NULL); // 기본 렌더 타겟으로 복원
            remote_last_x = -1;
            remote_last_y = -1;
            // needs_ui_redraw = 1; // UI를 다시 그릴 필요는 없음, 캔버스만 지움
            continue; // 다음 메시지 처리
        }

        int type, x, y, color_idx, point_size;
        // 메시지 포맷: "type,x,y,color_index,size"
        if (sscanf(buffer, "%d,%d,%d,%d,%d", &type, &x, &y, &color_idx, &point_size) == 5) {
            if (color_idx < 0 || color_idx >= (sizeof(colors)/sizeof(colors[0]))) {
                fprintf(stderr, "잘못된 색상 인덱스 수신: %d\n", color_idx);
                continue;
            }

            SDL_SetRenderTarget(renderer, canvas_texture); // 캔버스 텍스처에 그리기 설정

            if (type == MSG_TYPE_DOWN) { // 새로운 선 시작
                remote_last_x = -1; // 이전 좌표 초기화
            }

            if (remote_last_x != -1 && remote_last_y != -1) {
                // SDL_RenderDrawLine(renderer, remote_last_x, remote_last_y, x, y);
                draw_thick_line(renderer, remote_last_x, remote_last_y, x, y, point_size, colors[color_idx]);
            } else {
                // SDL_Rect rect = {x - point_size / 2, y - point_size / 2, point_size, point_size};
                // SDL_SetRenderDrawColor(renderer, colors[color_idx].r, colors[color_idx].g, colors[color_idx].b, 255);
                // SDL_RenderFillRect(renderer, &rect);
                draw_point_on_renderer(renderer, x, y, point_size, colors[color_idx]);
            }
            remote_last_x = x;
            remote_last_y = y;

            SDL_SetRenderTarget(renderer, NULL); // 기본 렌더 타겟으로 복원
        } else {
            fprintf(stderr, "잘못된 형식의 메시지 수신: %s\n", buffer);
        }
    }
    return NULL;
}

int main(int argc, char* argv[]) { // main 함수 시그니처 수정
    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window *window = SDL_CreateWindow("🎨 그림을 맞춰봐!!", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    // 그림을 그릴 텍스처 생성 (캔버스 역할)
    canvas_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, WINDOW_WIDTH, WINDOW_HEIGHT);
    SDL_SetRenderTarget(renderer, canvas_texture);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255); // 캔버스를 흰색으로 초기화
    SDL_RenderClear(renderer);
    SDL_SetRenderTarget(renderer, NULL); // 기본 렌더 타겟으로 복원

    client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        perror("소켓 생성 실패");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    if (connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("서버 연결 실패");
        close(client_fd);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, recv_data, NULL) != 0) {
        perror("수신 스레드 생성 실패");
        close(client_fd);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Event event;
    int running = 1;
    int drawing = 0;
    char send_buffer[BUFFER_SIZE]; // send 함수에 사용될 버퍼 이름 변경

    // 로컬 드로잉을 위한 이전 좌표 (macOS 문제 해결용)
    int local_prev_x = -1;
    int local_prev_y = -1;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    int x, y;
                    SDL_GetMouseState(&x, &y);

                    // UI 버튼 클릭 확인
                    int ui_clicked = 0;
                    for (int i = 0; i < 5; i++) { // 색상 버튼
                        if (check_button_click(x, y, 10 + i * 40, 10, 30)) {
                            current_color_index = i;
                            ui_clicked = 1;
                            break;
                        }
                    }
                    if (!ui_clicked) { // 펜 크기 버튼
                        if (check_button_click(x, y, 10, 50, 30)) { pen_size = 3; ui_clicked = 1; }
                        else if (check_button_click(x, y, 50, 50, 30)) { pen_size = 8; ui_clicked = 1; }
                        else if (check_button_click(x, y, 90, 50, 30)) { pen_size = 15; ui_clicked = 1; }
                    }
                     if (!ui_clicked) { // 전체 지우기 버튼
                        if (check_button_click(x, y, 10, 90, 80)) { // 버튼 영역 확인
                            // 로컬 캔버스 지우기
                            SDL_SetRenderTarget(renderer, canvas_texture);
                            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                            SDL_RenderClear(renderer);
                            SDL_SetRenderTarget(renderer, NULL);
                            local_prev_x = -1; // 로컬 이전 좌표 초기화
                            local_prev_y = -1;

                            // 서버에 "CLEAR" 메시지 전송
                            snprintf(send_buffer, BUFFER_SIZE, "CLEAR");
                            send(client_fd, send_buffer, strlen(send_buffer), 0);
                            ui_clicked = 1;
                        }
                    }


                    if (!ui_clicked && y > 130) { // UI 영역(대략 y=130 위)이 아닌 곳을 클릭했을 때만 그리기 시작
                        drawing = 1;
                        local_prev_x = -1; // 새 선 시작 시 이전 좌표 초기화 (macOS 문제 해결)

                        // 현재 점을 그림 (로컬 캔버스에)
                        SDL_SetRenderTarget(renderer, canvas_texture);
                        // draw_point_on_renderer(renderer, x, y, pen_size, colors[current_color_index]);
                        draw_thick_line(renderer, x,y,x,y, pen_size, colors[current_color_index]); // 점은 시작과 끝이 같은 선으로 표현
                        SDL_SetRenderTarget(renderer, NULL);

                        // 서버에 첫 번째 점 전송 (MSG_TYPE_DOWN)
                        snprintf(send_buffer, BUFFER_SIZE, "%d,%d,%d,%d,%d", MSG_TYPE_DOWN, x, y, current_color_index, pen_size);
                        send(client_fd, send_buffer, strlen(send_buffer), 0);
                        local_prev_x = x;
                        local_prev_y = y;
                    }
                }
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    drawing = 0;
                    local_prev_x = -1; // 선 그리기 종료 시 이전 좌표 초기화
                    local_prev_y = -1;
                }
            } else if (event.type == SDL_MOUSEMOTION && drawing) {
                int x, y;
                SDL_GetMouseState(&x, &y);

                if (local_prev_x != -1 && local_prev_y != -1) {
                    // 로컬 캔버스에 선 그리기
                    SDL_SetRenderTarget(renderer, canvas_texture);
                    // SDL_SetRenderDrawColor(renderer, colors[current_color_index].r, colors[current_color_index].g, colors[current_color_index].b, 255);
                    // SDL_RenderDrawLine(renderer, local_prev_x, local_prev_y, x, y); // 기본 1픽셀 라인
                    draw_thick_line(renderer, local_prev_x, local_prev_y, x, y, pen_size, colors[current_color_index]);
                    SDL_SetRenderTarget(renderer, NULL);

                    // 서버에 이어지는 점 전송 (MSG_TYPE_MOVE)
                    snprintf(send_buffer, BUFFER_SIZE, "%d,%d,%d,%d,%d", MSG_TYPE_MOVE, x, y, current_color_index, pen_size);
                    send(client_fd, send_buffer, strlen(send_buffer), 0);
                }
                local_prev_x = x;
                local_prev_y = y;
            }
        }

        // 화면 지우기 (기본 배경색 - 여기서는 흰색으로 가정)
        SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255); // 바깥 배경색 (회색톤)
        SDL_RenderClear(renderer);

        // 캔버스 텍스처를 화면에 복사
        SDL_RenderCopy(renderer, canvas_texture, NULL, NULL);

        // UI 그리기
        draw_ui(renderer); // UI는 매 프레임 다시 그림 (상태 변경이 있을 수 있으므로)

        SDL_RenderPresent(renderer); // 최종 화면 표시 (프레임 당 한 번)
    }

    close(client_fd);
    pthread_join(recv_thread, NULL); // 스레드가 정상적으로 종료될 때까지 대기 (선택 사항)
    SDL_DestroyTexture(canvas_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
