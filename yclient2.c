// gcc yclient2.c -o yclient2 -pthread $(sdl2-config --cflags --libs) -lSDL2_ttf
#include "SDL2/SDL.h"
#include "sdl2_ttf/2.24.0/include/SDL2/SDL_ttf.h" // SDL_ttf 사용을 위한 헤더 (선택 사항: UI 내 텍스트 표시용)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include <errno.h>

// 서버 및 버퍼 설정
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 12345
#define BUFFER_SIZE 1024
#define NICK_SIZE 32
#define ROOM_NAME_SIZE 32
#define INPUT_BUFFER_SIZE (BUFFER_SIZE * 2)

// SDL UI 설정
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define UI_AREA_HEIGHT 80


SDL_Color sdl_colors[] = {
    {0, 0, 0, 255}, {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}
};
int current_sdl_color_index = 0;
int current_pen_size = 5;

// 클라이언트 상태
typedef enum {
    STATE_LOBBY,
    STATE_IN_ROOM
} ClientAppState;

volatile ClientAppState app_current_state = STATE_LOBBY;
int client_socket_fd;
char client_nickname[NICK_SIZE] = "익명";
volatile int client_is_nick_set = 0;
int client_current_room_id = -1;
char client_current_room_name[ROOM_NAME_SIZE] = "";

// 서버 메시지 수신 버퍼
char server_message_buffer[INPUT_BUFFER_SIZE];
int server_message_len = 0;

// SDL 관련 전역 변수
SDL_Window* app_sdl_window = NULL;
SDL_Renderer* app_sdl_renderer = NULL;
SDL_Texture* app_drawing_canvas_texture = NULL;
TTF_Font* app_ui_font = NULL;
volatile int sdl_should_be_active = 0;
volatile int sdl_render_loop_running = 0;
volatile int main_program_should_run = 1;

// 로컬 드로잉용 변수
int local_mouse_is_drawing = 0;
int local_mouse_last_x = -1;
int local_mouse_last_y = -1;

// 수신한 그림 데이터를 저장하는 구조체 (client2.c 참고)
typedef struct {
    int type; // 0: POINT, 1: LINE, 2: CLEAR
    int x1, y1, x2, y2, color_index, size;
} ReceivedDrawData;

ReceivedDrawData *received_draw_commands = NULL;
int received_draw_commands_count = 0;
int received_draw_commands_capacity = 20; // 초기 용량
pthread_mutex_t received_draw_commands_lock; // 이 뮤텍스 추가

// 스레드
pthread_t server_message_receiver_tid;

// --- 함수 프로토타입 ---
void send_formatted_message_to_server(const char* type_or_full_cmd, const char* payload);
void* server_message_receiver_thread_func(void* arg);
void initialize_sdl_environment();
void run_sdl_main_event_loop(); // 이 함수는 이전 버전에 있었음
void shutdown_sdl_environment();
void render_sdl_ui_elements();
void handle_sdl_mouse_click_on_ui(int x, int y);
// apply_drawing_data_from_server 함수는 이제 run_sdl_main_event_loop 내부에서 직접 처리되므로 삭제 또는 주석처리 가능
// void apply_drawing_data_from_server(const char* full_draw_message_line);
void client_side_clear_canvas_and_notify_server();

// --- 메시지 전송 함수 ---
void send_formatted_message_to_server(const char* type_or_full_cmd, const char* payload) {
    char final_message_to_send[BUFFER_SIZE];
    if (payload && strlen(payload) > 0) {
        snprintf(final_message_to_send, BUFFER_SIZE, "%s:%s\n", type_or_full_cmd, payload);
    } else {
        snprintf(final_message_to_send, BUFFER_SIZE, "%s\n", type_or_full_cmd);
    }
    if (send(client_socket_fd, final_message_to_send, strlen(final_message_to_send), 0) < 0) {
        perror("send to server failed");
    }
}

