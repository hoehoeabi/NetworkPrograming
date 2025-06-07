// gcc yclient3.c -o yclient3 -pthread $(sdl2-config --cflags --libs) -lSDL2_ttf
#include "SDL2/SDL.h"
#include "sdl2_ttf/2.24.0/include/SDL2/SDL_ttf.h" 
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

// 그리기 색상 팔레트
SDL_Color sdl_colors[] = {
    {0, 0, 0, 255},       // 검정
    {255, 0, 0, 255},     // 빨강
    {0, 255, 0, 255},     // 초록
    {0, 0, 255, 255},     // 파랑
    {255, 255, 255, 255}  // 흰색 (지우개 용도)
};
int current_sdl_color_index = 0; // 현재 선택된 색상 인덱스
int current_pen_size = 5;        // 현재 펜 크기
volatile int text_input_focused = 0; // 텍스트 입력창 포커스 여부

// 클라이언트 상태
typedef enum {
    STATE_LOBBY,    // 로비 상태
    STATE_IN_ROOM   // 방 안에 있는 상태
} ClientAppState;

volatile ClientAppState app_current_state = STATE_LOBBY; // 현재 클라이언트 앱 상태
int client_socket_fd;                           // 클라이언트 소켓 파일 디스크립터
char client_nickname[NICK_SIZE] = "익명";       // 클라이언트 닉네임
volatile int client_is_nick_set = 0;            // 닉네임 설정 여부
int client_current_room_id = -1;                // 현재 입장한 방 ID
char client_current_room_name[ROOM_NAME_SIZE] = ""; // 현재 입장한 방 이름
char answer_input_buffer[BUFFER_SIZE] = "";     // 정답 입력 버퍼
int answer_input_len = 0;                       // 정답 입력 버퍼 길이
char ime_composition[BUFFER_SIZE] = "";
int ime_cursor = 0;
Uint32 round_start_tick = 0; // 라운드 종료 시각 (ms 단위)
int round_duration_seconds = 60; // 제한 시간 

// 서버 메시지 수신 버퍼
char server_message_buffer[INPUT_BUFFER_SIZE];
int server_message_len = 0;
int is_current_user_drawer = 0;

// SDL 관련 전역 변수
SDL_Window* app_sdl_window = NULL;
SDL_Renderer* app_sdl_renderer = NULL;
SDL_Texture* app_drawing_canvas_texture = NULL; // 그림 그리는 캔버스 텍스처
TTF_Font* app_ui_font = NULL;                   // UI 텍스트용 폰트
volatile int sdl_should_be_active = 0;          // SDL UI 활성화 필요 플래그
volatile int sdl_render_loop_running = 0;       // SDL 렌더링 루프 실행 중 여부
volatile int main_program_should_run = 1;       // 메인 프로그램 실행 플래그

// 로컬 드로잉용 변수
int local_mouse_is_drawing = 0;
int local_mouse_last_x = -1;
int local_mouse_last_y = -1;

// 수신한 그림 데이터를 저장하는 구조체
typedef struct {
    int type; // 0: POINT, 1: LINE, 2: CLEAR
    int x1, y1, x2, y2, color_index, size;
} ReceivedDrawData;

ReceivedDrawData *received_draw_commands = NULL; // 수신된 드로잉 명령 배열
int received_draw_commands_count = 0;            // 수신된 드로잉 명령 개수
int received_draw_commands_capacity = 20;        // 초기 용량
pthread_mutex_t received_draw_commands_lock;     // 드로잉 명령 접근 보호 뮤텍스

// 스레드
pthread_t server_message_receiver_tid; // 서버 메시지 수신 스레드 ID

// --- 함수 프로토타입 ---
void send_formatted_message_to_server(const char* type_or_full_cmd, const char* payload);
void* server_message_receiver_thread_func(void* arg);
void initialize_sdl_environment();
void shutdown_sdl_environment();
void render_sdl_ui_elements();
void handle_sdl_mouse_click_on_ui(int x, int y);
void client_side_clear_canvas_and_notify_server();
void draw_text(SDL_Renderer* renderer, const char* text, int x, int y, SDL_Color color) {
    extern TTF_Font* app_ui_font; // 전역 폰트
    SDL_Surface* surface = TTF_RenderUTF8_Blended(app_ui_font, text, color);
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_Rect dst = { x, y, surface->w, surface->h };
    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}
