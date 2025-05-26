// gcc yserver2.c -o yserver2 -pthread
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>

#define SERVER_PORT 12345
#define MAX_CLIENTS FD_SETSIZE
#define MAX_ROOMS 10
#define MAX_USERS_PER_ROOM 40
#define BUFFER_SIZE 1024
#define NICK_SIZE 32
#define ROOM_NAME_SIZE 32
#define INPUT_BUFFER_SIZE (BUFFER_SIZE * 2)

typedef struct ClientInfo ClientInfo; // Forward declaration
typedef struct Room Room;       // Forward declaration

struct ClientInfo {
    int sock;
    char nickname[NICK_SIZE];
    int roomID; // -1: 로비, 0 이상: 방 ID
    int has_set_initial_nick; // 라운지에서 초기 닉네임 설정 여부
    char input_buffer[INPUT_BUFFER_SIZE];
    int input_len;
};

struct Room {
    int id;
    char name[ROOM_NAME_SIZE];
    ClientInfo* users[MAX_USERS_PER_ROOM];
    int user_count;
    // int current_drawer_sock; // 캐치마인드용 추후 추가
};

ClientInfo* clients[MAX_CLIENTS]; // 인덱스 = 소켓 fd
Room rooms[MAX_ROOMS];
int room_count = 0;
int next_room_id = 1; // 1부터 시작

// --- 함수 프로토타입 ---
void send_to_client(int sock, const char* msg);
ClientInfo* get_client_by_sock(int sock);
Room* get_room_by_id(int id);
Room* get_room_by_name(const char* name);
int is_nickname_taken(const char* nick);
void add_client_to_room(Room* room, ClientInfo* client);
void remove_client_from_room(ClientInfo* client, Room* room);
void handle_client_disconnection(int client_sock);
void process_client_message(ClientInfo* client, const char* message_line);
// 방 관련 명령어 처리 함수
void handle_nick_command(ClientInfo* client, const char* nick_arg);
void handle_create_room_command(ClientInfo* client, const char* room_name_arg);
void handle_join_room_command(ClientInfo* client, const char* room_identifier_arg);
void handle_list_rooms_command(ClientInfo* client);
void handle_exit_room_command(ClientInfo* client);
void broadcast_message_to_room(ClientInfo* sender, const char* original_message_payload);
void broadcast_draw_data_to_room(ClientInfo* sender, const char* draw_message_line_without_newline);


void send_to_client(int sock, const char* msg) {
    if (send(sock, msg, strlen(msg), 0) < 0) {
        perror("send to client failed");
    }
}

ClientInfo* get_client_by_sock(int sock) {
    if (sock < 0 || sock >= MAX_CLIENTS) return NULL;
    return clients[sock];
}

Room* get_room_by_id(int id) {
    for (int i = 0; i < room_count; i++) {
        if (rooms[i].id == id) return &rooms[i];
    }
    return NULL;
}

Room* get_room_by_name(const char* name) {
    for (int i = 0; i < room_count; i++) {
        if (strcmp(rooms[i].name, name) == 0) return &rooms[i];
    }
    return NULL;
}

int is_nickname_taken(const char* nick) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] != NULL && clients[i]->has_set_initial_nick && strcmp(clients[i]->nickname, nick) == 0) {
            return 1; // 중복됨
        }
    }
    return 0; // 중복 아님
}

void add_client_to_room(Room* room, ClientInfo* client) {
    if (room->user_count < MAX_USERS_PER_ROOM) {
        room->users[room->user_count++] = client;
        client->roomID = room->id;
        char smsg[BUFFER_SIZE];
        snprintf(smsg, BUFFER_SIZE, "SMSG:방 '%s'(ID:%d)에 입장했습니다. 그림판 UI가 활성화됩니다.\n", room->name, room->id);
        send_to_client(client->sock, smsg);

        char enter_msg[BUFFER_SIZE];
        snprintf(enter_msg, BUFFER_SIZE, "ROOM_EVENT:[%s]님이 입장했습니다.\n", client->nickname);
        for (int i = 0; i < room->user_count; i++) {
            if (room->users[i]->sock != client->sock) {
                send_to_client(room->users[i]->sock, enter_msg);
            }
        }
    } else {
        send_to_client(client->sock, "SMSG:방이 가득 찼습니다.\n");
    }
}

