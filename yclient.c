// integrated_client.c
//gcc yclient.c -o yclient -pthread $(sdl2-config --cflags --libs) -lSDL2_ttf
#include "SDL2/SDL.h"
#include "sdl2_ttf/2.24.0/include/SDL2/SDL_ttf.h" // SDL_ttf 사용을 위한 헤더 (선택 사항: UI 내 텍스트 표시용)
// 컴파일: gcc integrated_client.c -o integrated_client -pthread $(sdl2-config --cflags --libs) -lSDL2_ttf
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
#define INPUT_BUFFER_SIZE (BUFFER_SIZE * 2)
#define ROOM_NAME_SIZE 32

// SDL UI 설정
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define UI_AREA_HEIGHT 80 // 상단 UI 영역 높이

// SDL 색상 (서버와 동일하게 유지)
SDL_Color sdl_colors[] = {
    {0, 0, 0, 255},     // Black (0)
    {255, 0, 0, 255},   // Red   (1)
    {0, 255, 0, 255},   // Green (2)
    {0, 0, 255, 255},   // Blue  (3)
    {255, 255, 255, 255} // White (Eraser) (4)
};
int current_sdl_color_index = 0; // 기본 검은색
int current_pen_size = 5;        // 기본 펜 굵기

// 클라이언트 상태
typedef enum {
    STATE_LOBBY,
    STATE_IN_ROOM_AWAIT_NICK,
    STATE_IN_ROOM_DRAWING
} ClientAppState;

ClientAppState app_current_state = STATE_LOBBY;
int client_socket;
char client_lobby_nickname[NICK_SIZE] = "익명"; // 로비/기본 닉네임
char client_room_nickname[NICK_SIZE] = "";   // 현재 방에서 사용하는 확정된 닉네임
int client_current_room_id = -1;
int client_nickname_confirmed_in_room = 0;

// 서버 메시지 수신 버퍼
char server_message_buffer[INPUT_BUFFER_SIZE];
int server_message_len = 0;

// SDL 관련 전역 변수
SDL_Window* sdl_main_window = NULL;
SDL_Renderer* sdl_main_renderer = NULL;
SDL_Texture* sdl_drawing_canvas = NULL;
TTF_Font* ui_font = NULL; // TTF 폰트 (선택)
volatile int sdl_is_active = 0; // SDL UI가 현재 활성 상태인지
volatile int main_loop_should_run = 1; // 전체 프로그램 실행 플래그

// 로컬 드로잉용 변수
int local_is_drawing = 0;
int local_last_x = -1;
int local_last_y = -1;

// 스레드
pthread_t server_receiver_thread_id;

// --- 함수 프로토타입 ---
void send_message_to_server(const char* msg_content);
void* server_message_receiver_thread(void* arg);
void initialize_sdl_components();
void run_sdl_event_and_render_loop();
void shutdown_sdl_components();
void render_ui_elements();
void process_mouse_click_in_ui(int x, int y);
void apply_drawing_data_from_server(const char* draw_msg_content);
void clear_local_canvas_and_notify_server();

// --- 메시지 전송 함수 ---
void send_message_to_server(const char* msg_content) {
    char buffer_to_send[BUFFER_SIZE];
    snprintf(buffer_to_send, BUFFER_SIZE, "%s\n", msg_content); // 메시지 끝에 개행 추가
    if (send(client_socket, buffer_to_send, strlen(buffer_to_send), 0) < 0) {
        perror("send to server failed");
        // 실제로는 여기서 연결 종료 처리 등을 해야 함
    }
}

