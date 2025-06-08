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

// ���� �� ���� ����
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 12345
#define BUFFER_SIZE 1024
#define NICK_SIZE 32
#define ROOM_NAME_SIZE 32
#define INPUT_BUFFER_SIZE (BUFFER_SIZE * 2)

// SDL UI ����
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define UI_AREA_HEIGHT 80

// �׸��� ���� �ȷ�Ʈ
SDL_Color sdl_colors[] = {
	{0, 0, 0, 255},       // ����
	{255, 0, 0, 255},     // ����
	{0, 255, 0, 255},     // �ʷ�
	{0, 0, 255, 255},     // �Ķ�
	{255, 255, 255, 255}  // ��� (���찳 �뵵)
};
int current_sdl_color_index = 0; // ���� ���õ� ���� �ε���
int current_pen_size = 5;        // ���� �� ũ��
volatile int text_input_focused = 0; // �ؽ�Ʈ �Է�â ��Ŀ�� ����

// Ŭ���̾�Ʈ ����
typedef enum {
	STATE_LOBBY,    // �κ� ����
	STATE_IN_ROOM   // �� �ȿ� �ִ� ����
} ClientAppState;

volatile ClientAppState app_current_state = STATE_LOBBY; // ���� Ŭ���̾�Ʈ �� ����
int client_socket_fd;                           // Ŭ���̾�Ʈ ���� ���� ��ũ����
char client_nickname[NICK_SIZE] = "�͸�";       // Ŭ���̾�Ʈ �г���
volatile int client_is_nick_set = 0;            // �г��� ���� ����
int client_current_room_id = -1;                // ���� ������ �� ID
char client_current_room_name[ROOM_NAME_SIZE] = ""; // ���� ������ �� �̸�
char answer_input_buffer[BUFFER_SIZE] = "";     // ���� �Է� ����
int answer_input_len = 0;                       // ���� �Է� ���� ����
char ime_composition[BUFFER_SIZE] = "";
int ime_cursor = 0;
Uint32 round_start_tick = 0; // ���� ���� �ð� (ms ����)
int round_duration_seconds = 60; // ���� �ð� 

// ���� �޽��� ���� ����
char server_message_buffer[INPUT_BUFFER_SIZE];
int server_message_len = 0;
int is_current_user_drawer = 0;

// SDL ���� ���� ����
SDL_Window* app_sdl_window = NULL;
SDL_Renderer* app_sdl_renderer = NULL;
SDL_Texture* app_drawing_canvas_texture = NULL; // �׸� �׸��� ĵ���� �ؽ�ó
TTF_Font* app_ui_font = NULL;                   // UI �ؽ�Ʈ�� ��Ʈ
volatile int sdl_should_be_active = 0;          // SDL UI Ȱ��ȭ �ʿ� �÷���
volatile int sdl_render_loop_running = 0;       // SDL ������ ���� ���� �� ����
volatile int main_program_should_run = 1;       // ���� ���α׷� ���� �÷���

// ���� ����׿� ����
int local_mouse_is_drawing = 0;
int local_mouse_last_x = -1;
int local_mouse_last_y = -1;

// ������ �׸� �����͸� �����ϴ� ����ü
typedef struct {
	int type; // 0: POINT, 1: LINE, 2: CLEAR
	int x1, y1, x2, y2, color_index, size;
} ReceivedDrawData;

ReceivedDrawData* received_draw_commands = NULL; // ���ŵ� ����� ��� �迭
int received_draw_commands_count = 0;            // ���ŵ� ����� ��� ����
int received_draw_commands_capacity = 20;        // �ʱ� �뷮
pthread_mutex_t received_draw_commands_lock;     // ����� ��� ���� ��ȣ ���ؽ�

// ������
pthread_t server_message_receiver_tid; // ���� �޽��� ���� ������ ID

// --- �Լ� ������Ÿ�� ---
void send_formatted_message_to_server(const char* type_or_full_cmd, const char* payload);
void* server_message_receiver_thread_func(void* arg);
void initialize_sdl_environment();
void shutdown_sdl_environment();
void render_sdl_ui_elements();
void handle_sdl_mouse_click_on_ui(int x, int y);
void client_side_clear_canvas_and_notify_server();
void draw_text(SDL_Renderer* renderer, const char* text, int x, int y, SDL_Color color) {
	extern TTF_Font* app_ui_font; // ���� ��Ʈ
	SDL_Surface* surface = TTF_RenderUTF8_Blended(app_ui_font, text, color);
	SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_Rect dst = { x, y, surface->w, surface->h };
	SDL_RenderCopy(renderer, texture, NULL, &dst);
	SDL_FreeSurface(surface);
	SDL_DestroyTexture(texture);
}
// ���� ��� ǥ�ÿ� ���� (�߰���)
char sdl_answer_result_text[BUFFER_SIZE] = "";
Uint32 answer_result_display_tick = 0; // �޽��� ǥ�� ���� �ð� (SDL_GetTicks() ��)

// --- �޽��� ���� �Լ� ---
void send_formatted_message_to_server(const char* type_or_full_cmd, const char* payload) {
	char final_message_to_send[BUFFER_SIZE];
	if (payload && strlen(payload) > 0) {
		snprintf(final_message_to_send, BUFFER_SIZE, "%s:%s\n", type_or_full_cmd, payload);
	}
	else {
		snprintf(final_message_to_send, BUFFER_SIZE, "%s\n", type_or_full_cmd);
	}
	if (send(client_socket_fd, final_message_to_send, strlen(final_message_to_send), 0) < 0) {
		perror("send to server failed");
	}
}

