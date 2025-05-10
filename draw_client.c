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

SDL_Color colors[] = {
    {0, 0, 0, 255},
    {255, 0, 0, 255},
    {0, 255, 0, 255},
    {0, 0, 255, 255},
    {255, 255, 255, 255}
};

int current_color_index = 0;
int pen_size = 3;

int client_fd;
SDL_Renderer *renderer;

void draw_button(SDL_Renderer *renderer, int x, int y, int size, SDL_Color color) {
    SDL_Rect rect = {x, y, size, size};
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
    SDL_RenderFillRect(renderer, &rect);
}

int check_button_click(int x, int y, int button_x, int button_y, int size) {
    return (x >= button_x && x <= button_x + size && y >= button_y && y <= button_y + size);
}

void *recv_data(void *arg) {
    char buffer[BUFFER_SIZE];
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client_fd, buffer, BUFFER_SIZE, 0);
        if (bytes_received <= 0) break;

        int x, y, color_index, size;
        sscanf(buffer, "%d,%d,%d,%d", &x, &y, &color_index, &size);
        SDL_SetRenderDrawColor(renderer, colors[color_index].r, colors[color_index].g, colors[color_index].b, 255);
        SDL_Rect rect = {x - size / 2, y - size / 2, size, size};
        SDL_RenderFillRect(renderer, &rect);
        SDL_RenderPresent(renderer);
    }
    return NULL;
}

int main() {
    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window *window = SDL_CreateWindow("🎨 그림을 맞춰봐!!", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);

    client_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    pthread_t recv_thread;
    pthread_create(&recv_thread, NULL, recv_data, NULL);

    SDL_Event event;
    int running = 1;
    int drawing = 0;
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
                    }
                }

                if (check_button_click(x, y, 10, 50, 30)) pen_size = 2;
                if (check_button_click(x, y, 50, 50, 30)) pen_size = 5;
                if (check_button_click(x, y, 90, 50, 30)) pen_size = 10;

                if (check_button_click(x, y, 10, 90, 80)) {
                    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                    SDL_RenderClear(renderer);
                    SDL_RenderPresent(renderer);
                }

                drawing = 1;
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                drawing = 0;
            } else if (event.type == SDL_MOUSEMOTION && drawing) {
                int x, y;
                SDL_GetMouseState(&x, &y);
                SDL_SetRenderDrawColor(renderer, colors[current_color_index].r, colors[current_color_index].g, colors[current_color_index].b, 255);
                SDL_Rect rect = {x - pen_size / 2, y - pen_size / 2, pen_size, pen_size};
                SDL_RenderFillRect(renderer, &rect);
                SDL_RenderPresent(renderer);

                snprintf(buffer, BUFFER_SIZE, "%d,%d,%d,%d", x, y, current_color_index, pen_size);
                send(client_fd, buffer, strlen(buffer), 0);
            }
        }

        for (int i = 0; i < 5; i++) {
            draw_button(renderer, 10 + i * 40, 10, 30, colors[i]);
        }

        draw_button(renderer, 10, 50, 30, (SDL_Color){0, 0, 0, 255});
        draw_button(renderer, 50, 50, 30, (SDL_Color){128, 128, 128, 255});
        draw_button(renderer, 90, 50, 30, (SDL_Color){192, 192, 192, 255});

        SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
        SDL_Rect clear_button = {10, 90, 80, 30};
        SDL_RenderFillRect(renderer, &clear_button);
        SDL_RenderPresent(renderer);
    }

    close(client_fd);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