// 정답 결과 표시용 변수 (추가됨)
char sdl_answer_result_text[BUFFER_SIZE] = "";
Uint32 answer_result_display_tick = 0; // 메시지 표시 시작 시간 (SDL_GetTicks() 값)

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
    char sdl_answer_result_text[128] = "";
    char server_response_line[INPUT_BUFFER_SIZE];
    Uint32 answer_result_display_tick = 0;

    int bytes_read;
    printf("[클라이언트:수신스레드] 시작됨.\n");

    while (main_program_should_run) {
        bytes_read = recv(client_socket_fd, temp_chunk_buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read <= 0) {
            if (bytes_read == 0) printf("\r[클라이언트:수신스레드] 서버 연결 정상 종료됨.\n> ");
            else perror("\r[클라이언트:수신스레드] 서버 메시지 수신 오류");
            main_program_should_run = 0;
            Uint32 now = SDL_GetTicks();
	if (now - answer_result_display_tick < 2000 && strlen(sdl_answer_result_text) > 0) {
    		SDL_Color color;
		if (strstr(sdl_answer_result_text, "정답입니다!")) {
    			color = (SDL_Color){0, 255, 0};  // 초록색
		} else {
    			color = (SDL_Color){255, 0, 0};  // 빨간색
		}

	draw_text(app_sdl_renderer, sdl_answer_result_text, 120, 30, color);
	}
	if (SDL_GetTicks() - answer_result_display_tick > 2000) {
        	sdl_answer_result_text[0] = '\0';
	}
	else if (strncmp(server_response_line, "TIMER:", 6) == 0) {
        int time_left = atoi(server_response_line + 6);  
        round_start_tick = SDL_GetTicks();
        round_duration_seconds = time_left;
        printf("[클라이언트:TIMER] %d초로 설정됨\n", time_left);
	}


        if (sdl_render_loop_running) sdl_should_be_active = 0; // SDL 루프 종료 유도
        break;
        }
        temp_chunk_buffer[bytes_read] = '\0'; // 널 종료

        // 버퍼 오버플로우 방지 및 메시지 추가
        if (server_message_len + bytes_read < INPUT_BUFFER_SIZE) {
            strncat(server_message_buffer, temp_chunk_buffer, bytes_read);
            server_message_len += bytes_read;
        } else {
            fprintf(stderr, "\r[클라이언트:수신스레드] 수신 버퍼 오버플로우. 버퍼 초기화.\n> ");
            server_message_len = 0; memset(server_message_buffer, 0, INPUT_BUFFER_SIZE);
            continue; // 다음 수신 시도
        }

        // 라인 단위로 메시지 처리
        char* current_line_ptr = server_message_buffer;
        char* newline_char_found;
        while ((newline_char_found = strchr(current_line_ptr, '\n')) != NULL) {
            *newline_char_found = '\0'; // 개행 문자를 널 문자로 바꿔 라인 끝 표시
            strcpy(server_response_line, current_line_ptr); // 현재 라인 복사
           if (strstr(server_response_line, "정답입니다!") != NULL) {
    		round_start_tick = SDL_GetTicks();
    		round_duration_seconds = 60;  // 혹은 서버에서 전달받은 시간 사용
	   }
           if (strncmp(server_response_line, "SMSG:제시어는 '", 15) == 0) {
    is_current_user_drawer = 1;
}
else if (strstr(server_response_line, "님이 출제자입니다.") != NULL) {
    is_current_user_drawer = strstr(server_response_line, client_nickname) != NULL;
}
else if (strncmp(server_response_line, "TIMER:", 6) == 0) {
    int time_left = atoi(server_response_line + 6);
    round_start_tick = SDL_GetTicks();
    round_duration_seconds = time_left;
}


            // 일반 메시지 출력 (화면에 표시)
            if (strncmp(server_response_line, "MSG:", 4) == 0) {
                printf("\r%s\n> ", server_response_line + 4);
            }
            // 그리기 메시지(DRAW_*)가 아니면서 시스템 메시지/에러/방 목록인 경우
            else if (strncmp(server_response_line, "DRAW_", 5) != 0) {
                if (strncmp(server_response_line, "SMSG:방", 7) == 0 || strncmp(server_response_line, "SMSG:닉네임이", 10) == 0) {
                    printf("\r[클라이언트 시스템] %s\n> ", server_response_line);
                } else if (strncmp(server_response_line, "ROOM_EVENT:", 11) == 0) {
                    printf("\r%s\n> ", server_response_line + 11);
                } else if (strncmp(server_response_line, "SMSG:", 5) == 0 || strncmp(server_response_line, "ERR_", 4) == 0 || strncmp(server_response_line, "ROOMLIST:", 9) == 0) {
                    printf("\r[서버 응답] %s\n> ", server_response_line);
                }
            }
            fflush(stdout); // 즉시 터미널에 출력

            // === 클라이언트 상태 변화 처리 ===
            if (strncmp(server_response_line, "SMSG:", 5) == 0) {
                const char* smsg_payload_content = server_response_line + 5;
                char temp_room_name[ROOM_NAME_SIZE];
                int temp_room_id;

                // 닉네임 설정 확인
                if (strstr(smsg_payload_content, "닉네임이 설정되었습니다.") || strstr(smsg_payload_content, "(으)로 설정되었습니다.")) {
                    char confirmed_nickname_str[NICK_SIZE];
                    if (sscanf(smsg_payload_content, "닉네임이 '%[^']'(으)로 설정되었습니다.", confirmed_nickname_str) == 1) {
                        strcpy(client_nickname, confirmed_nickname_str);
                        client_is_nick_set = 1;
                    }
                }
                // 방 입장/생성 확인
                else if (sscanf(smsg_payload_content, "방 '%[^']'(ID:%d)에 입장했습니다.", temp_room_name, &temp_room_id) == 2 ||
                         sscanf(smsg_payload_content, "방 '%[^']'(ID:%d)이(가) 생성되었고 입장했습니다.", temp_room_name, &temp_room_id) == 2) {
                    strcpy(client_current_room_name, temp_room_name);
                    client_current_room_id = temp_room_id;
                    app_current_state = STATE_IN_ROOM;
                    if (client_is_nick_set) sdl_should_be_active = 1; // 닉네임 설정되었으면 SDL UI 활성화 요청
                }
                // 방 퇴장 확인
                else if (strstr(smsg_payload_content, "방에서 퇴장했습니다.")) {
                    app_current_state = STATE_LOBBY;
                    client_current_room_id = -1;
                    strcpy(client_current_room_name, "");
                    if (sdl_render_loop_running) sdl_should_be_active = 0; // SDL 루프 종료 유도
                }
                // 정답/오답 메시지 처리 (추가됨)
                else if (strncmp(smsg_payload_content, "정답입니다", 12) == 0 ||
                         strncmp(smsg_payload_content, "오답입니다", 13) == 0) {
                    strncpy(sdl_answer_result_text, smsg_payload_content, sizeof(sdl_answer_result_text) - 1);
                    sdl_answer_result_text[sizeof(sdl_answer_result_text) - 1] = '\0';
                    answer_result_display_tick = SDL_GetTicks(); // 메시지 표시 시작 시간 기록
                }
            }

            // === 드로잉 명령 처리 (받은 드로잉 데이터를 큐에 추가) ===
            else if (strncmp(server_response_line, "DRAW_POINT:", 11) == 0) {
                ReceivedDrawData data = {0};
                data.type = 0; // POINT
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
                data.type = 1; // LINE
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
    data.type = 2; // CLEAR 명령
    pthread_mutex_lock(&received_draw_commands_lock);
    if (received_draw_commands_count >= received_draw_commands_capacity) {
        received_draw_commands_capacity *= 2;
        received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
        if (!received_draw_commands) {
            perror("realloc draw clear (CLEAR) failed");
            pthread_mutex_unlock(&received_draw_commands_lock);
            main_program_should_run = 0;
            break;
        }
    }
    received_draw_commands[received_draw_commands_count++] = data;
    pthread_mutex_unlock(&received_draw_commands_lock);
} else if (strcmp(server_response_line, "CLEAR") == 0) {
                ReceivedDrawData data = {0};
                data.type = 2; // CLEAR
                pthread_mutex_lock(&received_draw_commands_lock);
                if (received_draw_commands_count >= received_draw_commands_capacity) {
                    received_draw_commands_capacity *= 2;
                    received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
                    if (!received_draw_commands) { perror("realloc draw clear failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; break;}
                }
                received_draw_commands[received_draw_commands_count++] = data;
                pthread_mutex_unlock(&received_draw_commands_lock);
            }
            current_line_ptr = newline_char_found + 1; // 다음 라인의 시작 지점 갱신
        }
        // 처리되지 않은 잔여 데이터가 있다면 버퍼의 앞으로 이동
        if (current_line_ptr > server_message_buffer && strlen(current_line_ptr) > 0) {
            char temp_buf[INPUT_BUFFER_SIZE]; strcpy(temp_buf, current_line_ptr);
            memset(server_message_buffer, 0, INPUT_BUFFER_SIZE); strcpy(server_message_buffer, temp_buf);
            server_message_len = strlen(server_message_buffer);
        } else {
            // 모든 라인 처리 완료 또는 잔여 데이터 없음
            server_message_len = 0; memset(server_message_buffer, 0, INPUT_BUFFER_SIZE);
        }
    }
    printf("[클라이언트:수신스레드] 종료됨.\n");
    return NULL;
}

// --- SDL 환경 초기화 ---
void initialize_sdl_environment() {
    if (sdl_render_loop_running || app_sdl_window) {
        // 이미 실행 중이거나 윈도우가 존재하면 다시 초기화하지 않음
        return;
    }
    printf("[클라이언트:SDL] UI 초기화 시작 (방: '%s', 닉네임: '%s')\n", client_current_room_name, client_nickname);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL Init 실패: %s\n", SDL_GetError()); main_program_should_run = 0; return; }
    if (TTF_Init() == -1) { fprintf(stderr, "TTF Init 실패: %s (계속 진행)\n", TTF_GetError()); }

    // 폰트 로드
    app_ui_font = TTF_OpenFont("DejaVuSans.ttf", 16);
    if (!app_ui_font) {
        fprintf(stderr, "폰트 로딩 실패: %s\n", TTF_GetError());
    } 


    // 윈도우 타이틀 설정
    char win_title[128];
    snprintf(win_title, sizeof(win_title), "그림판 - 방: %s (%s)", client_current_room_name, client_nickname);
    app_sdl_window = SDL_CreateWindow(win_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    if (!app_sdl_window) { fprintf(stderr, "윈도우 생성 실패: %s\n", SDL_GetError()); main_program_should_run = 0; SDL_Quit(); return; }

    // 렌더러 생성
    app_sdl_renderer = SDL_CreateRenderer(app_sdl_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!app_sdl_renderer) { SDL_DestroyWindow(app_sdl_window); app_sdl_window=NULL; fprintf(stderr, "렌더러 생성 실패: %s\n", SDL_GetError()); main_program_should_run = 0; SDL_Quit(); return; }

    // 그림 그리기 캔버스 텍스처 생성 (UI 영역 제외)
    app_drawing_canvas_texture = SDL_CreateTexture(app_sdl_renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, WINDOW_WIDTH, WINDOW_HEIGHT - UI_AREA_HEIGHT);
    if (!app_drawing_canvas_texture) { fprintf(stderr, "캔버스 텍스처 생성 실패: %s\n", SDL_GetError()); shutdown_sdl_environment(); main_program_should_run = 0; return; }

    // 캔버스 초기화 (흰색으로 채움)
    SDL_SetRenderTarget(app_sdl_renderer, app_drawing_canvas_texture);
    SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 255, 255); // 흰색
    SDL_RenderClear(app_sdl_renderer);
    SDL_SetRenderTarget(app_sdl_renderer, NULL); // 다시 기본 렌더 타겟으로 설정

    sdl_render_loop_running = 1; // SDL 렌더링 루프 실행 상태로 변경
    printf("[클라이언트:SDL] UI 초기화 완료, 루프 실행 준비됨 (sdl_render_loop_running = %d).\n", sdl_render_loop_running);
    SDL_ShowWindow(app_sdl_window); // 윈도우를 보여줌
    SDL_RaiseWindow(app_sdl_window); // 윈도우를 포그라운드로 가져옴
    SDL_StartTextInput();            // 텍스트 입력 활성화 (입력창 포커스 시 사용)
}

// --- SDL 환경 종료 ---
void shutdown_sdl_environment() {
    // 이미 종료되었거나 초기화되지 않았다면 바로 리턴
    if (!app_sdl_window && !app_sdl_renderer && !app_drawing_canvas_texture && !sdl_render_loop_running && !app_ui_font) {
        return;
    }
    printf("[클라이언트:SDL] UI 종료 시작...\n");
    if (app_drawing_canvas_texture) { SDL_DestroyTexture(app_drawing_canvas_texture); app_drawing_canvas_texture = NULL; }
    if (app_sdl_renderer) { SDL_DestroyRenderer(app_sdl_renderer); app_sdl_renderer = NULL; }
    if (app_sdl_window) { SDL_DestroyWindow(app_sdl_window); app_sdl_window = NULL; }
    if (app_ui_font) { TTF_CloseFont(app_ui_font); app_ui_font = NULL; }
    
    SDL_StopTextInput(); // 텍스트 입력 비활성화
    TTF_Quit(); // TTF 종료
    SDL_Quit(); // SDL 종료

    sdl_render_loop_running = 0;
    sdl_should_be_active = 0;
    printf("[클라이언트:SDL] UI 자원 해제 완료 (sdl_render_loop_running = %d, sdl_should_be_active = %d).\n", sdl_render_loop_running, sdl_should_be_active);
}

// --- SDL UI 요소 렌더링 ---
void render_sdl_ui_elements() {
    Uint32 now = SDL_GetTicks();
    int seconds_passed = (now - round_start_tick) / 1000;
    int seconds_left = round_duration_seconds - seconds_passed;

    if (!app_sdl_renderer) return;

    // UI 배경
    SDL_Rect ui_bg_rect = {0, 0, WINDOW_WIDTH, UI_AREA_HEIGHT};
    SDL_SetRenderDrawColor(app_sdl_renderer, 220, 220, 220, 255);
    SDL_RenderFillRect(app_sdl_renderer, &ui_bg_rect);

    // 색상 팔레트 버튼
    for (int i = 0; i < 5; i++) {
        SDL_Rect btn = {10 + i * 45, 10, 40, 30};
        SDL_SetRenderDrawColor(app_sdl_renderer, sdl_colors[i].r, sdl_colors[i].g, sdl_colors[i].b, 255);
        SDL_RenderFillRect(app_sdl_renderer, &btn);
        if (i == current_sdl_color_index) {
            SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 0, 255);
            SDL_RenderDrawRect(app_sdl_renderer, &btn);
        }
    }

    // 펜 굵기 버튼
    int pen_sizes_options[] = {3, 8, 15};
    for (int i = 0; i < 3; i++) {
        SDL_Rect btn = {10 + i * 45, 45, 40, 30};
        SDL_SetRenderDrawColor(app_sdl_renderer, 180 - i * 10, 180 - i * 10, 180 - i * 10, 255);
        SDL_RenderFillRect(app_sdl_renderer, &btn);
        if (pen_sizes_options[i] == current_pen_size) {
            SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 0, 255);
            SDL_RenderDrawRect(app_sdl_renderer, &btn);
        }
    }

    // 전체 지우기 버튼
    SDL_Rect clear_btn = {10 + 3 * 45, 45, 100, 30};
    SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 255, 255); // 흰색
    SDL_RenderFillRect(app_sdl_renderer, &clear_btn);
    SDL_SetRenderDrawColor(app_sdl_renderer, 0, 0, 0, 255); // 검정
    SDL_RenderDrawRect(app_sdl_renderer, &clear_btn);
    SDL_Color black = {0, 0, 0, 255};  // 검정색
    draw_text(app_sdl_renderer, "All Clear", clear_btn.x + 10, clear_btn.y + 5, black);

    // 제출 버튼
    SDL_Rect submit_btn = {710, 10, 80, 30};
    SDL_SetRenderDrawColor(app_sdl_renderer, 100, 200, 100, 255);
    SDL_RenderFillRect(app_sdl_renderer, &submit_btn);

    if (app_ui_font) {
        SDL_Color text_color = {0, 0, 0};
        SDL_Surface* btn_text_surface = TTF_RenderUTF8_Blended(app_ui_font, "Submit", text_color);
        if (btn_text_surface) {
            SDL_Texture* btn_text_texture = SDL_CreateTextureFromSurface(app_sdl_renderer, btn_text_surface);
            SDL_Rect btn_text_rect = {
                submit_btn.x + (submit_btn.w - btn_text_surface->w) / 2,
                submit_btn.y + (submit_btn.h - btn_text_surface->h) / 2,
                btn_text_surface->w,
                btn_text_surface->h
            };
            SDL_RenderCopy(app_sdl_renderer, btn_text_texture, NULL, &btn_text_rect);
            SDL_FreeSurface(btn_text_surface);
            SDL_DestroyTexture(btn_text_texture);
        }
    }

    // 정답 입력창
    SDL_Rect answer_box = {400, 10, 300, 30};
    SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 255, 255);
    SDL_RenderFillRect(app_sdl_renderer, &answer_box);

    if (app_ui_font) {
        SDL_Color text_color = {0, 0, 0};
        char full_text_with_ime[BUFFER_SIZE * 2];
        snprintf(full_text_with_ime, sizeof(full_text_with_ime), "%s%s", answer_input_buffer, ime_composition);

        SDL_Surface* text_surface = TTF_RenderUTF8_Blended(app_ui_font, full_text_with_ime, text_color);
        if (text_surface) {
            SDL_Texture* text_texture = SDL_CreateTextureFromSurface(app_sdl_renderer, text_surface);
            SDL_Rect text_dest = {answer_box.x + 5, answer_box.y + 5, text_surface->w, text_surface->h};
            SDL_RenderCopy(app_sdl_renderer, text_texture, NULL, &text_dest);
            SDL_FreeSurface(text_surface);
            SDL_DestroyTexture(text_texture);
        }

        if (text_input_focused) {
            SDL_SetRenderDrawColor(app_sdl_renderer, 0, 0, 255, 255);
            SDL_RenderDrawRect(app_sdl_renderer, &answer_box);
        }
    }

    // 정답/오답 결과 텍스트 출력
    if (strlen(sdl_answer_result_text) > 0 && SDL_GetTicks() - answer_result_display_tick < 3000) {
        SDL_Color color = {0, 0, 0, 255};
        if (strstr(sdl_answer_result_text, "O")) color = (SDL_Color){0, 160, 0, 255};
        else if (strstr(sdl_answer_result_text, "X")) color = (SDL_Color){200, 0, 0, 255};

        SDL_Surface* result_surface = TTF_RenderUTF8_Blended(app_ui_font, sdl_answer_result_text, color);
        if (result_surface) {
            SDL_Texture* result_texture = SDL_CreateTextureFromSurface(app_sdl_renderer, result_surface);
            SDL_Rect rect = {
                (WINDOW_WIDTH - result_surface->w) / 2, 5, result_surface->w, result_surface->h
            };
            SDL_RenderCopy(app_sdl_renderer, result_texture, NULL, &rect);
            SDL_FreeSurface(result_surface);
            SDL_DestroyTexture(result_texture);
        }
    }
    //타이머 표시
    if (round_start_tick > 0 && round_duration_seconds > 0 && app_ui_font) {
    Uint32 now = SDL_GetTicks();
    int seconds_passed = (now - round_start_tick) / 1000;
    int seconds_left = round_duration_seconds - seconds_passed;
    if (seconds_left < 0) seconds_left = 0;

    char timer_text[64];
    snprintf(timer_text, sizeof(timer_text), "TIMER : %dsec", seconds_left);

    SDL_Color text_color = {30, 144, 255, 255};  // 파란색

    SDL_Surface* surface = TTF_RenderUTF8_Blended(app_ui_font, timer_text, text_color);
    if (surface) {
        SDL_Texture* texture = SDL_CreateTextureFromSurface(app_sdl_renderer, surface);
        if (texture) {
            int tex_w = surface->w;
            int tex_h = surface->h;

            // 수치로 위치 설정 (중앙 상단)
            int text_x = (WINDOW_WIDTH - tex_w) / 2;
            int text_y = 45; // 너무 위에 가면 버튼이랑 겹치니 약간 내림

            // 반투명 배경 사각형
            SDL_SetRenderDrawColor(app_sdl_renderer, 0, 0, 0, 160); // 검정 반투명
            SDL_SetRenderDrawBlendMode(app_sdl_renderer, SDL_BLENDMODE_BLEND);
            SDL_Rect bg_rect = {text_x - 10, text_y - 5, tex_w + 20, tex_h + 10};
            SDL_RenderFillRect(app_sdl_renderer, &bg_rect);
            SDL_SetRenderDrawBlendMode(app_sdl_renderer, SDL_BLENDMODE_NONE);

            // 텍스트 렌더링
            SDL_Rect text_rect = {text_x, text_y, tex_w, tex_h};
            SDL_RenderCopy(app_sdl_renderer, texture, NULL, &text_rect);

            SDL_DestroyTexture(texture);
        }
        SDL_FreeSurface(surface);
    }
} 

}