// --- ���� �޽��� ���� ������ ---
void* server_message_receiver_thread_func(void* arg) {
	char temp_chunk_buffer[BUFFER_SIZE];
	char sdl_answer_result_text[128] = "";
	char server_response_line[INPUT_BUFFER_SIZE];
	Uint32 answer_result_display_tick = 0;

	int bytes_read;
	printf("[Ŭ���̾�Ʈ:���Ž�����] ���۵�.\n");

	while (main_program_should_run) {
		bytes_read = recv(client_socket_fd, temp_chunk_buffer, BUFFER_SIZE - 1, 0);
		if (bytes_read <= 0) {
			if (bytes_read == 0) printf("\r[Ŭ���̾�Ʈ:���Ž�����] ���� ���� ���� �����.\n> ");
			else perror("\r[Ŭ���̾�Ʈ:���Ž�����] ���� �޽��� ���� ����");
			main_program_should_run = 0;
			Uint32 now = SDL_GetTicks();
			if (now - answer_result_display_tick < 2000 && strlen(sdl_answer_result_text) > 0) {
				SDL_Color color;
				if (strstr(sdl_answer_result_text, "�����Դϴ�!")) {
					color = (SDL_Color){ 0, 255, 0 };  // �ʷϻ�
				}
				else {
					color = (SDL_Color){ 255, 0, 0 };  // ������
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
				printf("[Ŭ���̾�Ʈ:TIMER] %d�ʷ� ������\n", time_left);
			}


			if (sdl_render_loop_running) sdl_should_be_active = 0; // SDL ���� ���� ����
			break;
		}
		temp_chunk_buffer[bytes_read] = '\0'; // �� ����

		// ���� �����÷ο� ���� �� �޽��� �߰�
		if (server_message_len + bytes_read < INPUT_BUFFER_SIZE) {
			strncat(server_message_buffer, temp_chunk_buffer, bytes_read);
			server_message_len += bytes_read;
		}
		else {
			fprintf(stderr, "\r[Ŭ���̾�Ʈ:���Ž�����] ���� ���� �����÷ο�. ���� �ʱ�ȭ.\n> ");
			server_message_len = 0; memset(server_message_buffer, 0, INPUT_BUFFER_SIZE);
			continue; // ���� ���� �õ�
		}

		// ���� ������ �޽��� ó��
		char* current_line_ptr = server_message_buffer;
		char* newline_char_found;
		while ((newline_char_found = strchr(current_line_ptr, '\n')) != NULL) {
			*newline_char_found = '\0'; // ���� ���ڸ� �� ���ڷ� �ٲ� ���� �� ǥ��
			strcpy(server_response_line, current_line_ptr); // ���� ���� ����
			if (strstr(server_response_line, "�����Դϴ�!") != NULL) {
				round_start_tick = SDL_GetTicks();
				round_duration_seconds = 60;  // Ȥ�� �������� ���޹��� �ð� ���
			}
			if (strncmp(server_response_line, "SMSG:���þ�� '", 15) == 0) {
				is_current_user_drawer = 1;
			}
			else if (strstr(server_response_line, "���� �������Դϴ�.") != NULL) {
				is_current_user_drawer = strstr(server_response_line, client_nickname) != NULL;
			}
			else if (strncmp(server_response_line, "TIMER:", 6) == 0) {
				int time_left = atoi(server_response_line + 6);
				round_start_tick = SDL_GetTicks();
				round_duration_seconds = time_left;
			}


			// �Ϲ� �޽��� ��� (ȭ�鿡 ǥ��)
			if (strncmp(server_response_line, "MSG:", 4) == 0) {
				printf("\r%s\n> ", server_response_line + 4);
			}
			// �׸��� �޽���(DRAW_*)�� �ƴϸ鼭 �ý��� �޽���/����/�� ����� ���
			else if (strncmp(server_response_line, "DRAW_", 5) != 0) {
				if (strncmp(server_response_line, "SMSG:��", 7) == 0 || strncmp(server_response_line, "SMSG:�г�����", 10) == 0) {
					printf("\r[Ŭ���̾�Ʈ �ý���] %s\n> ", server_response_line);
				}
				else if (strncmp(server_response_line, "ROOM_EVENT:", 11) == 0) {
					printf("\r%s\n> ", server_response_line + 11);
				}
				else if (strncmp(server_response_line, "SMSG:", 5) == 0 || strncmp(server_response_line, "ERR_", 4) == 0 || strncmp(server_response_line, "ROOMLIST:", 9) == 0) {
					printf("\r[���� ����] %s\n> ", server_response_line);
				}
			}
			fflush(stdout); // ��� �͹̳ο� ���

			// === Ŭ���̾�Ʈ ���� ��ȭ ó�� ===
			if (strncmp(server_response_line, "SMSG:", 5) == 0) {
				const char* smsg_payload_content = server_response_line + 5;
				char temp_room_name[ROOM_NAME_SIZE];
				int temp_room_id;

				// �г��� ���� Ȯ��
				if (strstr(smsg_payload_content, "�г����� �����Ǿ����ϴ�.") || strstr(smsg_payload_content, "(��)�� �����Ǿ����ϴ�.")) {
					char confirmed_nickname_str[NICK_SIZE];
					if (sscanf(smsg_payload_content, "�г����� '%[^']'(��)�� �����Ǿ����ϴ�.", confirmed_nickname_str) == 1) {
						strcpy(client_nickname, confirmed_nickname_str);
						client_is_nick_set = 1;
					}
				}
				// �� ����/���� Ȯ��
				else if (sscanf(smsg_payload_content, "�� '%[^']'(ID:%d)�� �����߽��ϴ�.", temp_room_name, &temp_room_id) == 2 ||
					sscanf(smsg_payload_content, "�� '%[^']'(ID:%d)��(��) �����Ǿ��� �����߽��ϴ�.", temp_room_name, &temp_room_id) == 2) {
					strcpy(client_current_room_name, temp_room_name);
					client_current_room_id = temp_room_id;
					app_current_state = STATE_IN_ROOM;
					if (client_is_nick_set) sdl_should_be_active = 1; // �г��� �����Ǿ����� SDL UI Ȱ��ȭ ��û
				}
				// �� ���� Ȯ��
				else if (strstr(smsg_payload_content, "�濡�� �����߽��ϴ�.")) {
					app_current_state = STATE_LOBBY;
					client_current_room_id = -1;
					strcpy(client_current_room_name, "");
					if (sdl_render_loop_running) sdl_should_be_active = 0; // SDL ���� ���� ����
				}
				// ����/���� �޽��� ó�� (�߰���)
				else if (strncmp(smsg_payload_content, "�����Դϴ�", 12) == 0 ||
					strncmp(smsg_payload_content, "�����Դϴ�", 13) == 0) {
					strncpy(sdl_answer_result_text, smsg_payload_content, sizeof(sdl_answer_result_text) - 1);
					sdl_answer_result_text[sizeof(sdl_answer_result_text) - 1] = '\0';
					answer_result_display_tick = SDL_GetTicks(); // �޽��� ǥ�� ���� �ð� ���
				}
			}

			// === ����� ��� ó�� (���� ����� �����͸� ť�� �߰�) ===
			else if (strncmp(server_response_line, "DRAW_POINT:", 11) == 0) {
				ReceivedDrawData data = { 0 };
				data.type = 0; // POINT
				if (sscanf(server_response_line + 11, "%d,%d,%d,%d", &data.x1, &data.y1, &data.color_index, &data.size) == 4) {
					pthread_mutex_lock(&received_draw_commands_lock);
					if (received_draw_commands_count >= received_draw_commands_capacity) {
						received_draw_commands_capacity *= 2;
						received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
						if (!received_draw_commands) { perror("realloc draw point failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; break; }
					}
					received_draw_commands[received_draw_commands_count++] = data;
					pthread_mutex_unlock(&received_draw_commands_lock);
				}
			}
			else if (strncmp(server_response_line, "DRAW_LINE:", 10) == 0) {
				ReceivedDrawData data = { 0 };
				data.type = 1; // LINE
				if (sscanf(server_response_line + 10, "%d,%d,%d,%d,%d,%d", &data.x1, &data.y1, &data.x2, &data.y2, &data.color_index, &data.size) == 6) {
					pthread_mutex_lock(&received_draw_commands_lock);
					if (received_draw_commands_count >= received_draw_commands_capacity) {
						received_draw_commands_capacity *= 2;
						received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
						if (!received_draw_commands) { perror("realloc draw line failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; break; }
					}
					received_draw_commands[received_draw_commands_count++] = data;
					pthread_mutex_unlock(&received_draw_commands_lock);
				}
			}
			else if (strcmp(server_response_line, "DRAW_CLEAR") == 0) {
				ReceivedDrawData data = { 0 };
				data.type = 2; // CLEAR ���
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
			}
			else if (strcmp(server_response_line, "CLEAR") == 0) {
				ReceivedDrawData data = { 0 };
				data.type = 2; // CLEAR
				pthread_mutex_lock(&received_draw_commands_lock);
				if (received_draw_commands_count >= received_draw_commands_capacity) {
					received_draw_commands_capacity *= 2;
					received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
					if (!received_draw_commands) { perror("realloc draw clear failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; break; }
				}
				received_draw_commands[received_draw_commands_count++] = data;
				pthread_mutex_unlock(&received_draw_commands_lock);
			}
			current_line_ptr = newline_char_found + 1; // ���� ������ ���� ���� ����
		}
		// ó������ ���� �ܿ� �����Ͱ� �ִٸ� ������ ������ �̵�
		if (current_line_ptr > server_message_buffer && strlen(current_line_ptr) > 0) {
			char temp_buf[INPUT_BUFFER_SIZE]; strcpy(temp_buf, current_line_ptr);
			memset(server_message_buffer, 0, INPUT_BUFFER_SIZE); strcpy(server_message_buffer, temp_buf);
			server_message_len = strlen(server_message_buffer);
		}
		else {
			// ��� ���� ó�� �Ϸ� �Ǵ� �ܿ� ������ ����
			server_message_len = 0; memset(server_message_buffer, 0, INPUT_BUFFER_SIZE);
		}
	}
	printf("[Ŭ���̾�Ʈ:���Ž�����] �����.\n");
	return NULL;
}


// --- SDL ȯ�� �ʱ�ȭ ---
void initialize_sdl_environment() {
	if (sdl_render_loop_running || app_sdl_window) {
		// �̹� ���� ���̰ų� �����찡 �����ϸ� �ٽ� �ʱ�ȭ���� ����
		return;
	}
	printf("[Ŭ���̾�Ʈ:SDL] UI �ʱ�ȭ ���� (��: '%s', �г���: '%s')\n", client_current_room_name, client_nickname);

	if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL Init ����: %s\n", SDL_GetError()); main_program_should_run = 0; return; }
	if (TTF_Init() == -1) { fprintf(stderr, "TTF Init ����: %s (��� ����)\n", TTF_GetError()); }

	// ��Ʈ �ε�
	app_ui_font = TTF_OpenFont("DejaVuSans.ttf", 16);
	if (!app_ui_font) {
		fprintf(stderr, "��Ʈ �ε� ����: %s\n", TTF_GetError());
	}


	// ������ Ÿ��Ʋ ����
	char win_title[128];
	snprintf(win_title, sizeof(win_title), "�׸��� - ��: %s (%s)", client_current_room_name, client_nickname);
	app_sdl_window = SDL_CreateWindow(win_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
	if (!app_sdl_window) { fprintf(stderr, "������ ���� ����: %s\n", SDL_GetError()); main_program_should_run = 0; SDL_Quit(); return; }

	// ������ ����
	app_sdl_renderer = SDL_CreateRenderer(app_sdl_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!app_sdl_renderer) { SDL_DestroyWindow(app_sdl_window); app_sdl_window = NULL; fprintf(stderr, "������ ���� ����: %s\n", SDL_GetError()); main_program_should_run = 0; SDL_Quit(); return; }

	// �׸� �׸��� ĵ���� �ؽ�ó ���� (UI ���� ����)
	app_drawing_canvas_texture = SDL_CreateTexture(app_sdl_renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, WINDOW_WIDTH, WINDOW_HEIGHT - UI_AREA_HEIGHT);
	if (!app_drawing_canvas_texture) { fprintf(stderr, "ĵ���� �ؽ�ó ���� ����: %s\n", SDL_GetError()); shutdown_sdl_environment(); main_program_should_run = 0; return; }

	// ĵ���� �ʱ�ȭ (������� ä��)
	SDL_SetRenderTarget(app_sdl_renderer, app_drawing_canvas_texture);
	SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 255, 255); // ���
	SDL_RenderClear(app_sdl_renderer);
	SDL_SetRenderTarget(app_sdl_renderer, NULL); // �ٽ� �⺻ ���� Ÿ������ ����

	sdl_render_loop_running = 1; // SDL ������ ���� ���� ���·� ����
	printf("[Ŭ���̾�Ʈ:SDL] UI �ʱ�ȭ �Ϸ�, ���� ���� �غ�� (sdl_render_loop_running = %d).\n", sdl_render_loop_running);
	SDL_ShowWindow(app_sdl_window); // �����츦 ������
	SDL_RaiseWindow(app_sdl_window); // �����츦 ���׶���� ������
	SDL_StartTextInput();            // �ؽ�Ʈ �Է� Ȱ��ȭ (�Է�â ��Ŀ�� �� ���)
}

// --- SDL ȯ�� ���� ---
void shutdown_sdl_environment() {
	// �̹� ����Ǿ��ų� �ʱ�ȭ���� �ʾҴٸ� �ٷ� ����
	if (!app_sdl_window && !app_sdl_renderer && !app_drawing_canvas_texture && !sdl_render_loop_running && !app_ui_font) {
		return;
	}
	printf("[Ŭ���̾�Ʈ:SDL] UI ���� ����...\n");
	if (app_drawing_canvas_texture) { SDL_DestroyTexture(app_drawing_canvas_texture); app_drawing_canvas_texture = NULL; }
	if (app_sdl_renderer) { SDL_DestroyRenderer(app_sdl_renderer); app_sdl_renderer = NULL; }
	if (app_sdl_window) { SDL_DestroyWindow(app_sdl_window); app_sdl_window = NULL; }
	if (app_ui_font) { TTF_CloseFont(app_ui_font); app_ui_font = NULL; }

	SDL_StopTextInput(); // �ؽ�Ʈ �Է� ��Ȱ��ȭ
	TTF_Quit(); // TTF ����
	SDL_Quit(); // SDL ����

	sdl_render_loop_running = 0;
	sdl_should_be_active = 0;
	printf("[Ŭ���̾�Ʈ:SDL] UI �ڿ� ���� �Ϸ� (sdl_render_loop_running = %d, sdl_should_be_active = %d).\n", sdl_render_loop_running, sdl_should_be_active);
}

// --- SDL UI ��� ������ ---
void render_sdl_ui_elements() {
	Uint32 now = SDL_GetTicks();
	int seconds_passed = (now - round_start_tick) / 1000;
	int seconds_left = round_duration_seconds - seconds_passed;

	if (!app_sdl_renderer) return;

	// UI ���
	SDL_Rect ui_bg_rect = { 0, 0, WINDOW_WIDTH, UI_AREA_HEIGHT };
	SDL_SetRenderDrawColor(app_sdl_renderer, 220, 220, 220, 255);
	SDL_RenderFillRect(app_sdl_renderer, &ui_bg_rect);

	// ���� �ȷ�Ʈ ��ư
	for (int i = 0; i < 5; i++) {
		SDL_Rect btn = { 10 + i * 45, 10, 40, 30 };
		SDL_SetRenderDrawColor(app_sdl_renderer, sdl_colors[i].r, sdl_colors[i].g, sdl_colors[i].b, 255);
		SDL_RenderFillRect(app_sdl_renderer, &btn);
		if (i == current_sdl_color_index) {
			SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 0, 255);
			SDL_RenderDrawRect(app_sdl_renderer, &btn);
		}
	}

	// �� ���� ��ư
	int pen_sizes_options[] = { 3, 8, 15 };
	for (int i = 0; i < 3; i++) {
		SDL_Rect btn = { 10 + i * 45, 45, 40, 30 };
		SDL_SetRenderDrawColor(app_sdl_renderer, 180 - i * 10, 180 - i * 10, 180 - i * 10, 255);
		SDL_RenderFillRect(app_sdl_renderer, &btn);
		if (pen_sizes_options[i] == current_pen_size) {
			SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 0, 255);
			SDL_RenderDrawRect(app_sdl_renderer, &btn);
		}
	}

	// ��ü ����� ��ư
	SDL_Rect clear_btn = { 10 + 3 * 45, 45, 100, 30 };
	SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 255, 255); // ���
	SDL_RenderFillRect(app_sdl_renderer, &clear_btn);
	SDL_SetRenderDrawColor(app_sdl_renderer, 0, 0, 0, 255); // ����
	SDL_RenderDrawRect(app_sdl_renderer, &clear_btn);
	SDL_Color black = { 0, 0, 0, 255 };  // ������
	draw_text(app_sdl_renderer, "All Clear", clear_btn.x + 10, clear_btn.y + 5, black);

	// ���� ��ư
	SDL_Rect submit_btn = { 710, 10, 80, 30 };
	SDL_SetRenderDrawColor(app_sdl_renderer, 100, 200, 100, 255);
	SDL_RenderFillRect(app_sdl_renderer, &submit_btn);

	if (app_ui_font) {
		SDL_Color text_color = { 0, 0, 0 };
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

	// ���� �Է�â
	SDL_Rect answer_box = { 400, 10, 300, 30 };
	SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 255, 255);
	SDL_RenderFillRect(app_sdl_renderer, &answer_box);

	if (app_ui_font) {
		SDL_Color text_color = { 0, 0, 0 };
		char full_text_with_ime[BUFFER_SIZE * 2];
		snprintf(full_text_with_ime, sizeof(full_text_with_ime), "%s%s", answer_input_buffer, ime_composition);

		SDL_Surface* text_surface = TTF_RenderUTF8_Blended(app_ui_font, full_text_with_ime, text_color);
		if (text_surface) {
			SDL_Texture* text_texture = SDL_CreateTextureFromSurface(app_sdl_renderer, text_surface);
			SDL_Rect text_dest = { answer_box.x + 5, answer_box.y + 5, text_surface->w, text_surface->h };
			SDL_RenderCopy(app_sdl_renderer, text_texture, NULL, &text_dest);
			SDL_FreeSurface(text_surface);
			SDL_DestroyTexture(text_texture);
		}

		if (text_input_focused) {
			SDL_SetRenderDrawColor(app_sdl_renderer, 0, 0, 255, 255);
			SDL_RenderDrawRect(app_sdl_renderer, &answer_box);
		}
	}

	// ����/���� ��� �ؽ�Ʈ ���
	if (strlen(sdl_answer_result_text) > 0 && SDL_GetTicks() - answer_result_display_tick < 3000) {
		SDL_Color color = { 0, 0, 0, 255 };
		if (strstr(sdl_answer_result_text, "O")) color = (SDL_Color){ 0, 160, 0, 255 };
		else if (strstr(sdl_answer_result_text, "X")) color = (SDL_Color){ 200, 0, 0, 255 };

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
	//Ÿ�̸� ǥ��
	if (round_start_tick > 0 && round_duration_seconds > 0 && app_ui_font) {
		Uint32 now = SDL_GetTicks();
		int seconds_passed = (now - round_start_tick) / 1000;
		int seconds_left = round_duration_seconds - seconds_passed;
		if (seconds_left < 0) seconds_left = 0;

		char timer_text[64];
		snprintf(timer_text, sizeof(timer_text), "TIMER : %dsec", seconds_left);

		SDL_Color text_color = { 30, 144, 255, 255 };  // �Ķ���

		SDL_Surface* surface = TTF_RenderUTF8_Blended(app_ui_font, timer_text, text_color);
		if (surface) {
			SDL_Texture* texture = SDL_CreateTextureFromSurface(app_sdl_renderer, surface);
			if (texture) {
				int tex_w = surface->w;
				int tex_h = surface->h;

				// ��ġ�� ��ġ ���� (�߾� ���)
				int text_x = (WINDOW_WIDTH - tex_w) / 2;
				int text_y = 45; // �ʹ� ���� ���� ��ư�̶� ��ġ�� �ణ ����

				// ������ ��� �簢��
				SDL_SetRenderDrawColor(app_sdl_renderer, 0, 0, 0, 160); // ���� ������
				SDL_SetRenderDrawBlendMode(app_sdl_renderer, SDL_BLENDMODE_BLEND);
				SDL_Rect bg_rect = { text_x - 10, text_y - 5, tex_w + 20, tex_h + 10 };
				SDL_RenderFillRect(app_sdl_renderer, &bg_rect);
				SDL_SetRenderDrawBlendMode(app_sdl_renderer, SDL_BLENDMODE_NONE);

				// �ؽ�Ʈ ������
				SDL_Rect text_rect = { text_x, text_y, tex_w, tex_h };
				SDL_RenderCopy(app_sdl_renderer, texture, NULL, &text_rect);

				SDL_DestroyTexture(texture);
			}
			SDL_FreeSurface(surface);
		}
	}

}


// --- SDL UI ���콺 Ŭ�� ó�� ---
void handle_sdl_mouse_click_on_ui(int x, int y) {
	// ���� ��ư Ŭ�� ó��
	for (int i = 0; i < 5; i++) {
		if (x >= 10 + i * 45 && x <= 10 + i * 45 + 40 && y >= 10 && y <= 10 + 30) {
			current_sdl_color_index = i;
			return;
		}
	}
	// �� ũ�� ��ư Ŭ�� ó��
	int pen_sizes_opt[] = { 3, 8, 15 };
	for (int i = 0; i < 3; i++) {
		if (x >= 10 + i * 45 && x <= 10 + i * 45 + 40 && y >= 45 && y <= 45 + 30) {
			current_pen_size = pen_sizes_opt[i];
			return;
		}
	}
	// ���찳/Ŭ���� ��ư Ŭ�� ó��
	if (x >= 10 + 3 * 45 && x <= 10 + 3 * 45 + 100 && y >= 45 && y <= 45 + 30) {
		if (!is_current_user_drawer) {
			printf("[������ �ƴ�] ��ü ����� ��ư�� �����ڸ� ����� �� �ֽ��ϴ�.\n");
			return;
		}
		client_side_clear_canvas_and_notify_server();
		return;
	}


	// ���� �Է�â Ŭ�� ó�� 
	SDL_Rect answer_box = { 400, 10, 300, 30 }; // �Է�â ��ġ ����

	if (x >= answer_box.x && x <= answer_box.x + answer_box.w &&
		y >= answer_box.y && y <= answer_box.y + answer_box.h) {
		text_input_focused = 1;
		SDL_StartTextInput();  // �ؽ�Ʈ �Է� Ȱ��ȭ
		//printf("[�����] �Է�â ��Ŀ�� ON\n");
	}
	else {
		text_input_focused = 0;
		SDL_StopTextInput();   // �ؽ�Ʈ �Է� ��Ȱ��ȭ
	}
	SDL_Rect submit_btn = { 710, 10, 80, 30 };
	if (x >= 710 && x <= 710 + 80 && y >= 10 && y <= 10 + 30) {
		if (answer_input_len > 0) {
			send_formatted_message_to_server("MSG", answer_input_buffer);
			answer_input_buffer[0] = '\0';
			answer_input_len = 0;
		}
		return;
	}
}

// --- ĵ���� ����� �� ������ �˸� ---
void client_side_clear_canvas_and_notify_server() {
	if (!app_drawing_canvas_texture || !app_sdl_renderer) return;

	// ���� ����� ��� ���ۿ� CLEAR ��� �߰� (��� ȭ�鿡 �ݿ� ����)
	ReceivedDrawData data = { 0 };
	data.type = 2; // CLEAR
	pthread_mutex_lock(&received_draw_commands_lock);
	if (received_draw_commands_count >= received_draw_commands_capacity) {
		received_draw_commands_capacity *= 2;
		received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
		if (!received_draw_commands) { perror("realloc clear cmd failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; return; }
	}
	if (received_draw_commands) received_draw_commands[received_draw_commands_count++] = data;
	pthread_mutex_unlock(&received_draw_commands_lock);

	send_formatted_message_to_server("DRAW_CLEAR", NULL); // ������ ĵ���� ����� ��� ����
}

// --- ���� �Լ� ---
int main(int argc, char* argv[]) {
	// 1. ���� ���� �� ���� ����
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
	printf("[Ŭ���̾�Ʈ:����] ���� ���� ���� (IP: %s, Port: %d)\n", SERVER_IP, SERVER_PORT);

	// 2. ���� �޽��� ���� ������ ����
	if (pthread_create(&server_message_receiver_tid, NULL, server_message_receiver_thread_func, NULL) != 0) {
		perror("���� ������ ���� ����"); close(client_socket_fd); exit(1);
	}

	// 3. ����� ��� ���� �� ���ؽ� �ʱ�ȭ
	pthread_mutex_init(&received_draw_commands_lock, NULL);
	received_draw_commands = malloc(received_draw_commands_capacity * sizeof(ReceivedDrawData));
	if (received_draw_commands == NULL) {
		perror("[Ŭ���̾�Ʈ:����] received_draw_commands �޸� �Ҵ� ����");
		exit(1);
	}

	// 4. �͹̳� �Է� �� SDL �̺�Ʈ ����
	char terminal_input_line[BUFFER_SIZE];
	printf("> "); fflush(stdout); // �ʱ� ������Ʈ ���

	SDL_Event sdl_event_handler;
	Uint32 sdl_frame_start_tick; // ������ ���� �ð� ������

	while (main_program_should_run) {
		// SDL UI Ȱ��ȭ ����: �濡 �����߰�, �г����� �����Ǿ���, SDL Ȱ��ȭ �÷��װ� true�� ��
		if (app_current_state == STATE_IN_ROOM && client_is_nick_set && sdl_should_be_active) {
			// SDL UI�� ���� ���� ���� �ƴϸ� �ʱ�ȭ �õ�
			if (!sdl_render_loop_running) {
				initialize_sdl_environment();
				if (!sdl_render_loop_running) { // �ʱ�ȭ ���� ��
					printf("[Ŭ���̾�Ʈ:����] SDL �ʱ�ȭ ����. �濡�� ���� ��û.\n");
					sdl_should_be_active = 0; // SDL Ȱ��ȭ �ߴ�
					if (app_current_state == STATE_IN_ROOM) {
						send_formatted_message_to_server("EXIT_ROOM", NULL); // ������ �� ���� �˸�
					}
				}
			}

			// SDL ������ ���� (UI�� Ȱ��ȭ�� ���)
			if (sdl_render_loop_running) {
				sdl_frame_start_tick = SDL_GetTicks(); // ���� �ð� ���

				// SDL �̺�Ʈ ó�� ����
				while (SDL_PollEvent(&sdl_event_handler)) {
					if (sdl_event_handler.type == SDL_QUIT) { // â �ݱ� ��ư Ŭ��
						printf("[Ŭ���̾�Ʈ:SDL�̺�Ʈ] ���� �̺�Ʈ (X ��ư). EXIT_ROOM ����.\n");
						send_formatted_message_to_server("EXIT_ROOM", NULL); // ������ �� ���� �˸�
						// sdl_should_be_active = 0; // ���� ������ ��ٷ� ���� �����ϹǷ� �ּ� ó��
						break; // �̺�Ʈ ���� ����
					}

					//  �ؽ�Ʈ �Է� ó�� (�߰���)
					if (sdl_event_handler.type == SDL_TEXTINPUT && text_input_focused) {
						int len = strlen(sdl_event_handler.text.text);
						if (answer_input_len + len < BUFFER_SIZE - 1) { // ���� �����÷ο� ����
							strcat(answer_input_buffer, sdl_event_handler.text.text);
							answer_input_len += len;
							// printf("[�����] �Էµ� ����: %s, ���� ����: %s\n", sdl_event_handler.text.text, answer_input_buffer);
						}
					}
					else if (sdl_event_handler.type == SDL_TEXTEDITING && text_input_focused) {
						strncpy(ime_composition, sdl_event_handler.edit.text, sizeof(ime_composition) - 1);
						ime_composition[sizeof(ime_composition) - 1] = '\0';
					}
					else if (sdl_event_handler.type == SDL_KEYDOWN) {
						if (sdl_event_handler.key.keysym.sym == SDLK_RETURN && text_input_focused) {
							if (answer_input_len > 0) {
								if (!is_current_user_drawer) {
									send_formatted_message_to_server("MSG", answer_input_buffer); // ������ ���� �޽��� ����
									//printf("[�����] ���� �޽��� ���۵�: %s\n", answer_input_buffer);
								}
								else {
									printf("[������ ����] �����ڴ� ������ �Է��� �� �����ϴ�: %s\n", answer_input_buffer);
								}
								answer_input_buffer[0] = '\0';
								answer_input_len = 0;
							}
						}
						else if (sdl_event_handler.key.keysym.sym == SDLK_BACKSPACE && text_input_focused) {
							if (answer_input_len > 0) {
								answer_input_buffer[--answer_input_len] = '\0';
							}
						}
					}
					if (sdl_event_handler.type == SDL_MOUSEBUTTONDOWN) {
						int x = sdl_event_handler.button.x;
						int y = sdl_event_handler.button.y;

						if (!is_current_user_drawer) {
							// �����ڴ� �Է�â Ŭ���� ����
							if (x >= 400 && x <= 700 && y >= 10 && y <= 40) {
								text_input_focused = 1;
								SDL_StartTextInput();
							}
							else {
								text_input_focused = 0;
								SDL_StopTextInput();
							}

							// �����ڴ� �׸� �׸��� �Ұ� �� return ����, �׳� �ƹ� ó���� ����
						}
						else {
							// �����ڴ� �Է�â ��Ȱ��ȭ
							text_input_focused = 0;
							SDL_StopTextInput();
						}
					}
					// ���콺 ��� �̺�Ʈ (�巡�� ��)
					else if (sdl_event_handler.type == SDL_MOUSEMOTION && local_mouse_is_drawing) {
						int m_x = sdl_event_handler.motion.x;
						int m_y = sdl_event_handler.motion.y;
						if (m_y >= UI_AREA_HEIGHT) { // �׸��� ���� �������� �׸���
							// �� �׸��� ��� ������ ����
							char line_payload[BUFFER_SIZE];
							snprintf(line_payload, BUFFER_SIZE, "%d,%d,%d,%d,%d,%d", local_mouse_last_x, local_mouse_last_y, m_x, m_y, current_sdl_color_index, current_pen_size);
							send_formatted_message_to_server("DRAW_LINE", line_payload);

							// ���� ����� ��� ���ۿ��� �߰��Ͽ� ��� �ݿ�
							ReceivedDrawData data = { 0 }; data.type = 1; data.x1 = local_mouse_last_x; data.y1 = local_mouse_last_y;
							data.x2 = m_x; data.y2 = m_y; data.color_index = current_sdl_color_index; data.size = current_pen_size;
							pthread_mutex_lock(&received_draw_commands_lock);
							if (received_draw_commands_count >= received_draw_commands_capacity) {
								received_draw_commands_capacity *= 2;
								received_draw_commands = realloc(received_draw_commands, received_draw_commands_capacity * sizeof(ReceivedDrawData));
								if (!received_draw_commands) { perror("realloc draw line cmd failed"); pthread_mutex_unlock(&received_draw_commands_lock); main_program_should_run = 0; break; }
							}
							if (received_draw_commands) received_draw_commands[received_draw_commands_count++] = data;
							pthread_mutex_unlock(&received_draw_commands_lock);

							local_mouse_last_x = m_x; local_mouse_last_y = m_y; // ������ ��ġ ������Ʈ
						}
					}
					// ���콺 ��ư �� �̺�Ʈ (�׸��� ����)
					else if (sdl_event_handler.type == SDL_MOUSEBUTTONUP && sdl_event_handler.button.button == SDL_BUTTON_LEFT) {
						local_mouse_is_drawing = 0;
						local_mouse_last_x = -1;
						local_mouse_last_y = -1;
					}
					// ���콺 ��ư �ٿ� �̺�Ʈ (Ŭ��)
					if (sdl_event_handler.type == SDL_MOUSEBUTTONDOWN && sdl_event_handler.button.button == SDL_BUTTON_LEFT) {
						int m_x = sdl_event_handler.button.x;
						int m_y = sdl_event_handler.button.y;
						if (m_y < UI_AREA_HEIGHT) { // UI ���� Ŭ��
							handle_sdl_mouse_click_on_ui(m_x, m_y);
						}
						else {
							if (is_current_user_drawer) {
								// �׸��� Ŭ�� (�����ڸ� ����)
								local_mouse_is_drawing = 1;
								local_mouse_last_x = m_x;
								local_mouse_last_y = m_y;

								// �� �׸��� ��� ������ ����
								char point_payload[BUFFER_SIZE];
								snprintf(point_payload, BUFFER_SIZE, "%d,%d,%d,%d", m_x, m_y, current_sdl_color_index, current_pen_size);
								send_formatted_message_to_server("DRAW_POINT", point_payload);

								// ���� ����� ��� ���ۿ��� �߰��Ͽ� ��� �ݿ�
								ReceivedDrawData data = { 0 }; data.type = 0; data.x1 = m_x; data.y1 = m_y;
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
							}
							else {
								printf("[����] �����ڰ� �ƴϹǷ� �׸��� �׸� �� �����ϴ�.\n");
							}
						}

					}
				} // end of SDL_PollEvent loop

				// --- ����� ��� ���� (received_draw_commands ť ó��) ---
				pthread_mutex_lock(&received_draw_commands_lock);
				if (received_draw_commands_count > 0) {
					SDL_SetRenderTarget(app_sdl_renderer, app_drawing_canvas_texture); // ĵ���� �ؽ�ó�� �׸�
					for (int i = 0; i < received_draw_commands_count; i++) {
						ReceivedDrawData cmd = received_draw_commands[i];
						if (cmd.type == 0) { // POINT
							SDL_SetRenderDrawColor(app_sdl_renderer, sdl_colors[cmd.color_index].r, sdl_colors[cmd.color_index].g, sdl_colors[cmd.color_index].b, 255);
							// UI_AREA_HEIGHT ��ŭ ���� �����Ͽ� �׸��� ������ �°� �׸���
							SDL_Rect pr = { cmd.x1 - cmd.size / 2, (cmd.y1 - UI_AREA_HEIGHT) - cmd.size / 2, cmd.size, cmd.size };
							SDL_RenderFillRect(app_sdl_renderer, &pr);
						}
						else if (cmd.type == 1) { // LINE
							SDL_SetRenderDrawColor(app_sdl_renderer, sdl_colors[cmd.color_index].r, sdl_colors[cmd.color_index].g, sdl_colors[cmd.color_index].b, 255);
							// �� �β��� ���� ���� ���� ���� �׸� (���� ȿ��)
							for (int dx = -cmd.size / 2; dx <= cmd.size / 2; ++dx) {
								for (int dy = -cmd.size / 2; dy <= cmd.size / 2; ++dy) {
									// ���� ����� ���� ���� �ٻ�ġ ���
									if (dx * dx + dy * dy <= (cmd.size / 2) * (cmd.size / 2) + 1) {
										// UI_AREA_HEIGHT ��ŭ ���� ����
										SDL_RenderDrawLine(app_sdl_renderer, cmd.x1 + dx, (cmd.y1 - UI_AREA_HEIGHT) + dy, cmd.x2 + dx, (cmd.y2 - UI_AREA_HEIGHT) + dy);
									}
								}
							}
						}
						else if (cmd.type == 2) { // CLEAR
							SDL_SetRenderDrawColor(app_sdl_renderer, 255, 255, 255, 255); // ������� ����
							SDL_RenderClear(app_sdl_renderer);
						}
					}
					received_draw_commands_count = 0; // ó�� �Ϸ�� ��� �ʱ�ȭ
					SDL_SetRenderTarget(app_sdl_renderer, NULL); // �ٽ� �⺻ ���� Ÿ������ ����
				}
				pthread_mutex_unlock(&received_draw_commands_lock);

				// --- ȭ�� ������ ---
				SDL_SetRenderDrawColor(app_sdl_renderer, 0, 0, 0, 255); // ���������� ��� �ʱ�ȭ
				SDL_RenderClear(app_sdl_renderer);

				// ĵ���� �ؽ�ó�� ȭ�鿡 �׸���
				SDL_Rect canvas_dest_rect = { 0, UI_AREA_HEIGHT, WINDOW_WIDTH, WINDOW_HEIGHT - UI_AREA_HEIGHT };
				SDL_RenderCopy(app_sdl_renderer, app_drawing_canvas_texture, NULL, &canvas_dest_rect);

				render_sdl_ui_elements(); // UI ��� �׸���

				SDL_RenderPresent(app_sdl_renderer); // ȭ�� ������Ʈ

				// ������ ����Ʈ ���� (60 FPS)
				Uint32 frame_time = SDL_GetTicks() - sdl_frame_start_tick;
				if (frame_time < 1000 / 60) {
					SDL_Delay((1000 / 60) - frame_time);
				}
			} // end of if (sdl_render_loop_running)
		}
		else {
			// SDL UI�� ��Ȱ��ȭ�� ��� (�κ� ���� ��) �͹̳� �Է� ó��
			// non-blocking�ϰ� �͹̳� �Է� Ȯ��
			fd_set read_fds;
			struct timeval tv;

			FD_ZERO(&read_fds);
			FD_SET(STDIN_FILENO, &read_fds); // ǥ�� �Է� ����

			// ª�� �ð��� ����Ͽ� CPU �������� �ʹ� ������ ����
			tv.tv_sec = 0;
			tv.tv_usec = 10000; // 10ms

			int select_result = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &tv);

			if (select_result < 0) {
				if (errno == EINTR) continue; // ���ͷ�Ʈ �� ��� ��õ�
				perror("select error");
				main_program_should_run = 0;
				break;
			}
			else if (select_result > 0) {
				if (FD_ISSET(STDIN_FILENO, &read_fds)) {
					if (fgets(terminal_input_line, BUFFER_SIZE, stdin) != NULL) {
						terminal_input_line[strcspn(terminal_input_line, "\n")] = 0; // ���� ���� ����

						if (strlen(terminal_input_line) == 0) {
							printf("> ");
							fflush(stdout);
							continue;
						}

						if (strncmp(terminal_input_line, "/nick ", 6) == 0) {
							send_formatted_message_to_server("NICK", terminal_input_line + 6);
						}
						else if (strncmp(terminal_input_line, "/create ", 8) == 0) {
							send_formatted_message_to_server("CREATE_ROOM", terminal_input_line + 8);
						}
						else if (strncmp(terminal_input_line, "/join ", 6) == 0) {
							send_formatted_message_to_server("JOIN_ROOM", terminal_input_line + 6);
						}
						else if (strcmp(terminal_input_line, "/exit") == 0) {
							send_formatted_message_to_server("EXIT_ROOM", NULL);
						}
						else if (strcmp(terminal_input_line, "/list") == 0) {
							send_formatted_message_to_server("LIST_ROOMS", NULL);
						}
						else if (strcmp(terminal_input_line, "/quit") == 0) {
							main_program_should_run = 0; // ���α׷� ���� �÷���
							break;
						}
						else if (strncmp(terminal_input_line, "/msg ", 5) == 0) {
							send_formatted_message_to_server("MSG", terminal_input_line + 5);
						}
						else if (app_current_state == STATE_IN_ROOM && client_is_nick_set) {
							printf("\r[Ŭ���̾�Ʈ] ���� �׸��� UI Ȱ�� �����Դϴ�. ä���� UI���� ���� �Է����ּ���.\n> ");
						}
						else {
							printf("\r[Ŭ���̾�Ʈ] �� �� ���� ����̰ų�, ���� ���¿��� ����� �� ���� ����Դϴ�.\n> ");
						}
						printf("> "); // ���ο� ������Ʈ ���
						fflush(stdout);
					}
					else {
						// EOF (Ctrl+D) �Ǵ� �б� ���� �� ����
						main_program_should_run = 0;
					}
				}
			}
			// SDL UI�� ��Ȱ��ȭ�� ���ȿ��� CPU�� ���� �� �纸
			SDL_Delay(10);
		}
		// SDL UI Ȱ��ȭ ���°� �ٲ���µ� ���� SDL�� ���� ���̶�� ����
		if (!sdl_should_be_active && sdl_render_loop_running) {
			printf("[Ŭ���̾�Ʈ:����] SDL UI ��Ȱ��ȭ ��û ����. ���� �Լ� ȣ��.\n");
			shutdown_sdl_environment(); // SDL �ڿ� ����
		}
	} // end of while (main_program_should_run)

	// 5. ���α׷� ���� ����
	printf("[Ŭ���̾�Ʈ:����] ���α׷� ���� ��...\n");
	// ���� �����尡 ����� ������ ���
	pthread_join(server_message_receiver_tid, NULL);

	// ���� SDL �ڿ� Ȯ���� ���� (���� ȣ�� ���� ���� ����)
	shutdown_sdl_environment();

	// ����� ��� ���� �޸� ����
	if (received_draw_commands) {
		free(received_draw_commands);
		received_draw_commands = NULL;
	}
	pthread_mutex_destroy(&received_draw_commands_lock);

	// ���� �ݱ�
	close(client_socket_fd);
	printf("[Ŭ���̾�Ʈ:����] ��� �ڿ� ���� �Ϸ�. ����.\n");

	return 0;
}