void remove_client_from_room(ClientInfo* client, Room* room) {
    if (!room || client->roomID != room->id) return;

    int found = 0;
    for (int i = 0; i < room->user_count; i++) {
        if (room->users[i]->sock == client->sock) {
            found = 1;
        }
        if (found && i < room->user_count - 1) {
            room->users[i] = room->users[i + 1];
        }
    }
    if (found) {
        room->user_count--;
        char leave_msg[BUFFER_SIZE];
        snprintf(leave_msg, BUFFER_SIZE, "ROOM_EVENT:[%s]님이 퇴장했습니다.\n", client->nickname);
        for (int i = 0; i < room->user_count; i++) { // 남은 사람들에게 알림
            send_to_client(room->users[i]->sock, leave_msg);
        }
        printf("[서버] %s님이 방 '%s'(ID:%d)에서 퇴장.\n", client->nickname, room->name, room->id);
    }
    client->roomID = -1; // 로비로 상태 변경
    // 방이 비었을 때 자동 삭제 로직 (선택 사항)
    if (room->user_count == 0 && room_count > 0) {
        printf("[서버] 방 '%s'(ID:%d)이(가) 비어서 삭제됩니다.\n", room->name, room->id);
        int room_idx = -1;
        for(int i=0; i<room_count; ++i) if(rooms[i].id == room->id) room_idx = i;

        if(room_idx != -1){
            for(int i = room_idx; i < room_count - 1; ++i) {
                rooms[i] = rooms[i+1];
            }
            room_count--;
        }
    }
}

void handle_client_disconnection(int client_sock) {
    ClientInfo* client = get_client_by_sock(client_sock);
    if (!client) return;

    printf("[서버] 클라이언트 %s (sock %d) 연결 종료 처리 시작.\n", client->has_set_initial_nick ? client->nickname : "익명", client_sock);
    if (client->roomID != -1) {
        Room* room = get_room_by_id(client->roomID);
        if (room) {
            remove_client_from_room(client, room);
        }
    }
    close(client_sock);
    free(clients[client_sock]);
    clients[client_sock] = NULL;
    printf("[서버] 클라이언트 (sock %d) 리소스 해제 완료.\n", client_sock);
}

void handle_nick_command(ClientInfo* client, const char* nick_arg) {
    if (strlen(nick_arg) == 0 || strlen(nick_arg) >= NICK_SIZE) {
        send_to_client(client->sock, "SMSG:닉네임 형식이 올바르지 않습니다. (1~31자)\n");
        return;
    }
    if (is_nickname_taken(nick_arg)) {
        char err_msg[BUFFER_SIZE];
        snprintf(err_msg, BUFFER_SIZE, "ERR_NICK_TAKEN:%s\n", nick_arg);
        send_to_client(client->sock, err_msg);
        return;
    }
    char old_nick[NICK_SIZE];
    strcpy(old_nick, client->nickname);
    int was_initial_nick_set = client->has_set_initial_nick;

    strcpy(client->nickname, nick_arg);
    client->has_set_initial_nick = 1;

    char smsg[BUFFER_SIZE];
    snprintf(smsg, BUFFER_SIZE, "SMSG:닉네임이 '%s'(으)로 설정되었습니다.\n", client->nickname);
    send_to_client(client->sock, smsg);

    if (client->roomID != -1 && was_initial_nick_set) { // 방 안에서 닉변
        char nick_change_broadcast[BUFFER_SIZE];
        snprintf(nick_change_broadcast, BUFFER_SIZE, "ROOM_EVENT:['%s'님이 '%s'(으)로 닉네임을 변경했습니다.]\n", old_nick, client->nickname);
        Room* room = get_room_by_id(client->roomID);
        if(room){
            for(int i=0; i < room->user_count; ++i){
                if(room->users[i]->sock != client->sock) send_to_client(room->users[i]->sock, nick_change_broadcast);
            }
        }
    }
}