// --- SDL UI 마우스 클릭 처리 ---
void handle_sdl_mouse_click_on_ui(int x, int y) {
    // 색상 버튼 클릭 처리
    for (int i = 0; i < 5; i++) {
        if (x >= 10 + i * 45 && x <= 10 + i * 45 + 40 && y >= 10 && y <= 10 + 30) {
            current_sdl_color_index = i;
            return;
        }
    }
    // 펜 크기 버튼 클릭 처리
    int pen_sizes_opt[] = {3, 8, 15};
    for (int i = 0; i < 3; i++) {
        if (x >= 10 + i * 45 && x <= 10 + i * 45 + 40 && y >= 45 && y <= 45 + 30) {
            current_pen_size = pen_sizes_opt[i];
            return;
        }
    }
    // 지우개/클리어 버튼 클릭 처리
    if (x >= 10 + 3 * 45 && x <= 10 + 3 * 45 + 100 && y >= 45 && y <= 45 + 30) {
    if (!is_current_user_drawer) {
        printf("[출제자 아님] 전체 지우기 버튼은 출제자만 사용할 수 있습니다.\n");
        return;
    }
    client_side_clear_canvas_and_notify_server();
    return;
}


    // 정답 입력창 클릭 처리 (추가됨)
    SDL_Rect answer_box = {400, 10, 300, 30}; // 입력창 위치 정의

    if (x >= answer_box.x && x <= answer_box.x + answer_box.w &&
        y >= answer_box.y && y <= answer_box.y + answer_box.h) {
        text_input_focused = 1;
        SDL_StartTextInput();  // 텍스트 입력 활성화
        printf("[디버깅] 입력창 포커스 ON\n");
    } else {
        text_input_focused = 0;
        SDL_StopTextInput();   // 텍스트 입력 비활성화
        // printf("[디버깅] 입력창 포커스 OFF\n");
    }
    SDL_Rect submit_btn = {710, 10, 80, 30};
    if (x >= 710 && x <= 710 + 80 && y >= 10 && y <= 10 + 30) {
    	if (answer_input_len > 0) {
        	send_formatted_message_to_server("MSG", answer_input_buffer);
        	answer_input_buffer[0] = '\0';
        	answer_input_len = 0;
    	}
    return;
    }
}