// --- 서버 메시지 수신 스레드 ---
void* server_message_receiver_thread(void* arg) {
    char chunk_buffer[BUFFER_SIZE];
    int bytes_read;
    printf("[클라이언트] 서버 메시지 수신 스레드 시작.\n");

    while (main_loop_should_run) {
        bytes_read = recv(client_socket, chunk_buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read <= 0) {
            if (bytes_read == 0) printf("\r[클라이언트] 서버 연결이 종료되었습니다.\n> ");
            else perror("\r[클라이언트] 서버 메시지 수신 오류");
            main_loop_should_run = 0; // 메인 루프 종료 유도
            if (sdl_is_active) sdl_is_active = 0; // SDL 루프도 종료되도록
            break;
        }
        chunk_buffer[bytes_read] = '\0';

        if (server_message_len + bytes_read < INPUT_BUFFER_SIZE) {
            strncat(server_message_buffer, chunk_buffer, bytes_read);
            server_message_len += bytes_read;
        } else {
            fprintf(stderr, "\r[클라이언트 경고] 수신 버퍼 오버플로우.\n> ");
            server_message_len = 0; memset(server_message_buffer, 0, INPUT_BUFFER_SIZE);
            continue;
        }

        char* current_message_start = server_message_buffer;
        char* newline_found;
        while ((newline_found = strchr(current_message_start, '\n')) != NULL) {
            *newline_found = '\0';
            char received_line[INPUT_BUFFER_SIZE];
            strcpy(received_line, current_message_start);

            printf("\r[서버] %s\n> ", received_line); // 모든 메시지 일단 터미널에 표시
            fflush(stdout);

            if (strncmp(received_line, "SMSG:", 5) == 0) {
                const char* smsg_payload = received_line + 5;
                if (strstr(smsg_payload, "닉네임이 설정되었습니다.")) {
                    // 서버로부터 받은 닉네임으로 로컬 닉네임 확정
                    char confirmed_nick[NICK_SIZE];
                     // 서버가 "SMSG:닉네임이 '새닉네임'(으)로 설정되었습니다." 형식으로 보내준다고 가정
                    if (sscanf(smsg_payload, "닉네임이 '%[^']'(으)로 설정되었습니다.", confirmed_nick) == 1) {
                        strcpy(client_room_nickname, confirmed_nick);
                    } else { // 파싱 실패 시, 로비 닉네임으로 설정 (클라이언트가 /nick 보낼 때 로비 닉네임 업데이트 필요)
                        strcpy(client_room_nickname, client_lobby_nickname);
                    }
                    printf("\r[클라이언트] 방 내 닉네임 확정: %s\n> ", client_room_nickname);

                    if (client_current_room_id != -1) { // 방에 이미 접속한 상태에서 닉네임 확정
                        client_nickname_confirmed_in_room = 1;
                        app_current_state = STATE_IN_ROOM_DRAWING;
                        // sdl_is_active = 1; // 메인 루프에서 SDL 시작하도록 신호
                        printf("\r[클라이언트] 그림판 UI 활성화 조건을 충족했습니다.\n> ");
                    } else { // 로비에서 닉네임 설정
                         strcpy(client_lobby_nickname, client_room_nickname); // 로비 닉네임도 업데이트
                    }
                } else if (strstr(smsg_payload, "방에 입장했습니다.") || strstr(smsg_payload, "생성되었고 입장했습니다.")) {
                    char parsed_room_name[ROOM_NAME_SIZE];
                    int parsed_room_id = -1;
                    if (sscanf(smsg_payload, "방 '%[^']'(ID:%d)", parsed_room_name, &parsed_room_id) == 2) {
                        client_current_room_id = parsed_room_id;
                        printf("\r[클라이언트] 방 '%s'(ID:%d) 입장.\n> ", parsed_room_name, client_current_room_id);
                    } else {
                        printf("\r[클라이언트] 방 입장 정보 파싱 실패: %s\n> ", smsg_payload);
                    }
                    app_current_state = STATE_IN_ROOM_AWAIT_NICK;
                    client_nickname_confirmed_in_room = 0; // 방에 새로 들어왔으므로 닉네임 재확인 필요
                    strcpy(client_room_nickname, "");      // 방 닉네임 초기화
                    printf("\r[클라이언트] 이 방에서 사용할 닉네임을 /nick <닉네임> 으로 설정하세요. (기본: %s)\n> ", client_lobby_nickname);

                } else if (strstr(smsg_payload, "방에서 퇴장했습니다.")) {
                    printf("\r[클라이언트] 방에서 퇴장. 로비로 돌아갑니다.\n> ");
                    app_current_state = STATE_LOBBY;
                    client_current_room_id = -1;
                    strcpy(client_room_nickname, "");
                    client_nickname_confirmed_in_room = 0;
                    if (sdl_is_active) sdl_is_active = 0; // SDL UI 종료 신호
                }
            } else if (strncmp(received_line, "DRAW_LINE:", 10) == 0 ||
                       strncmp(received_line, "DRAW_POINT:", 11) == 0 ||
                       strcmp(received_line, "DRAW_CLEAR") == 0) {
                if (app_current_state == STATE_IN_ROOM_DRAWING && sdl_is_active) {
                    apply_drawing_data_from_server(received_line);
                }
            }
            current_message_start = newline_found + 1;
        }
        if (current_message_start > server_message_buffer && strlen(current_message_start) > 0) {
            char temp_remaining[INPUT_BUFFER_SIZE];
            strcpy(temp_remaining, current_message_start);
            memset(server_message_buffer, 0, INPUT_BUFFER_SIZE);
            strcpy(server_message_buffer, temp_remaining);
            server_message_len = strlen(server_message_buffer);
        } else {
            server_message_len = 0;
            memset(server_message_buffer, 0, INPUT_BUFFER_SIZE);
        }
    }
    printf("[클라이언트] 서버 메시지 수신 스레드 종료.\n");
    return NULL;
}