void handle_create_room_command(ClientInfo* client, const char* room_name_arg) {
    if (!client->has_set_initial_nick) {
        send_to_client(client->sock, "ERR_NICK_REQUIRED:방 생성\n");
        return;
    }
    if (client->roomID != -1) {
        send_to_client(client->sock, "SMSG:이미 다른 방에 참여중입니다. 먼저 퇴장해주세요. (/exit_room)\n");
        return;
    }
    if (room_count >= MAX_ROOMS) {
        send_to_client(client->sock, "SMSG:더 이상 방을 만들 수 없습니다.\n");
        return;
    }
    if (strlen(room_name_arg) == 0 || strlen(room_name_arg) >= ROOM_NAME_SIZE) {
        send_to_client(client->sock, "SMSG:방 이름이 유효하지 않습니다.\n");
        return;
    }
    // 방 이름 중복 검사 (선택 사항)
    if (get_room_by_name(room_name_arg) != NULL){
        send_to_client(client->sock, "SMSG:이미 존재하는 방 이름입니다.\n");
        return;
    }

    Room* new_room = &rooms[room_count]; // 새 방 슬롯
    new_room->id = next_room_id++;
    strncpy(new_room->name, room_name_arg, ROOM_NAME_SIZE - 1);
    new_room->name[ROOM_NAME_SIZE - 1] = '\0';
    new_room->user_count = 0;
    // new_room->current_drawer_sock = -1; // 게임 관련 초기화
    // new_room->game_state = WAITING;
    room_count++; // 실제 방 개수 증가

    add_client_to_room(new_room, client);
}

void handle_join_room_command(ClientInfo* client, const char* room_identifier_arg) {
    if (!client->has_set_initial_nick) {
        send_to_client(client->sock, "ERR_NICK_REQUIRED:방 입장\n");
        return;
    }
    if (client->roomID != -1) {
        send_to_client(client->sock, "SMSG:이미 다른 방에 참여중입니다. 먼저 퇴장해주세요. (/exit_room)\n");
        return;
    }
    if (strlen(room_identifier_arg) == 0) {
        send_to_client(client->sock, "SMSG:입장할 방 번호 또는 방 이름을 입력하세요.\n");
        return;
    }

    Room* room_to_join = NULL;
    int room_id_attempt = atoi(room_identifier_arg);
    if (room_id_attempt > 0) { // 숫자로 변환 시도 (ID로 간주)
        room_to_join = get_room_by_id(room_id_attempt);
    }
    if (!room_to_join) { // ID로 못 찾았거나, 원래 문자열이었으면 이름으로 검색
        room_to_join = get_room_by_name(room_identifier_arg);
    }

    if (!room_to_join) {
        send_to_client(client->sock, "SMSG:존재하지 않는 방이거나 잘못된 입력입니다.\n");
        return;
    }
    add_client_to_room(room_to_join, client);
}

void handle_list_rooms_command(ClientInfo* client) {
    char list_buffer[BUFFER_SIZE] = "ROOMLIST:";
    int current_len = strlen(list_buffer);
    int first_room = 1;

    if (room_count == 0) {
        strcat(list_buffer, "현재 생성된 방이 없습니다.\n");
    } else {
        for (int i = 0; i < room_count; i++) {
            if (rooms[i].id > 0 ) { // 유효한 방만 (id가 0이하가 아닌)
                 char room_detail[128];
                if (!first_room) {
                    if (current_len + 1 < BUFFER_SIZE -1) {
                        strcat(list_buffer, ";"); current_len +=1;
                    } else break;
                }
                snprintf(room_detail, sizeof(room_detail), "%d-%s(%d)", rooms[i].id, rooms[i].name, rooms[i].user_count);
                if (current_len + strlen(room_detail) < BUFFER_SIZE -1) {
                    strcat(list_buffer, room_detail);
                    current_len += strlen(room_detail);
                    first_room = 0;
                } else {
                     if (current_len + 4 < BUFFER_SIZE -1) strcat(list_buffer, "..."); current_len +=3;
                    break;
                }
            }
        }
        if (current_len < BUFFER_SIZE -1) strcat(list_buffer, "\n");
        else list_buffer[BUFFER_SIZE-1] = '\0';
    }
    send_to_client(client->sock, list_buffer);
}

void handle_exit_room_command(ClientInfo* client) {
    if (client->roomID == -1) {
        send_to_client(client->sock, "SMSG:현재 어떤 방에도 참여하고 있지 않습니다.\n");
        return;
    }
    Room* room = get_room_by_id(client->roomID);
    if (room) {
        remove_client_from_room(client, room); // 내부에서 roomID = -1로 변경
    }
    send_to_client(client->sock, "SMSG:방에서 퇴장했습니다. 로비로 돌아갑니다.\n");
}

void broadcast_message_to_room(ClientInfo* sender, const char* original_message_payload) {
    if (sender->roomID == -1 || !sender->has_set_initial_nick) {
        send_to_client(sender->sock, "SMSG:메시지를 보내려면 방에 입장하고 닉네임을 설정해야 합니다.\n");
        return;
    }
    Room* room = get_room_by_id(sender->roomID);
    if (!room) return;

    char msg_to_broadcast[BUFFER_SIZE];
    snprintf(msg_to_broadcast, BUFFER_SIZE, "MSG:[%s] %s\n", sender->nickname, original_message_payload);

    for (int i = 0; i < room->user_count; i++) {
        if (room->users[i]->sock != sender->sock) { // 자신 제외
            send_to_client(room->users[i]->sock, msg_to_broadcast);
        }
    }
}