// --- 캔버스 지우기 및 서버에 알림 ---
void client_side_clear_canvas_and_notify_server() {
    if (!app_drawing_canvas_texture || !app_sdl_renderer) return;
    
    // 로컬 드로잉 명령 버퍼에 CLEAR 명령 추가 (즉시 화면에 반영 위함)
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
    
    send_formatted_message_to_server("DRAW_CLEAR", NULL); // 서버에 캔버스 지우기 명령 전송
}

// --- 메인 함수 ---
int main(int argc, char* argv[]) {
    // 1. 소켓 생성 및 서버 연결
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

    // 2. 서버 메시지 수신 스레드 생성
    if (pthread_create(&server_message_receiver_tid, NULL, server_message_receiver_thread_func, NULL) != 0) {
        perror("수신 스레드 생성 실패"); close(client_socket_fd); exit(1);
    }

    // 3. 드로잉 명령 버퍼 및 뮤텍스 초기화
    pthread_mutex_init(&received_draw_commands_lock, NULL);
    received_draw_commands = malloc(received_draw_commands_capacity * sizeof(ReceivedDrawData));
    if (received_draw_commands == NULL) {
        perror("[클라이언트:메인] received_draw_commands 메모리 할당 실패");
        exit(1);
    }

    // 4. 터미널 입력 및 SDL 이벤트 루프
    char terminal_input_line[BUFFER_SIZE];
    printf("> "); fflush(stdout); // 초기 프롬프트 출력
    
    SDL_Event sdl_event_handler;
    Uint32 sdl_frame_start_tick; // 프레임 시작 시간 측정용

    while (main_program_should_run) {
        // SDL UI 활성화 조건: 방에 입장했고, 닉네임이 설정되었고, SDL 활성화 플래그가 true일 때
        if (app_current_state == STATE_IN_ROOM && client_is_nick_set && sdl_should_be_active) {
            // SDL UI가 아직 실행 중이 아니면 초기화 시도
            if (!sdl_render_loop_running) {
                initialize_sdl_environment();
                if (!sdl_render_loop_running) { // 초기화 실패 시
                    printf("[클라이언트:메인] SDL 초기화 실패. 방에서 퇴장 요청.\n");
                    sdl_should_be_active = 0; // SDL 활성화 중단
                    if(app_current_state == STATE_IN_ROOM) {
                        send_formatted_message_to_server("EXIT_ROOM", NULL); // 서버에 방 퇴장 알림
                    }
                }
            }

            // SDL 렌더링 루프 (UI가 활성화된 경우)
            if (sdl_render_loop_running) {
                sdl_frame_start_tick = SDL_GetTicks(); // 현재 시간 기록

                // SDL 이벤트 처리 루프
                while (SDL_PollEvent(&sdl_event_handler)) {
                    if (sdl_event_handler.type == SDL_QUIT) { // 창 닫기 버튼 클릭
                        printf("[클라이언트:SDL이벤트] 종료 이벤트 (X 버튼). EXIT_ROOM 전송.\n");
                        send_formatted_message_to_server("EXIT_ROOM", NULL); // 서버에 방 퇴장 알림
                        // sdl_should_be_active = 0; // 서버 응답을 기다려 상태 변경하므로 주석 처리
                        break; // 이벤트 루프 종료
                    }
              
                    //  텍스트 입력 처리 (추가됨)
                    if (sdl_event_handler.type == SDL_TEXTINPUT && text_input_focused) {
                        int len = strlen(sdl_event_handler.text.text);
                        if (answer_input_len + len < BUFFER_SIZE - 1) { // 버퍼 오버플로우 방지
                            strcat(answer_input_buffer, sdl_event_handler.text.text);
                            answer_input_len += len;
                            // printf("[디버깅] 입력된 글자: %s, 현재 버퍼: %s\n", sdl_event_handler.text.text, answer_input_buffer);
                        }
                    } else if (sdl_event_handler.type == SDL_TEXTEDITING && text_input_focused) {
    strncpy(ime_composition, sdl_event_handler.edit.text, sizeof(ime_composition) - 1);
    ime_composition[sizeof(ime_composition) - 1] = '\0';
		    } else if (sdl_event_handler.type == SDL_KEYDOWN) {
    if (sdl_event_handler.key.keysym.sym == SDLK_RETURN && text_input_focused) {
        if (answer_input_len > 0) {
            if (!is_current_user_drawer) {
                send_formatted_message_to_server("MSG", answer_input_buffer); // 서버로 정답 메시지 전송
                printf("[디버깅] 정답 메시지 전송됨: %s\n", answer_input_buffer);
            } else {
                printf("[출제자 차단] 출제자는 정답을 입력할 수 없습니다: %s\n", answer_input_buffer);
            }
            answer_input_buffer[0] = '\0';
            answer_input_len = 0;
        }
    } else if (sdl_event_handler.key.keysym.sym == SDLK_BACKSPACE && text_input_focused) {
        if (answer_input_len > 0) {
            answer_input_buffer[--answer_input_len] = '\0';
        }
    }
}
if (sdl_event_handler.type == SDL_MOUSEBUTTONDOWN) {
    int x = sdl_event_handler.button.x;
    int y = sdl_event_handler.button.y;

    if (!is_current_user_drawer) {
        // 참여자는 입력창 클릭만 가능
        if (x >= 400 && x <= 700 && y >= 10 && y <= 40) {
            text_input_focused = 1;
            SDL_StartTextInput();
        } else {
            text_input_focused = 0;
            SDL_StopTextInput();
        }

        // 참여자는 그림 그리기 불가 → return 없음, 그냥 아무 처리도 안함
    } else {
        // 출제자는 입력창 비활성화
        text_input_focused = 0;
        SDL_StopTextInput();

        // 여기에 그림 그리기 로직 작성
        // drawing = 1;
        // draw_start_x = x; draw_start_y = y; 등
    }
}
                    // 마우스 모션 이벤트 (드래그 중)
                    else if (sdl_event_handler.type == SDL_MOUSEMOTION && local_mouse_is_drawing) {
                        int m_x = sdl_event_handler.motion.x;
                        int m_y = sdl_event_handler.motion.y;
                        if (m_y >= UI_AREA_HEIGHT) { // 그림판 영역 내에서만 그리기
                            // 선 그리기 명령 서버로 전송
                            char line_payload[BUFFER_SIZE];
                            snprintf(line_payload, BUFFER_SIZE, "%d,%d,%d,%d,%d,%d", local_mouse_last_x, local_mouse_last_y, m_x, m_y, current_sdl_color_index, current_pen_size);
                            send_formatted_message_to_server("DRAW_LINE", line_payload);
                            
                            // 로컬 드로잉 명령 버퍼에도 추가하여 즉시 반영
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

                            local_mouse_last_x = m_x; local_mouse_last_y = m_y; // 마지막 위치 업데이트
                        }
                    }
                    // 마우스 버튼 업 이벤트 (그리기 종료)
                    else if (sdl_event_handler.type == SDL_MOUSEBUTTONUP && sdl_event_handler.button.button == SDL_BUTTON_LEFT) {
                        local_mouse_is_drawing = 0;
                        local_mouse_last_x = -1;
                        local_mouse_last_y = -1;
                    }
                // 마우스 버튼 다운 이벤트 (클릭)
                    if (sdl_event_handler.type == SDL_MOUSEBUTTONDOWN && sdl_event_handler.button.button == SDL_BUTTON_LEFT) {
                        int m_x = sdl_event_handler.button.x;
                        int m_y = sdl_event_handler.button.y;
                        if (m_y < UI_AREA_HEIGHT) { // UI 영역 클릭
                            handle_sdl_mouse_click_on_ui(m_x, m_y);
                        } else {
    if (is_current_user_drawer) {
        // 그림판 클릭 (출제자만 가능)
        local_mouse_is_drawing = 1;
        local_mouse_last_x = m_x;
        local_mouse_last_y = m_y;

        // 점 그리기 명령 서버로 전송
        char point_payload[BUFFER_SIZE];
        snprintf(point_payload, BUFFER_SIZE, "%d,%d,%d,%d", m_x, m_y, current_sdl_color_index, current_pen_size);
        send_formatted_message_to_server("DRAW_POINT", point_payload);

        // 로컬 드로잉 명령 버퍼에도 추가하여 즉시 반영
        ReceivedDrawData data = {0}; data.type = 0; data.x1 = m_x; data.y1 = m_y;
        data.color_index = current_sdl_color_index; data.size = current_pen_size;
        pthread_mutex_lock(&received_draw_commands_lock);
        if (received_draw_commands_count >= received_draw_commands_capacity) {
            received_draw_commands_capacity *= 2;
            received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
            if (!received_draw_commands) {
                perror("realloc draw point cmd failed");
                pthread_mutex_unlock(&received_draw_commands_lock);
                main_program_should_run = 0;
                break;
            }
        }
        if (received_draw_commands)
            received_draw_commands[received_draw_commands_count++] = data;
        pthread_mutex_unlock(&received_draw_commands_lock);
    } else {
        printf("[차단] 출제자가 아니므로 그림을 그릴 수 없습니다.\n");
    }
}

                    }
                } // end of SDL_PollEvent loop

                // --- 드로잉 명령 적용 (received_draw_commands 큐 처리) ---
                pthread_mutex_lock(&received_draw_commands_lock);
                if (received_draw_commands_count > 0) {
                    SDL_SetRenderTarget(app_sdl_renderer, app_drawing_canvas_texture); // 캔버스 텍스처에 그림
                    for (int i = 0; i < received_draw_commands_count; i++) {
                        ReceivedDrawData cmd = received_draw_commands[i];
                        if (cmd.type == 0) { // POINT
                            SDL_SetRenderDrawColor(app_sdl_renderer, sdl_colors[cmd.color_index].r, sdl_colors[cmd.color_index].g, sdl_colors[cmd.color_index].b, 255);
                            // UI_AREA_HEIGHT 만큼 위로 조정하여 그림판 영역에 맞게 그리기
                            SDL_Rect pr = {cmd.x1 - cmd.size/2, (cmd.y1 - UI_AREA_HEIGHT) - cmd.size/2, cmd.size, cmd.size};
                            SDL_RenderFillRect(app_sdl_renderer, &pr);
                        } else if (cmd.type == 1) { // LINE
                            SDL_SetRenderDrawColor(app_sdl_renderer, sdl_colors[cmd.color_index].r, sdl_colors[cmd.color_index].g, sdl_colors[cmd.color_index].b, 255);
                            // 펜 두께를 위해 여러 개의 선을 그림 (원형 효과)
                            for (int dx = -cmd.size/2; dx <= cmd.size/2; ++dx) {
                                for (int dy = -cmd.size/2; dy <= cmd.size/2; ++dy) {
                                    // 원형 모양의 펜을 위한 근사치 계산
                                    if(dx*dx + dy*dy <= (cmd.size/2)*(cmd.size/2) +1 ){
                                        // UI_AREA_HEIGHT 만큼 위로 조정
                                        SDL_RenderDrawLine(app_sdl_renderer, cmd.x1 + dx, (cmd.y1 - UI_AREA_HEIGHT) + dy, cmd.x2 + dx, (cmd.y2 - UI_AREA_HEIGHT) + dy);
                                    }
                                }
                            }
                        } else if (cmd.type == 2) { // CLEAR
                            SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 255, 255); // 흰색으로 지움
                            SDL_RenderClear(app_sdl_renderer);
                        }
                    }
                    received_draw_commands_count = 0; // 처리 완료된 명령 초기화
                    SDL_SetRenderTarget(app_sdl_renderer, NULL); // 다시 기본 렌더 타겟으로 설정
                }
                pthread_mutex_unlock(&received_draw_commands_lock);

                // --- 화면 렌더링 ---
                SDL_SetRenderDrawColor(app_sdl_renderer, 0, 0, 0, 255); // 검정색으로 배경 초기화
                SDL_RenderClear(app_sdl_renderer);

                // 캔버스 텍스처를 화면에 그리기
                SDL_Rect canvas_dest_rect = {0, UI_AREA_HEIGHT, WINDOW_WIDTH, WINDOW_HEIGHT - UI_AREA_HEIGHT};
                SDL_RenderCopy(app_sdl_renderer, app_drawing_canvas_texture, NULL, &canvas_dest_rect);
                
                render_sdl_ui_elements(); // UI 요소 그리기

                SDL_RenderPresent(app_sdl_renderer); // 화면 업데이트

                // 프레임 레이트 조절 (60 FPS)
                Uint32 frame_time = SDL_GetTicks() - sdl_frame_start_tick;
                if (frame_time < 1000 / 60) {
                    SDL_Delay((1000 / 60) - frame_time);
                }
            } // end of if (sdl_render_loop_running)
        } else {
            // SDL UI가 비활성화된 경우 (로비 상태 등) 터미널 입력 처리
            // non-blocking하게 터미널 입력 확인
            fd_set read_fds;
            struct timeval tv;

            FD_ZERO(&read_fds);
            FD_SET(STDIN_FILENO, &read_fds); // 표준 입력 감지

            // 짧은 시간만 대기하여 CPU 점유율을 너무 높이지 않음
            tv.tv_sec = 0;
            tv.tv_usec = 10000; // 10ms

            int select_result = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &tv);

            if (select_result < 0) {
                if (errno == EINTR) continue; // 인터럽트 된 경우 재시도
                perror("select error");
                main_program_should_run = 0;
                break;
            } else if (select_result > 0) {
                if (FD_ISSET(STDIN_FILENO, &read_fds)) {
                    if (fgets(terminal_input_line, BUFFER_SIZE, stdin) != NULL) {
                        terminal_input_line[strcspn(terminal_input_line, "\n")] = 0; // 개행 문자 제거

                        if (strlen(terminal_input_line) == 0) {
                            printf("> ");
                            fflush(stdout);
                            continue;
                        }
                        
                        if (strncmp(terminal_input_line, "/nick ", 6) == 0) {
                            send_formatted_message_to_server("NICK", terminal_input_line + 6);
                        } else if (strncmp(terminal_input_line, "/create ", 8) == 0) {
                            send_formatted_message_to_server("CREATE_ROOM", terminal_input_line + 8);
                        } else if (strncmp(terminal_input_line, "/join ", 6) == 0) {
                            send_formatted_message_to_server("JOIN_ROOM", terminal_input_line + 6);
                        } else if (strcmp(terminal_input_line, "/exit") == 0) {
                            send_formatted_message_to_server("EXIT_ROOM", NULL);
                        } else if (strcmp(terminal_input_line, "/list") == 0) {
                            send_formatted_message_to_server("LIST_ROOMS", NULL);
                        } else if (strcmp(terminal_input_line, "/quit") == 0) {
                            main_program_should_run = 0; // 프로그램 종료 플래그
                            break;
                        }
                        else if (strncmp(terminal_input_line, "/msg ", 5) == 0) {
                             send_formatted_message_to_server("MSG", terminal_input_line + 5);
                        }
                        else if (app_current_state == STATE_IN_ROOM && client_is_nick_set) {
                            printf("\r[클라이언트] 현재 그림판 UI 활성 상태입니다. 채팅은 UI에서 직접 입력해주세요.\n> ");
                        }
                        else {
                            printf("\r[클라이언트] 알 수 없는 명령이거나, 현재 상태에서 사용할 수 없는 명령입니다.\n> ");
                        }
                        printf("> "); // 새로운 프롬프트 출력
                        fflush(stdout);
                    } else {
                        // EOF (Ctrl+D) 또는 읽기 오류 시 종료
                        main_program_should_run = 0;
                    }
                }
            }
            // SDL UI가 비활성화된 동안에는 CPU를 조금 더 양보
            SDL_Delay(10); 
        }
        // SDL UI 활성화 상태가 바뀌었는데 아직 SDL이 동작 중이라면 정리
        if (!sdl_should_be_active && sdl_render_loop_running) {
            printf("[클라이언트:메인] SDL UI 비활성화 요청 감지. 종료 함수 호출.\n");
            shutdown_sdl_environment(); // SDL 자원 해제
        }
    } // end of while (main_program_should_run)

    // 5. 프로그램 종료 정리
    printf("[클라이언트:메인] 프로그램 종료 중...\n");
    // 수신 스레드가 종료될 때까지 대기
    pthread_join(server_message_receiver_tid, NULL);
    
    // 남은 SDL 자원 확실히 해제 (이중 호출 방지 로직 있음)
    shutdown_sdl_environment();

    // 드로잉 명령 버퍼 메모리 해제
    if (received_draw_commands) {
        free(received_draw_commands);
        received_draw_commands = NULL;
    }
    pthread_mutex_destroy(&received_draw_commands_lock);

    // 소켓 닫기
    close(client_socket_fd);
    printf("[클라이언트:메인] 모든 자원 해제 완료. 종료.\n");

    return 0;
}