// --- 서버 메시지 수신 스레드 ---
void* server_message_receiver_thread_func(void* arg) {
    char temp_chunk_buffer[BUFFER_SIZE];
    int bytes_read;
    printf("[클라이언트:수신스레드] 시작됨.\n");

    while (main_program_should_run) {
        bytes_read = recv(client_socket_fd, temp_chunk_buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read <= 0) {
            if (bytes_read == 0) printf("\r[클라이언트:수신스레드] 서버 연결 정상 종료됨.\n> ");
            else perror("\r[클라이언트:수신스레드] 서버 메시지 수신 오류");
            main_program_should_run = 0;
            if(sdl_render_loop_running) sdl_should_be_active = 0;
            break;
        }
        temp_chunk_buffer[bytes_read] = '\0';

        if (server_message_len + bytes_read < INPUT_BUFFER_SIZE) {
            strncat(server_message_buffer, temp_chunk_buffer, bytes_read);
            server_message_len += bytes_read;
        } else {
            fprintf(stderr, "\r[클라이언트:수신스레드] 수신 버퍼 오버플로우.\n> ");
            server_message_len = 0; memset(server_message_buffer, 0, INPUT_BUFFER_SIZE);
            continue;
        }

        char* current_line_ptr = server_message_buffer;
        char* newline_char_found;
        while ((newline_char_found = strchr(current_line_ptr, '\n')) != NULL) {
            *newline_char_found = '\0';
            char server_response_line[INPUT_BUFFER_SIZE];
            strcpy(server_response_line, current_line_ptr);

            // 터미널 출력 필터링
            if (strncmp(server_response_line, "MSG:", 4) == 0) {
                printf("\r%s\n> ", server_response_line + 4); // "MSG:" 제거하고 [닉네임] 메시지 형식으로 출력
            } else if (strncmp(server_response_line, "DRAW_", 5) != 0) { // DRAW_로 시작하는 메시지는 출력 안 함
                 if (strncmp(server_response_line, "SMSG:방", 7) == 0 || strncmp(server_response_line, "SMSG:닉네임이", 10) ==0) {
                     printf("\r[클라이언트 시스템] %s\n> ", server_response_line);
                 } else if (strncmp(server_response_line, "ROOM_EVENT:", 11) == 0 ) {
                     printf("\r%s\n> ", server_response_line + 11); // ROOM_EVENT: 접두어 제거
                 } else if (strncmp(server_response_line, "SMSG:", 5) == 0 || strncmp(server_response_line, "ERR_", 4) == 0 || strncmp(server_response_line, "ROOMLIST:", 9) == 0 ) {
                     printf("\r[서버 응답] %s\n> ", server_response_line);
                 }
                 // 기타 다른 형식의 서버 메시지가 있다면 여기에 조건 추가
            }
            fflush(stdout);


            if (strncmp(server_response_line, "SMSG:", 5) == 0) {
                const char* smsg_payload_content = server_response_line + 5;
                char temp_room_name[ROOM_NAME_SIZE];
                int temp_room_id;

                if (strstr(smsg_payload_content, "닉네임이 설정되었습니다.") || strstr(smsg_payload_content, "(으)로 설정되었습니다.")) {
                    char confirmed_nickname_str[NICK_SIZE];
                    if (sscanf(smsg_payload_content, "닉네임이 '%[^']'(으)로 설정되었습니다.", confirmed_nickname_str) == 1) {
                        strcpy(client_nickname, confirmed_nickname_str);
                        client_is_nick_set = 1;
                        // 아래 시스템 메시지로 이미 출력되므로 중복 printf 제거
                        // printf("\r[클라이언트:수신스레드] === 상태 변경: 닉네임 '%s' 설정 (client_is_nick_set = %d) ===\n> ", client_nickname, client_is_nick_set);
                    } else {
                         // printf("\r[클라이언트:수신스레드] 닉네임 설정 응답 파싱 실패: %s\n> ", smsg_payload_content);
                    }
                } else if (sscanf(smsg_payload_content, "방 '%[^']'(ID:%d)에 입장했습니다.", temp_room_name, &temp_room_id) == 2 ||
                           sscanf(smsg_payload_content, "방 '%[^']'(ID:%d)이(가) 생성되었고 입장했습니다.", temp_room_name, &temp_room_id) == 2) {

                    strcpy(client_current_room_name, temp_room_name);
                    client_current_room_id = temp_room_id;
                    // printf("\r[클라이언트:수신스레드] 방 '%s'(ID:%d) 입장/생성 정보 파싱 성공.\n> ", client_current_room_name, client_current_room_id);

                    app_current_state = STATE_IN_ROOM;
                    // printf("\r[클라이언트:수신스레드] === 상태 변경: app_current_state = STATE_IN_ROOM (%d) ===\n> ", app_current_state);

                    if (client_is_nick_set) {
                        sdl_should_be_active = 1;
                        // printf("\r[클라이언트:수신스레드] === 상태 변경: SDL UI 활성화 요청 (sdl_should_be_active = %d) ===\n> ", sdl_should_be_active);
                    } else {
                        // printf("\r[클라이언트:수신스레드] 경고: 방 진입 성공했으나, client_is_nick_set이 false. SDL UI 보류.\n> ");
                    }
                } else if (strstr(smsg_payload_content, "방에서 퇴장했습니다.")) {
                    // printf("\r[클라이언트:수신스레드] === 상태 변경: 방 퇴장 ===\n> ");
                    app_current_state = STATE_LOBBY;
                    client_current_room_id = -1;
                    strcpy(client_current_room_name, "");
                    if (sdl_render_loop_running) sdl_should_be_active = 0; // SDL 루프 종료 유도
                    // printf("\r[클라이언트:수신스레드] 로비로 돌아감 (app_current_state = STATE_LOBBY, sdl_should_be_active = %d)\n> ", sdl_should_be_active);
                }
            } else if (strncmp(server_response_line, "ERR_NICK_TAKEN:", 15) == 0) {
                // 이미 [서버 응답]으로 출력됨
            } else if (strncmp(server_response_line, "ERR_NICK_REQUIRED:", 18) == 0) {
                // 이미 [서버 응답]으로 출력됨
            } else if (strncmp(server_response_line, "DRAW_POINT:", 11) == 0) {
                ReceivedDrawData data = {0};
                data.type = 0;
                if (sscanf(server_response_line + 11, "%d,%d,%d,%d", &data.x1, &data.y1, &data.color_index, &data.size) == 4) {
                    pthread_mutex_lock(&received_draw_commands_lock);
                    if (received_draw_commands_count >= received_draw_commands_capacity) {
                        received_draw_commands_capacity *= 2;
                        received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
                         if (!received_draw_commands) { perror("realloc draw point failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; break;}
                    }
                    received_draw_commands[received_draw_commands_count++] = data;
                    pthread_mutex_unlock(&received_draw_commands_lock);
                }
            } else if (strncmp(server_response_line, "DRAW_LINE:", 10) == 0) {
                ReceivedDrawData data = {0};
                data.type = 1;
                if (sscanf(server_response_line + 10, "%d,%d,%d,%d,%d,%d", &data.x1, &data.y1, &data.x2, &data.y2, &data.color_index, &data.size) == 6) {
                    pthread_mutex_lock(&received_draw_commands_lock);
                    if (received_draw_commands_count >= received_draw_commands_capacity) {
                         received_draw_commands_capacity *= 2;
                         received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
                         if (!received_draw_commands) { perror("realloc draw line failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; break;}
                    }
                    received_draw_commands[received_draw_commands_count++] = data;
                    pthread_mutex_unlock(&received_draw_commands_lock);
                }
            } else if (strcmp(server_response_line, "DRAW_CLEAR") == 0) {
                ReceivedDrawData data = {0};
                data.type = 2;
                pthread_mutex_lock(&received_draw_commands_lock);
                 if (received_draw_commands_count >= received_draw_commands_capacity) {
                    received_draw_commands_capacity *= 2;
                    received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
                    if (!received_draw_commands) { perror("realloc draw clear failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; break;}
                }
                received_draw_commands[received_draw_commands_count++] = data;
                pthread_mutex_unlock(&received_draw_commands_lock);
            }
            current_line_ptr = newline_char_found + 1;
        }
        if (current_line_ptr > server_message_buffer && strlen(current_line_ptr) > 0) {
            char temp_buf[INPUT_BUFFER_SIZE]; strcpy(temp_buf, current_line_ptr);
            memset(server_message_buffer, 0, INPUT_BUFFER_SIZE); strcpy(server_message_buffer, temp_buf);
            server_message_len = strlen(server_message_buffer);
        } else {
            server_message_len = 0; memset(server_message_buffer, 0, INPUT_BUFFER_SIZE);
        }
    }
    printf("[클라이언트:수신스레드] 종료됨.\n");
    return NULL;
}

void initialize_sdl_environment() {
    if (sdl_render_loop_running || app_sdl_window) {
        // printf("[클라이언트:SDL] 초기화 시도: 이미 실행 중이거나 윈도우 존재 (sdl_render_loop_running=%d, app_sdl_window=%p)\n", sdl_render_loop_running, (void*)app_sdl_window);
        return;
    }
    printf("[클라이언트:SDL] UI 초기화 시작 (방: '%s', 닉네임: '%s')\n", client_current_room_name, client_nickname);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL Init 실패: %s\n", SDL_GetError()); main_program_should_run = 0; return; }
    if (TTF_Init() == -1) { fprintf(stderr, "TTF Init 실패: %s (계속 진행)\n", TTF_GetError()); }

    char win_title[128];
    snprintf(win_title, sizeof(win_title), "그림판 - 방: %s (%s)", client_current_room_name, client_nickname);
    app_sdl_window = SDL_CreateWindow(win_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    if (!app_sdl_window) { fprintf(stderr, "윈도우 생성 실패: %s\n", SDL_GetError()); main_program_should_run = 0; SDL_Quit(); return; }

    app_sdl_renderer = SDL_CreateRenderer(app_sdl_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!app_sdl_renderer) { SDL_DestroyWindow(app_sdl_window); app_sdl_window=NULL; fprintf(stderr, "렌더러 생성 실패: %s\n", SDL_GetError()); main_program_should_run = 0; SDL_Quit(); return; }

    app_drawing_canvas_texture = SDL_CreateTexture(app_sdl_renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, WINDOW_WIDTH, WINDOW_HEIGHT - UI_AREA_HEIGHT);
    if (!app_drawing_canvas_texture) { fprintf(stderr, "캔버스 텍스처 생성 실패: %s\n", SDL_GetError()); shutdown_sdl_environment(); main_program_should_run = 0; return; }

    SDL_SetRenderTarget(app_sdl_renderer, app_drawing_canvas_texture);
    SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 255, 255);
    SDL_RenderClear(app_sdl_renderer);
    SDL_SetRenderTarget(app_sdl_renderer, NULL);

    sdl_render_loop_running = 1; // 여기서 루프 실행 상태로 변경
    printf("[클라이언트:SDL] UI 초기화 완료, 루프 실행 준비됨 (sdl_render_loop_running = %d).\n", sdl_render_loop_running);
}

void shutdown_sdl_environment() {
    if (!app_sdl_window && !app_sdl_renderer && !app_drawing_canvas_texture && !sdl_render_loop_running) {
        return;
    }
    printf("[클라이언트:SDL] UI 종료 시작...\n");
    if (app_drawing_canvas_texture) { SDL_DestroyTexture(app_drawing_canvas_texture); app_drawing_canvas_texture = NULL; /*printf("  캔버스 텍스처 해제됨.\n");*/}
    if (app_sdl_renderer) { SDL_DestroyRenderer(app_sdl_renderer); app_sdl_renderer = NULL; /*printf("  렌더러 해제됨.\n");*/}
    if (app_sdl_window) { SDL_DestroyWindow(app_sdl_window); app_sdl_window = NULL; /*printf("  윈도우 해제됨.\n");*/}
    if (app_ui_font) { TTF_CloseFont(app_ui_font); app_ui_font = NULL; /*printf("  폰트 해제됨.\n");*/}
    
    sdl_render_loop_running = 0;
    sdl_should_be_active = 0;
    printf("[클라이언트:SDL] UI 자원 해제 완료 (sdl_render_loop_running = %d, sdl_should_be_active = %d).\n", sdl_render_loop_running, sdl_should_be_active);
}

void render_sdl_ui_elements() {
    if (!app_sdl_renderer) return;
    SDL_Rect ui_bg_rect = {0, 0, WINDOW_WIDTH, UI_AREA_HEIGHT};
    SDL_SetRenderDrawColor(app_sdl_renderer, 220, 220, 220, 255);
    SDL_RenderFillRect(app_sdl_renderer, &ui_bg_rect);
    for (int i = 0; i < 5; i++) {
        SDL_Rect btn = {10 + i * 45, 10, 40, 30};
        SDL_SetRenderDrawColor(app_sdl_renderer, sdl_colors[i].r, sdl_colors[i].g, sdl_colors[i].b, 255);
        SDL_RenderFillRect(app_sdl_renderer, &btn);
        if (i == current_sdl_color_index) {
            SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 0, 255); SDL_RenderDrawRect(app_sdl_renderer, &btn);
        }
    }
    int pen_sizes_options[] = {3, 8, 15};
    for (int i = 0; i < 3; i++) {
        SDL_Rect btn = {10 + i * 45, 45, 40, 30};
        SDL_SetRenderDrawColor(app_sdl_renderer, 180 - i*10, 180 - i*10, 180 - i*10, 255);
        SDL_RenderFillRect(app_sdl_renderer, &btn);
        if (pen_sizes_options[i] == current_pen_size) {
            SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 0, 255); SDL_RenderDrawRect(app_sdl_renderer, &btn);
        }
    }
    SDL_Rect clear_btn = {WINDOW_WIDTH - 90, 10, 80, 30};
    SDL_SetRenderDrawColor(app_sdl_renderer, 230, 80, 80, 255);
    SDL_RenderFillRect(app_sdl_renderer, &clear_btn);
}

void handle_sdl_mouse_click_on_ui(int x, int y) {
    for (int i = 0; i < 5; i++) {
        if (x >= 10 + i * 45 && x <= 10 + i * 45 + 40 && y >= 10 && y <= 10 + 30) {
            current_sdl_color_index = i; return;
        }
    }
    int pen_sizes_opt[] = {3, 8, 15};
    for (int i = 0; i < 3; i++) {
        if (x >= 10 + i * 45 && x <= 10 + i * 45 + 40 && y >= 45 && y <= 45 + 30) {
            current_pen_size = pen_sizes_opt[i]; return;
        }
    }
    if (x >= WINDOW_WIDTH - 90 && x <= WINDOW_WIDTH - 10 && y >= 10 && y <= 10 + 30) {
        client_side_clear_canvas_and_notify_server(); return;
    }
}

void client_side_clear_canvas_and_notify_server() {
    if (!app_drawing_canvas_texture || !app_sdl_renderer) return;
    
    // 로컬 버퍼에 CLEAR 명령 추가
    ReceivedDrawData data = {0};
    data.type = 2; // CLEAR
    pthread_mutex_lock(&received_draw_commands_lock);
    if (received_draw_commands_count >= received_draw_commands_capacity) {
        received_draw_commands_capacity *= 2;
        received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
        if (!received_draw_commands) { perror("realloc clear cmd failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; return;}
    }
    if(received_draw_commands) received_draw_commands[received_draw_commands_count++] = data;
    pthread_mutex_unlock(&received_draw_commands_lock);
    
    send_formatted_message_to_server("DRAW_CLEAR", NULL); // 서버에 알림
}

// run_sdl_main_event_loop 함수는 이전 답변에서 main 함수에 통합되었으므로 여기서는 주석 처리합니다.
// 만약 별도 함수로 유지한다면, 이 함수의 내용을 이전 답변의 main 함수 SDL 처리 부분으로 대체해야 합니다.
/*
void run_sdl_main_event_loop() {
    // 이 함수의 내용은 이전 답변의 main 함수 내 SDL 처리 로직으로 대체되었습니다.
    // 만약 이 함수를 계속 사용하려면, 이전 답변의 main 함수 내 SDL_PollEvent 루프와
    // received_draw_commands 처리, 렌더링 로직을 이 함수 안으로 옮겨야 합니다.
    // 그리고 main 함수에서는 이 함수를 호출하는 부분을 되돌려야 합니다.
    // 현재 제공된 코드에는 이 함수가 main 함수에 통합되어 있습니다.
}
*/

int main(int argc, char* argv[]) {
    struct sockaddr_in server_address;
    client_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket_fd < 0) { perror("socket"); exit(1); }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_address.sin_addr) <= 0) {
        perror("inet_pton"); close(client_socket_fd); exit(1);
    }
    if (connect(client_socket_fd, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        perror("connect"); close(client_socket_fd); exit(1);
    }
    printf("[클라이언트:메인] 서버 연결 성공 (IP: %s, Port: %d)\n", SERVER_IP, SERVER_PORT);

    if (pthread_create(&server_message_receiver_tid, NULL, server_message_receiver_thread_func, NULL) != 0) {
        perror("수신 스레드 생성 실패"); close(client_socket_fd); exit(1);
    }

    pthread_mutex_init(&received_draw_commands_lock, NULL);
    
    received_draw_commands = malloc(received_draw_commands_capacity * sizeof(ReceivedDrawData));
    if (received_draw_commands == NULL) {
        perror("[클라이언트:메인] received_draw_commands 메모리 할당 실패");
        exit(1);
    }
    char terminal_input_line[BUFFER_SIZE];
    printf("> "); fflush(stdout);
    
    SDL_Event sdl_event_handler;
    Uint32 sdl_frame_start_tick;

    while (main_program_should_run) {
            if (app_current_state == STATE_IN_ROOM && client_is_nick_set && sdl_should_be_active) {
                if (!sdl_render_loop_running) {
                    // printf("[클라이언트:메인] SDL UI 시작 조건 충족. 초기화 시도...\n"); // initialize_sdl_environment 내부에서 로그 출력
                    initialize_sdl_environment();
                    if (!sdl_render_loop_running) {
                        printf("[클라이언트:메인] SDL 초기화 실패. sdl_render_loop_running = %d\n", sdl_render_loop_running);
                        sdl_should_be_active = 0;
                        if(app_current_state == STATE_IN_ROOM) {
                            send_formatted_message_to_server("EXIT_ROOM", NULL);
                        }
                    } else {
                        // printf("[클라이언트:SDL루프] === 그래픽 루프 로직 시작 (방: '%s', 닉네임: '%s') ===\n", client_current_room_name, client_nickname); // initialize_sdl_environment 내부에서 로그 출력
                    }
                }

                if (sdl_render_loop_running) {
                    sdl_frame_start_tick = SDL_GetTicks();

                    while (SDL_PollEvent(&sdl_event_handler)) {
                        if (sdl_event_handler.type == SDL_QUIT) {
                            printf("[클라이언트:SDL이벤트] 종료 이벤트 (X 버튼). EXIT_ROOM 전송.\n");
                            send_formatted_message_to_server("EXIT_ROOM", NULL);
                            // sdl_should_be_active = 0; // 서버 응답을 기다려 상태 변경
                            break;
                        }
                        if (sdl_event_handler.type == SDL_MOUSEBUTTONDOWN && sdl_event_handler.button.button == SDL_BUTTON_LEFT) {
                            int m_x = sdl_event_handler.button.x; int m_y = sdl_event_handler.button.y;
                            if (m_y < UI_AREA_HEIGHT) { handle_sdl_mouse_click_on_ui(m_x, m_y); }
                            else {
                                local_mouse_is_drawing = 1; local_mouse_last_x = m_x; local_mouse_last_y = m_y;
                                char point_payload[BUFFER_SIZE];
                                snprintf(point_payload, BUFFER_SIZE, "%d,%d,%d,%d", m_x, m_y, current_sdl_color_index, current_pen_size);
                                send_formatted_message_to_server("DRAW_POINT", point_payload);
                                
                                // 로컬 버퍼에도 추가
                                ReceivedDrawData data = {0}; data.type = 0; data.x1 = m_x; data.y1 = m_y;
                                data.color_index = current_sdl_color_index; data.size = current_pen_size;
                                pthread_mutex_lock(&received_draw_commands_lock);
                                if (received_draw_commands_count >= received_draw_commands_capacity) {
                                    received_draw_commands_capacity *= 2;
                                    received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
                                    if (!received_draw_commands) { perror("realloc draw point cmd failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; break;}
                                }
                                if(received_draw_commands) received_draw_commands[received_draw_commands_count++] = data;
                                pthread_mutex_unlock(&received_draw_commands_lock);
                            }
                        } else if (sdl_event_handler.type == SDL_MOUSEMOTION && local_mouse_is_drawing) {
                            int m_x = sdl_event_handler.motion.x; int m_y = sdl_event_handler.motion.y;
                            if (m_y >= UI_AREA_HEIGHT) {
                                char line_payload[BUFFER_SIZE];
                                snprintf(line_payload, BUFFER_SIZE, "%d,%d,%d,%d,%d,%d", local_mouse_last_x, local_mouse_last_y, m_x, m_y, current_sdl_color_index, current_pen_size);
                                send_formatted_message_to_server("DRAW_LINE", line_payload);
                                
                                // 로컬 버퍼에도 추가
                                ReceivedDrawData data = {0}; data.type = 1; data.x1 = local_mouse_last_x; data.y1 = local_mouse_last_y;
                                data.x2 = m_x; data.y2 = m_y; data.color_index = current_sdl_color_index; data.size = current_pen_size;
                                pthread_mutex_lock(&received_draw_commands_lock);
                                if (received_draw_commands_count >= received_draw_commands_capacity) {
                                   received_draw_commands_capacity *= 2;
                                   received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
                                   if (!received_draw_commands) { perror("realloc draw line cmd failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; break;}
                                }
                                if(received_draw_commands) received_draw_commands[received_draw_commands_count++] = data;
                                pthread_mutex_unlock(&received_draw_commands_lock);

                                local_mouse_last_x = m_x; local_mouse_last_y = m_y;
                            }
                        } else if (sdl_event_handler.type == SDL_MOUSEBUTTONUP && sdl_event_handler.button.button == SDL_BUTTON_LEFT) {
                            local_mouse_is_drawing = 0; local_mouse_last_x = -1; local_mouse_last_y = -1;
                        }
                    }

                    pthread_mutex_lock(&received_draw_commands_lock);
                    if (received_draw_commands_count > 0) {
                        SDL_SetRenderTarget(app_sdl_renderer, app_drawing_canvas_texture);
                        for (int i = 0; i < received_draw_commands_count; i++) {
                            ReceivedDrawData cmd = received_draw_commands[i];
                            if (cmd.type == 0) { // POINT
                                SDL_SetRenderDrawColor(app_sdl_renderer, sdl_colors[cmd.color_index].r, sdl_colors[cmd.color_index].g, sdl_colors[cmd.color_index].b, 255);
                                SDL_Rect pr = {cmd.x1 - cmd.size/2, (cmd.y1 - UI_AREA_HEIGHT) - cmd.size/2, cmd.size, cmd.size};
                                SDL_RenderFillRect(app_sdl_renderer, &pr);
                            } else if (cmd.type == 1) { // LINE
                                SDL_SetRenderDrawColor(app_sdl_renderer, sdl_colors[cmd.color_index].r, sdl_colors[cmd.color_index].g, sdl_colors[cmd.color_index].b, 255);
                                for (int dx = -cmd.size/2; dx <= cmd.size/2; ++dx) {
                                    for (int dy = -cmd.size/2; dy <= cmd.size/2; ++dy) {
                                        if(dx*dx + dy*dy <= (cmd.size/2)*(cmd.size/2) +1 ){
                                            SDL_RenderDrawLine(app_sdl_renderer, cmd.x1 + dx, (cmd.y1 - UI_AREA_HEIGHT) + dy, cmd.x2 + dx, (cmd.y2 - UI_AREA_HEIGHT) + dy);
                                        }
                                    }
                                }
                            } else if (cmd.type == 2) { // CLEAR
                                SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 255, 255);
                                SDL_RenderClear(app_sdl_renderer);
                            }
                        }
                        SDL_SetRenderTarget(app_sdl_renderer, NULL);
                        received_draw_commands_count = 0;
                    }
                    pthread_mutex_unlock(&received_draw_commands_lock);

                    SDL_SetRenderDrawColor(app_sdl_renderer, 50, 50, 50, 255);
                    SDL_RenderClear(app_sdl_renderer);
                    SDL_Rect canvas_display_rect = {0, UI_AREA_HEIGHT, WINDOW_WIDTH, WINDOW_HEIGHT - UI_AREA_HEIGHT};
                    SDL_RenderCopy(app_sdl_renderer, app_drawing_canvas_texture, NULL, &canvas_display_rect);
                    render_sdl_ui_elements();
                    SDL_RenderPresent(app_sdl_renderer);

                    Uint32 frame_elapsed_time = SDL_GetTicks() - sdl_frame_start_tick;
                    if (frame_elapsed_time < 16) { SDL_Delay(16 - frame_elapsed_time); }

                    if (!sdl_should_be_active || app_current_state != STATE_IN_ROOM) {
                        // printf("[클라이언트:메인] SDL 종료 조건 충족. (sdl_should_be_active=%d, app_current_state=%d)\n", sdl_should_be_active, app_current_state);
                        shutdown_sdl_environment();
                        printf("\r[클라이언트:메인] SDL UI 종료됨. 터미널 모드.\n> ");
                        fflush(stdout);
                    }
                }
            }
            else if (sdl_render_loop_running) {
                // printf("[클라이언트:메인] SDL 실행 중단 조건. (app_current_state=%d, client_is_nick_set=%d, sdl_should_be_active=%d)\n", app_current_state, client_is_nick_set, sdl_should_be_active);
                shutdown_sdl_environment();
                printf("\r[클라이언트:메인] SDL UI 비정상 상태로 종료. 터미널 모드.\n> ");
                fflush(stdout);
            }


            fd_set terminal_read_fds;
            FD_ZERO(&terminal_read_fds);
            FD_SET(STDIN_FILENO, &terminal_read_fds);
            struct timeval short_timeout = {0, 50000};

            int activity_on_stdin = select(STDIN_FILENO + 1, &terminal_read_fds, NULL, NULL, &short_timeout);

            if (activity_on_stdin < 0 && errno != EINTR) { perror("select on stdin"); main_program_should_run = 0; break;}

            if (FD_ISSET(STDIN_FILENO, &terminal_read_fds)) {
                if (fgets(terminal_input_line, BUFFER_SIZE, stdin) == NULL) {
                    printf("\r[클라이언트:메인] 입력 EOF. 종료합니다.\n");
                    send_formatted_message_to_server("QUIT", NULL); main_program_should_run = 0; break;
                }
                terminal_input_line[strcspn(terminal_input_line, "\n")] = 0;

                if (strlen(terminal_input_line) > 0) {
                    char command_part[BUFFER_SIZE];
                    char argument_part[BUFFER_SIZE] = "";
                    sscanf(terminal_input_line, "%s %[^\n]", command_part, argument_part);

                    if (strcmp(command_part, "/nick") == 0) {
                        if (strlen(argument_part) > 0) send_formatted_message_to_server("NICK", argument_part);
                        else printf("> 사용법: /nick <닉네임>\n> ");
                    } else if (!client_is_nick_set) {
                         printf("> 명령어 사용 전 /nick <닉네임>으로 닉네임을 먼저 설정해주세요. (현재 client_is_nick_set = %d)\n> ", client_is_nick_set);
                    } else if (strcmp(command_part, "/create") == 0) {
                        if (strlen(argument_part) > 0) send_formatted_message_to_server("CREATE_ROOM", argument_part);
                        else printf("> 사용법: /create <방이름>\n> ");
                    } else if (strcmp(command_part, "/join") == 0) {
                        if (strlen(argument_part) > 0) send_formatted_message_to_server("JOIN_ROOM", argument_part);
                        else printf("> 사용법: /join <방ID 또는 방이름>\n> ");
                    } else if (strcmp(command_part, "/roomlist") == 0) {
                        send_formatted_message_to_server("LIST_ROOMS", NULL);
                    } else if (strcmp(command_part, "/exit") == 0) {
                        if (app_current_state == STATE_IN_ROOM) {
                            send_formatted_message_to_server("EXIT_ROOM", NULL);
                        } else {
                            printf("> 현재 방에 참여하고 있지 않습니다.\n> ");
                        }
                    } else if (strcmp(command_part, "/quit") == 0) {
                        send_formatted_message_to_server("QUIT", NULL); main_program_should_run = 0;
                    } else if (terminal_input_line[0] == '/') {
                         printf("\r[클라이언트:메인] 알 수 없는 명령어: %s\n> ", terminal_input_line);
                    }
                    else { // 일반 채팅
                        if (app_current_state == STATE_IN_ROOM && client_is_nick_set) {
                            send_formatted_message_to_server("MSG", terminal_input_line);
                        } else {
                            printf("\r[클라이언트:메인] 채팅 불가 (방상태:%d, 닉네임설정:%d)\n> ", app_current_state, client_is_nick_set);
                        }
                    }
                }
                if (main_program_should_run && !sdl_render_loop_running) {
                     printf("> "); fflush(stdout);
                }
            }
        }

        printf("[클라이언트:메인] 메인 루프 종료. 리소스 정리 시작...\n");
        main_program_should_run = 0;
        sdl_should_be_active = 0;

        if (sdl_render_loop_running) { // SDL이 실행 중이었다면 명시적 종료
            shutdown_sdl_environment();
        }

        if (client_socket_fd >= 0) shutdown(client_socket_fd, SHUT_RDWR);
        pthread_join(server_message_receiver_tid, NULL);
        if (client_socket_fd >= 0) { close(client_socket_fd); client_socket_fd = -1;}

        pthread_mutex_destroy(&received_draw_commands_lock);
        free(received_draw_commands);

        // SDL_Quit(); // shutdown_sdl_environment에서 호출되거나, 여기서 한 번 더 호출해도 무방
        // TTF_Quit(); // shutdown_sdl_environment에서 호출되거나, 여기서 한 번 더 호출해도 무방

        printf("[클라이언트:메인] 프로그램 완전 종료.\n");
        return 0;
}