void broadcast_draw_data_to_room(ClientInfo* sender, const char* draw_message_line_without_newline) {
    if (sender->roomID == -1 || !sender->has_set_initial_nick) {
        return;
    }
    Room* room = get_room_by_id(sender->roomID);
    if (!room) return;

    char message_with_newline[BUFFER_SIZE];
    snprintf(message_with_newline, BUFFER_SIZE, "%s\n", draw_message_line_without_newline); // 개행 문자 추가

    for (int i = 0; i < room->user_count; i++) {
        if (room->users[i]->sock != sender->sock) {
            send_to_client(room->users[i]->sock, message_with_newline);
        }
    }
}


void process_client_message(ClientInfo* client, const char* message_line) {
    printf("[서버 수신] (sock %d, nick %s, room %d): %s\n", client->sock, client->nickname, client->roomID, message_line);

    if (strncmp(message_line, "NICK:", 5) == 0) {
        handle_nick_command(client, message_line + 5);
    } else if (strncmp(message_line, "CREATE_ROOM:", 12) == 0) {
        handle_create_room_command(client, message_line + 12);
    } else if (strncmp(message_line, "JOIN_ROOM:", 10) == 0) {
        handle_join_room_command(client, message_line + 10);
    } else if (strcmp(message_line, "LIST_ROOMS") == 0) {
        handle_list_rooms_command(client);
    } else if (strcmp(message_line, "EXIT_ROOM") == 0) {
        handle_exit_room_command(client);
    } else if (strncmp(message_line, "MSG:", 4) == 0) {
        broadcast_message_to_room(client, message_line + 4);
    } else if (strncmp(message_line, "DRAW_POINT:", 11) == 0 ||
               strncmp(message_line, "DRAW_LINE:", 10) == 0 ||
               strcmp(message_line, "DRAW_CLEAR") == 0) {
        broadcast_draw_data_to_room(client, message_line); // message_line은 이미 \n 포함
    } else if (strcmp(message_line, "QUIT") == 0) {
        // 클라이언트가 /quit 입력. 연결 종료 처리.
        // handle_client_disconnection에서 FD_CLR은 해주므로 여기서는 호출만.
        // select 루프에서 recv <= 0 일 때와 동일하게 처리되도록 유도.
        // send_to_client(client->sock, "SMSG:연결을 종료합니다.\n"); // 선택적
        // 이 메시지 후 클라이언트는 아마 close 할 것임.
        // 서버에서는 다음 recv에서 0을 받아 연결 종료 처리.
        printf("[서버] 클라이언트 %s (sock %d)가 QUIT 요청.\n", client->nickname, client->sock);
    }
    else {
        send_to_client(client->sock, "SMSG:알 수 없는 형식의 메시지 또는 명령어입니다.\n");
    }
}