// --- SDL UI 관련 함수 구현 ---
void initialize_sdl_components() {
    if (sdl_main_window || sdl_main_renderer) return; // 이미 초기화됨
    printf("[클라이언트 SDL] UI 초기화 중...\n");

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL 비디오 초기화 실패: %s\n", SDL_GetError());
        main_loop_should_run = 0; return;
    }
    if (TTF_Init() == -1) {
        fprintf(stderr, "SDL_ttf 초기화 실패: %s\n", TTF_GetError());
        // 폰트 없이 진행하려면 TTF 관련 코드 주석 처리
    }
    // ui_font = TTF_OpenFont("sans.ttf", 16); // 실제 폰트 파일 경로 제공 필요
    // if (!ui_font) fprintf(stderr, "폰트 로드 실패: %s (계속 진행)\n", TTF_GetError());


    sdl_main_window = SDL_CreateWindow("캐치마인드 (방 ID: ...)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    if (!sdl_main_window) { fprintf(stderr, "윈도우 생성 실패: %s\n", SDL_GetError()); main_loop_should_run = 0; return; }

    sdl_main_renderer = SDL_CreateRenderer(sdl_main_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdl_main_renderer) { fprintf(stderr, "렌더러 생성 실패: %s\n", SDL_GetError()); SDL_DestroyWindow(sdl_main_window); sdl_main_window = NULL; main_loop_should_run = 0; return; }

    sdl_drawing_canvas = SDL_CreateTexture(sdl_main_renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, WINDOW_WIDTH, WINDOW_HEIGHT);
    if (!sdl_drawing_canvas) { fprintf(stderr, "캔버스 텍스처 생성 실패: %s\n", SDL_GetError()); /* ... */ main_loop_should_run = 0; return; }

    SDL_SetRenderTarget(sdl_main_renderer, sdl_drawing_canvas); // 캔버스 대상으로 설정
    SDL_SetRenderDrawColor(sdl_main_renderer, 255, 255, 255, 255); // 흰색으로 클리어
    SDL_RenderClear(sdl_main_renderer);
    SDL_SetRenderTarget(sdl_main_renderer, NULL); // 기본 렌더 타겟으로 복원

    char window_title[100];
    snprintf(window_title, 100, "캐치마인드 - 방 ID: %d (%s)", client_current_room_id, client_room_nickname);
    SDL_SetWindowTitle(sdl_main_window, window_title);

    sdl_is_active = 1; // SDL UI 활성화 상태
    printf("[클라이언트 SDL] UI 초기화 완료.\n");
}