int main() {
    int listen_sock, new_sock_fd;
    struct sockaddr_in server_address, client_address;
    socklen_t client_address_len;
    fd_set master_fd_set, read_fd_set;
    int max_fd;

    for (int i = 0; i < MAX_CLIENTS; i++) clients[i] = NULL;
    for (int i = 0; i < MAX_ROOMS; i++) rooms[i].id = 0; // id 0은 사용 안 함

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int optval = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(SERVER_PORT);

    if (bind(listen_sock, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        perror("bind"); close(listen_sock); exit(EXIT_FAILURE);
    }
    if (listen(listen_sock, MAX_CLIENTS) < 0) {
        perror("listen"); close(listen_sock); exit(EXIT_FAILURE);
    }

    FD_ZERO(&master_fd_set);
    FD_SET(listen_sock, &master_fd_set);
    max_fd = listen_sock;

    printf("[서버] 캐치마인드 기능 통합 서버 실행 중 (포트: %d)...\n", SERVER_PORT);

    while (1) {
        read_fd_set = master_fd_set;
        if (select(max_fd + 1, &read_fd_set, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select error"); break;
        }

        for (int sock_fd = 0; sock_fd <= max_fd; sock_fd++) {
            if (FD_ISSET(sock_fd, &read_fd_set)) {
                if (sock_fd == listen_sock) { // New connection
                    client_address_len = sizeof(client_address);
                    new_sock_fd = accept(listen_sock, (struct sockaddr*)&client_address, &client_address_len);
                    if (new_sock_fd < 0) { perror("accept"); continue; }

                    if (new_sock_fd >= MAX_CLIENTS) {
                        fprintf(stderr, "[서버] 최대 클라이언트 수(%d) 도달. 연결 거부 (fd %d).\n", MAX_CLIENTS, new_sock_fd);
                        send_to_client(new_sock_fd, "SMSG:서버가 가득 찼습니다.\n");
                        close(new_sock_fd);
                        continue;
                    }
                    
                    clients[new_sock_fd] = (ClientInfo*)malloc(sizeof(ClientInfo));
                    if (!clients[new_sock_fd]) { perror("malloc for client failed"); close(new_sock_fd); continue;}

                    clients[new_sock_fd]->sock = new_sock_fd;
                    strcpy(clients[new_sock_fd]->nickname, "익명");
                    clients[new_sock_fd]->roomID = -1;
                    clients[new_sock_fd]->has_set_initial_nick = 0;
                    clients[new_sock_fd]->input_len = 0;
                    memset(clients[new_sock_fd]->input_buffer, 0, INPUT_BUFFER_SIZE);

                    FD_SET(new_sock_fd, &master_fd_set);
                    if (new_sock_fd > max_fd) max_fd = new_sock_fd;
                    printf("[서버] 새 클라이언트 연결: %s (sock %d)\n", inet_ntoa(client_address.sin_addr), new_sock_fd);
                    send_to_client(new_sock_fd, "SMSG:서버에 연결되었습니다. 라운지입니다. 먼저 /nick <원하는닉네임> 으로 닉네임을 설정해주세요.\nSMSG:닉네임 설정 후 /roomlist, /create <방이름>, /join <방ID/이름> 명령어를 사용할 수 있습니다.\n");

                } else { // Data from existing client
                    ClientInfo* client = get_client_by_sock(sock_fd);
                    if (!client) continue;

                    char recv_temp_buffer[BUFFER_SIZE];
                    int nbytes = recv(sock_fd, recv_temp_buffer, BUFFER_SIZE - 1, 0);

                    if (nbytes <= 0) {
                        if (nbytes == 0) printf("[서버] 클라이언트 %s (sock %d) 연결 정상 종료.\n", client->has_set_initial_nick ? client->nickname : "익명", sock_fd);
                        else perror("recv from client failed");
                        FD_CLR(sock_fd, &master_fd_set);
                        handle_client_disconnection(sock_fd);
                    } else {
                        recv_temp_buffer[nbytes] = '\0';
                        if (client->input_len + nbytes < INPUT_BUFFER_SIZE) {
                            strncat(client->input_buffer, recv_temp_buffer, nbytes);
                            client->input_len += nbytes;
                        } else {
                            fprintf(stderr, "[서버] 클라이언트 (sock %d) 입력 버퍼 오버플로우.\n", sock_fd);
                            client->input_len = 0; memset(client->input_buffer, 0, INPUT_BUFFER_SIZE);
                        }

                        char *line_start = client->input_buffer;
                        char *newline_ptr;
                        while ((newline_ptr = strchr(line_start, '\n')) != NULL) {
                            *newline_ptr = '\0';
                            char single_line_msg[INPUT_BUFFER_SIZE];
                            strcpy(single_line_msg, line_start);
                            
                            process_client_message(client, single_line_msg);
                            
                            line_start = newline_ptr + 1;
                        }
                        if (line_start > client->input_buffer && strlen(line_start) > 0) {
                            char temp_remaining_data[INPUT_BUFFER_SIZE];
                            strcpy(temp_remaining_data, line_start);
                            memset(client->input_buffer, 0, INPUT_BUFFER_SIZE);
                            strcpy(client->input_buffer, temp_remaining_data);
                            client->input_len = strlen(client->input_buffer);
                        } else {
                             client->input_len = 0;
                             memset(client->input_buffer, 0, INPUT_BUFFER_SIZE);
                        }
                    }
                }
            }
        }
    }
    // 실제로는 이 루프를 빠져나오지 않음. 시그널 처리 등으로 종료 시 자원 해제 필요.
    for(int i=0; i<=max_fd; ++i) if(FD_ISSET(i, &master_fd_set) && clients[i]) handle_client_disconnection(i);
    close(listen_sock);
    printf("[서버] 서버 종료.\n");
    return 0;
}