void shutdown_sdl_components() {
    printf("[클라이언트 SDL] UI 종료 중...\n");
    if (sdl_drawing_canvas) { SDL_DestroyTexture(sdl_drawing_canvas); sdl_drawing_canvas = NULL; }
    if (sdl_main_renderer) { SDL_DestroyRenderer(sdl_main_renderer); sdl_main_renderer = NULL; }
    if (sdl_main_window) { SDL_DestroyWindow(sdl_main_window); sdl_main_window = NULL; }
    if (ui_font) { TTF_CloseFont(ui_font); ui_font = NULL; }
    // TTF_Quit()과 SDL_Quit()은 프로그램 완전 종료 시 한 번만 호출
    sdl_is_active = 0;
}

void render_ui_elements() {
    if (!sdl_main_renderer) return;
    // 색상 버튼
    for (int i = 0; i < 5; i++) {
        SDL_Rect btn_rect = {10 + i * 45, 10, 40, 30};
        SDL_SetRenderDrawColor(sdl_main_renderer, sdl_colors[i].r, sdl_colors[i].g, sdl_colors[i].b, 255);
        SDL_RenderFillRect(sdl_main_renderer, &btn_rect);
        if (i == current_sdl_color_index) { // 선택된 색상 테두리
            SDL_SetRenderDrawColor(sdl_main_renderer, 255, 255, 0, 255); // 노란색
            SDL_RenderDrawRect(sdl_main_renderer, &btn_rect);
        }
    }
    // 펜 굵기 버튼 (예시 - 3개: small, medium, large)
    int pen_sizes[] = {3, 8, 15};
    for (int i = 0; i < 3; i++) {
        SDL_Rect btn_rect = {10 + i * 45, 50, 40, 30};
        // 버튼 색상은 회색톤으로, 선택된 굵기에 따라 다르게 표시 가능
        SDL_SetRenderDrawColor(sdl_main_renderer, 200 - i*20, 200 - i*20, 200 - i*20, 255);
        SDL_RenderFillRect(sdl_main_renderer, &btn_rect);
        if (pen_sizes[i] == current_pen_size) {
            SDL_SetRenderDrawColor(sdl_main_renderer, 255, 255, 0, 255);
            SDL_RenderDrawRect(sdl_main_renderer, &btn_rect);
        }
        // TODO: 여기에 굵기 텍스트나 아이콘 표시 (SDL_ttf 사용)
    }
    // 전체 지우기 버튼
    SDL_Rect clear_btn_rect = {WINDOW_WIDTH - 90, 10, 80, 30};
    SDL_SetRenderDrawColor(sdl_main_renderer, 220, 50, 50, 255); // 빨간색 계열
    SDL_RenderFillRect(sdl_main_renderer, &clear_btn_rect);
    // TODO: "Clear" 텍스트 표시 (SDL_ttf 사용)
}

void process_mouse_click_in_ui(int x, int y) {
    // 색상 버튼 클릭 확인
    for (int i = 0; i < 5; i++) {
        if (x >= 10 + i * 45 && x <= 10 + i * 45 + 40 && y >= 10 && y <= 10 + 30) {
            current_sdl_color_index = i;
            printf("[클라이언트 SDL] 색상 변경: %d\n", i);
            return;
        }
    }
    // 펜 굵기 버튼 클릭 확인
    int pen_sizes_arr[] = {3, 8, 15};
    for (int i = 0; i < 3; i++) {
        if (x >= 10 + i * 45 && x <= 10 + i * 45 + 40 && y >= 50 && y <= 50 + 30) {
            current_pen_size = pen_sizes_arr[i];
            printf("[클라이언트 SDL] 펜 굵기 변경: %d\n", current_pen_size);
            return;
        }
    }
    // 전체 지우기 버튼 클릭 확인
    if (x >= WINDOW_WIDTH - 90 && x <= WINDOW_WIDTH - 10 && y >= 10 && y <= 10 + 30) {
        printf("[클라이언트 SDL] 전체 지우기 클릭.\n");
        clear_local_canvas_and_notify_server();
        return;
    }
}

void clear_local_canvas_and_notify_server() {
    if (!sdl_drawing_canvas || !sdl_main_renderer) return;
    SDL_SetRenderTarget(sdl_main_renderer, sdl_drawing_canvas);
    SDL_SetRenderDrawColor(sdl_main_renderer, 255, 255, 255, 255); // 흰색으로 클리어
    SDL_RenderClear(sdl_main_renderer);
    SDL_SetRenderTarget(sdl_main_renderer, NULL);
    send_message_to_server("DRAW_CLEAR");
}


void apply_drawing_data_from_server(const char* draw_msg_content) {
    if (!sdl_drawing_canvas || !sdl_main_renderer || !sdl_is_active) return;
    SDL_SetRenderTarget(sdl_main_renderer, sdl_drawing_canvas);

    int x1,y1,x2,y2,c_idx,psize;
    if (strncmp(draw_msg_content, "DRAW_LINE:", 10) == 0) {
        if (sscanf(draw_msg_content + 10, "%d,%d,%d,%d,%d,%d", &x1, &y1, &x2, &y2, &c_idx, &psize) == 6) {
            if (c_idx >= 0 && c_idx < 5) {
                SDL_SetRenderDrawColor(sdl_main_renderer, sdl_colors[c_idx].r, sdl_colors[c_idx].g, sdl_colors[c_idx].b, 255);
                // 두꺼운 선 (간단 버전)
                for (int i = -psize / 2; i <= psize / 2; ++i) {
                     SDL_RenderDrawLine(sdl_main_renderer, x1 + i, y1, x2 + i, y2); // 수평 두께
                     SDL_RenderDrawLine(sdl_main_renderer, x1, y1 + i, x2, y2 + i); // 수직 두께 (대각선은 부정확)
                }
                // 더 나은 두꺼운 선: SDL_gfx 사용 또는 각 점에 원 그리기
            }
        }
    } else if (strncmp(draw_msg_content, "DRAW_POINT:", 11) == 0) {
        if (sscanf(draw_msg_content + 11, "%d,%d,%d,%d", &x1, &y1, &c_idx, &psize) == 4) {
             if (c_idx >= 0 && c_idx < 5) {
                SDL_SetRenderDrawColor(sdl_main_renderer, sdl_colors[c_idx].r, sdl_colors[c_idx].g, sdl_colors[c_idx].b, 255);
                SDL_Rect point_fill_rect = {x1 - psize / 2, y1 - psize / 2, psize, psize};
                SDL_RenderFillRect(sdl_main_renderer, &point_fill_rect);
             }
        }
    } else if (strcmp(draw_msg_content, "DRAW_CLEAR") == 0) {
        SDL_SetRenderDrawColor(sdl_main_renderer, 255, 255, 255, 255);
        SDL_RenderClear(sdl_main_renderer);
    }
    SDL_SetRenderTarget(sdl_main_renderer, NULL);
}


void run_sdl_event_and_render_loop() {
    if (!sdl_is_active || !sdl_main_renderer || !sdl_main_window) return;
    printf("[클라이언트 SDL] 그래픽 루프 시작 (방: %d, 닉네임: %s).\n", client_current_room_id, client_room_nickname);

    SDL_Event sdl_event_handler;
    Uint32 last_frame_time = 0;

    while (sdl_is_active && app_current_state == STATE_IN_ROOM_DRAWING && client_nickname_confirmed_in_room) {
        while (SDL_PollEvent(&sdl_event_handler)) {
            if (sdl_event_handler.type == SDL_QUIT) {
                send_message_to_server("CMD:/exit"); // 방 나가기 (서버 응답 후 상태 변경됨)
                // sdl_is_active = 0; // 수신 스레드에서 서버 응답 받고 변경하도록 유도
                // main_loop_should_run = 0; // 전체 종료는 /quit으로
                break; // SDL 이벤트 루프만 빠져나감
            }
            if (sdl_event_handler.type == SDL_MOUSEBUTTONDOWN) {
                if (sdl_event_handler.button.button == SDL_BUTTON_LEFT) {
                    int mouse_x = sdl_event_handler.button.x;
                    int mouse_y = sdl_event_handler.button.y;
                    if (mouse_y < UI_AREA_HEIGHT) { // UI 영역 클릭
                        process_mouse_click_in_ui(mouse_x, mouse_y);
                    } else { // 그림 영역 클릭
                        local_is_drawing = 1;
                        local_last_x = mouse_x;
                        local_last_y = mouse_y;
                        char point_msg[BUFFER_SIZE];
                        snprintf(point_msg, BUFFER_SIZE, "DRAW_POINT:%d,%d,%d,%d", mouse_x, mouse_y, current_sdl_color_index, current_pen_size);
                        send_message_to_server(point_msg);
                        apply_drawing_data_from_server(point_msg); // 로컬에도 즉시 반영
                    }
                }
            } else if (sdl_event_handler.type == SDL_MOUSEMOTION) {
                if (local_is_drawing) {
                    int mouse_x = sdl_event_handler.motion.x;
                    int mouse_y = sdl_event_handler.motion.y;
                    if (mouse_y >= UI_AREA_HEIGHT) { // 그림 영역 안에서만
                        char line_msg[BUFFER_SIZE];
                        snprintf(line_msg, BUFFER_SIZE, "DRAW_LINE:%d,%d,%d,%d,%d,%d", local_last_x, local_last_y, mouse_x, mouse_y, current_sdl_color_index, current_pen_size);
                        send_message_to_server(line_msg);
                        apply_drawing_data_from_server(line_msg); // 로컬 반영
                        local_last_x = mouse_x;
                        local_last_y = mouse_y;
                    }
                }
            } else if (sdl_event_handler.type == SDL_MOUSEBUTTONUP) {
                if (sdl_event_handler.button.button == SDL_BUTTON_LEFT) {
                    local_is_drawing = 0;
                    local_last_x = -1;
                    local_last_y = -1;
                }
            }
        }
        if (!sdl_is_active || app_current_state != STATE_IN_ROOM_DRAWING || !client_nickname_confirmed_in_room) {
            break; // 루프 종료 조건 (외부에서 변경 시)
        }


        SDL_SetRenderDrawColor(sdl_main_renderer, 100, 100, 100, 255); // 창 배경
        SDL_RenderClear(sdl_main_renderer);
        SDL_RenderCopy(sdl_main_renderer, sdl_drawing_canvas, NULL, NULL); // 캔버스 그리기
        render_ui_elements(); // UI 그리기
        SDL_RenderPresent(sdl_main_renderer);

        Uint32 current_render_time = SDL_GetTicks();
        if (current_render_time - last_frame_time < 16) { // 약 60FPS
            SDL_Delay(16 - (current_render_time - last_frame_time));
        }
        last_frame_time = SDL_GetTicks();
    }
    shutdown_sdl_components(); // SDL 루프가 끝나면 자원 해제
    printf("[클라이언트 SDL] 그래픽 루프 종료.\n");
}


// --- 메인 함수 ---
int main(int argc, char* argv[]) {
    struct sockaddr_in server_address_info;
    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0) { perror("socket"); exit(EXIT_FAILURE); }

    memset(&server_address_info, 0, sizeof(server_address_info));
    server_address_info.sin_family = AF_INET;
    server_address_info.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_address_info.sin_addr) <= 0) {
        perror("inet_pton"); close(client_socket); exit(EXIT_FAILURE);
    }

    if (connect(client_socket, (struct sockaddr*)&server_address_info, sizeof(server_address_info)) < 0) {
        perror("connect"); close(client_socket); exit(EXIT_FAILURE);
    }
    printf("[클라이언트] 서버에 연결되었습니다. (IP: %s, Port: %d)\n", SERVER_IP, SERVER_PORT);

    // 수신 스레드 생성
    if (pthread_create(&server_receiver_thread_id, NULL, server_message_receiver_thread, NULL) != 0) {
        perror("수신 스레드 생성 실패"); close(client_socket); exit(EXIT_FAILURE);
    }

    char terminal_user_input[BUFFER_SIZE];
    printf("> "); fflush(stdout);

    while (main_loop_should_run) {
        // SDL UI 실행 조건: 방에 있고, 닉네임 확정되었고, sdl_is_active가 0 (아직 안 켜짐)
        if (app_current_state == STATE_IN_ROOM_DRAWING && client_nickname_confirmed_in_room && !sdl_is_active) {
            initialize_sdl_components(); // 여기서 성공하면 sdl_is_active = 1
            if (sdl_is_active) {
                run_sdl_event_and_render_loop(); // 블로킹 함수, 종료 시 sdl_is_active = 0
            }
            // SDL 루프 종료 후 (예: 창 닫기, /exit 등)
            if (app_current_state == STATE_IN_ROOM_DRAWING && !sdl_is_active) { // sdl_is_active가 0으로 바뀌었으면
                printf("\r[클라이언트] 그림판 UI가 닫혔습니다. 터미널로 돌아갑니다.\n");
                // 서버에 /exit 메시지를 보내지 않았다면 (예: SDL 창의 X 버튼 클릭) 보내야 함
                // 여기서는 run_sdl_event_and_render_loop 내부에서 SDL_QUIT 시 /exit 보내도록 가정
                // app_current_state = STATE_LOBBY; // 수신 스레드에서 서버 응답 받고 변경하는 것이 더 정확
                printf("> "); fflush(stdout);
            }
        }

        // 터미널 입력 처리 (select 사용)
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        struct timeval tv = {0, 100000}; // 0.1초 타임아웃 (SDL 상태 변경 감지용)

        int activity = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0 && errno != EINTR) { perror("select on stdin"); main_loop_should_run = 0; break; }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (fgets(terminal_user_input, BUFFER_SIZE, stdin) == NULL) {
                printf("\r[클라이언트] 입력 EOF. 종료합니다.\n");
                send_message_to_server("CMD:/quit"); main_loop_should_run = 0; break;
            }
            terminal_user_input[strcspn(terminal_user_input, "\n")] = 0;

            if (strlen(terminal_user_input) > 0) {
                char msg_to_send_server[BUFFER_SIZE];
                if (terminal_user_input[0] == '/') {
                    snprintf(msg_to_send_server, BUFFER_SIZE, "CMD:%s", terminal_user_input);
                    if (strncmp(terminal_user_input, "/nick ", 6) == 0) {
                         // /nick 명령어의 경우, 로비 닉네임도 업데이트
                         char temp_nick[NICK_SIZE];
                         if(sscanf(terminal_user_input, "/nick %s", temp_nick) == 1){
                            if(strlen(temp_nick) > 0 && strlen(temp_nick) < NICK_SIZE) {
                                strcpy(client_lobby_nickname, temp_nick);
                                printf("\r[클라이언트] (로컬) 로비 닉네임 시도: %s\n", client_lobby_nickname);
                            }
                         }
                    }
                } else { // 일반 채팅
                    if (app_current_state == STATE_IN_ROOM_DRAWING && client_nickname_confirmed_in_room) {
                        snprintf(msg_to_send_server, BUFFER_SIZE, "CHAT:%s", terminal_user_input);
                    } else {
                        printf("\r[클라이언트] 채팅은 방에 입장하여 닉네임을 설정한 후에 가능합니다.\n> ");
                        fflush(stdout);
                        continue; // 서버로 안 보냄
                    }
                }
                send_message_to_server(msg_to_send_server);
                if (strcmp(terminal_user_input, "/quit") == 0) {
                    main_loop_should_run = 0; break;
                }
            }
            if (main_loop_should_run && !sdl_is_active) { // SDL이 안돌고 있을때만 프롬프트
                 printf("> "); fflush(stdout);
            }
        }
    }

    printf("[클라이언트] 메인 루프 종료. 정리 시작...\n");
    main_loop_should_run = 0; // 수신 스레드 종료 유도
    sdl_is_active = 0;        // SDL 루프 (혹시 돌고있다면) 종료 유도

    shutdown(client_socket, SHUT_RDWR); // recv 스레드의 recv()가 즉시 리턴하도록
    pthread_join(server_receiver_thread_id, NULL);
    // close(client_socket); // 수신 스레드에서 이미 닫았을 수 있음

    if (sdl_main_window) shutdown_sdl_components(); // SDL 자원 남아있으면 정리
    if (ui_font) TTF_CloseFont(ui_font);
    TTF_Quit();
    SDL_Quit();

    printf("[클라이언트] 프로그램 종료.\n");
    return 0;
}
