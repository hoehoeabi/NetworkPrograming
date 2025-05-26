// integrated_server.c
//gcc yserver.c -o yserver -pthread
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h> // errno, EWOULDBLOCK, EAGAIN

#define SERVER_PORT 12345
#define MAX_CLIENTS FD_SETSIZE // select가 다룰 수 있는 최대 클라이언트 수
#define MAX_ROOMS 10
#define MAX_USERS_PER_ROOM 40
#define BUFFER_SIZE 1024
#define NICK_SIZE 32
#define ROOM_NAME_SIZE 32
#define INPUT_BUFFER_SIZE (BUFFER_SIZE * 2) // 메시지 프레이밍을 위한 충분한 버퍼


// 클라이언트 정보 구조체
typedef struct {
    int sock;
    char nickname[NICK_SIZE];
    int roomID; // -1: 로비, 0 이상: 방 ID
    int has_nickname;
    char input_buffer[INPUT_BUFFER_SIZE]; // 메시지 프레이밍을 위한 개별 입력 버퍼
    int input_len;                        // 현재 input_buffer에 저장된 데이터 길이
    // 캐치마인드 게임 관련 정보 (추후 추가)
    // int score;
    // int is_drawer; // 현재 출제자인지 여부
} ClientInfo;

// 채팅방 정보 구조체
typedef struct {
    int id;
    char name[ROOM_NAME_SIZE];
    ClientInfo* users[MAX_USERS_PER_ROOM]; // ClientInfo 포인터 배열
    int user_count;
    // 캐치마인드 게임 관련 정보 (추후 추가)
    // char current_answer[NICK_SIZE];
    // int current_drawer_sock; // 현재 출제자의 소켓
    // enum { WAITING, PLAYING, GUESSING } game_state;
} Room;

// --- 함수 프로토타입 선언 ---
void send_to_client(int sock, const char* msg);
ClientInfo* get_client_by_sock(int sock); // 이미 유사한 것이 있을 수 있음
Room* get_room_by_id(int id);
void add_client_to_room(Room* room, ClientInfo* client);
void remove_client_from_room_struct(Room* room, int client_sock);
void notify_room_join(Room* room, ClientInfo* joining_client);
void notify_room_leave(Room* room, ClientInfo* leaving_client);
void handle_client_disconnection(int client_sock);
void create_room(ClientInfo* client, const char* room_name_arg);
void join_room(ClientInfo* client, int room_id_to_join);
void leave_room(ClientInfo* client); // <--- 이 프로토타입이 누락되었을 가능성
void list_rooms_to_client(ClientInfo* client);
void list_users_in_room_to_client(ClientInfo* client);
void process_client_command(ClientInfo* client, const char* cmd_line_full);
void broadcast_to_room(int roomID, const char* msg, int sender_sock);
// ---------------------------



ClientInfo* clients[MAX_CLIENTS]; // client_info 포인터 배열, 인덱스가 소켓 fd와 같도록 관리하면 편리
Room rooms[MAX_ROOMS];
int room_count = 0;
int next_room_id = 1;

// --- 유틸리티 함수 ---

Room* get_room_by_name(const char* name) {
    for (int i = 0; i < room_count; i++) {
        if (strcmp(rooms[i].name, name) == 0) {
            return &rooms[i];
        }
    }
    return NULL;
}

void broadcast_to_room(int roomID, const char* msg, int sender_sock) {
    // pthread_mutex_lock(&clients_mutex); // select 기반에서는 뮤텍스가 꼭 필요하지 않을 수 있음. clients 배열 접근 시 주의
    for (int i = 0; i < MAX_CLIENTS; i++) { // fd_max까지 또는 실제 연결된 클라이언트만 순회하도록 최적화 가능
        if (clients[i] != NULL &&             // 클라이언트가 존재하고
            clients[i]->sock != sender_sock && // 메시지를 보낸 자신을 제외하고
            clients[i]->roomID == roomID) {    // 같은 방에 있다면
            send_to_client(clients[i]->sock, msg);
        }
    }
    // pthread_mutex_unlock(&clients_mutex);
}

void send_to_client(int sock, const char* msg) {
    if (send(sock, msg, strlen(msg), 0) < 0) {
        // EWOULDBLOCK, EAGAIN 에러는 non-blocking 소켓에서 발생 가능, 여기서는 blocking 소켓 사용 가정
        perror("send to client failed");
    }
}

ClientInfo* get_client_by_sock(int sock) {
    if (sock < 0 || sock >= MAX_CLIENTS) return NULL;
    return clients[sock]; // 클라이언트 배열의 인덱스를 소켓 디스크립터로 사용
}

Room* get_room_by_id(int id) {
    for (int i = 0; i < room_count; i++) {
        if (rooms[i].id == id) {
            return &rooms[i];
        }
    }
    return NULL;
}

void add_client_to_room(Room* room, ClientInfo* client) {
    if (room->user_count < MAX_USERS_PER_ROOM) {
        room->users[room->user_count++] = client;
        client->roomID = room->id;
    }
}

void remove_client_from_room_struct(Room* room, int client_sock) {
    int found = 0;
    for (int i = 0; i < room->user_count; i++) {
        if (room->users[i]->sock == client_sock) {
            found = 1;
        }
        if (found && i < room->user_count - 1) {
            room->users[i] = room->users[i + 1];
        }
    }
    if (found) {
        room->user_count--;
    }
}

void notify_room_join(Room* room, ClientInfo* joining_client) {
    char msg[BUFFER_SIZE];
    snprintf(msg, BUFFER_SIZE, "CHAT:[알림] %s님이 입장했습니다.\n", joining_client->nickname);
    for (int i = 0; i < room->user_count; i++) {
        if (room->users[i]->sock != joining_client->sock) { // 자신 제외
            send_to_client(room->users[i]->sock, msg);
        }
    }
}

void notify_room_leave(Room* room, ClientInfo* leaving_client) {
    char msg[BUFFER_SIZE];
    snprintf(msg, BUFFER_SIZE, "CHAT:[알림] %s님이 퇴장했습니다.\n", leaving_client->nickname);
    for (int i = 0; i < room->user_count; i++) {
        // 떠나는 클라이언트에게는 이 메시지를 보낼 필요 없음 (이미 접속 종료되었을 수 있음)
        if (room->users[i]->sock != leaving_client->sock) {
            send_to_client(room->users[i]->sock, msg);
        }
    }
}


void handle_client_disconnection(int client_sock) {
    ClientInfo* client = get_client_by_sock(client_sock);
    if (!client) return;

    printf("[서버] 클라이언트 %s (sock %d) 연결 종료됨.\n", client->nickname, client_sock);

    if (client->roomID != -1) {
        Room* room = get_room_by_id(client->roomID);
        if (room) {
            remove_client_from_room_struct(room, client_sock);
            notify_room_leave(room, client);
            // TODO: 방이 비었으면 방 삭제 로직 (선택적)
        }
    }
    close(client_sock);
    free(clients[client_sock]); // ClientInfo 메모리 해제
    clients[client_sock] = NULL;  // 해당 슬롯 비움
}


// --- 방 관리 함수 구현 ---
void create_room(ClientInfo* client, const char* room_name_arg) {
    if (room_count >= MAX_ROOMS) {
        send_to_client(client->sock, "SMSG:더 이상 방을 만들 수 없습니다.\n");
        return;
    }
    if (client->roomID != -1) {
        send_to_client(client->sock, "SMSG:이미 다른 방에 참여중입니다. 먼저 퇴장해주세요. (/exit)\n");
        return;
    }
    if (strlen(room_name_arg) == 0 || strlen(room_name_arg) >= ROOM_NAME_SIZE) {
        send_to_client(client->sock, "SMSG:방 이름이 유효하지 않습니다.\n");
        return;
    }

    // 기존 방에서 나가기 (혹시 모를 경우 대비)
    leave_room(client);

    Room* new_room = &rooms[room_count++];
    new_room->id = next_room_id++;
    strncpy(new_room->name, room_name_arg, ROOM_NAME_SIZE -1);
    new_room->name[ROOM_NAME_SIZE -1] = '\0';
    new_room->user_count = 0;

    add_client_to_room(new_room, client);
    char smsg[BUFFER_SIZE];
    snprintf(smsg, BUFFER_SIZE, "SMSG:방 '%s'(ID:%d)이(가) 생성되었고 입장했습니다. 그림판 UI를 준비하세요.\n", new_room->name, new_room->id);
    send_to_client(client->sock, smsg);
    notify_room_join(new_room, client); // 자신에게는 보내지 않음 (위에서 이미 알림)
}

void join_room(ClientInfo* client, int room_id_to_join) {
    if (client->roomID != -1) {
        send_to_client(client->sock, "SMSG:이미 다른 방에 참여중입니다. 먼저 퇴장해주세요. (/exit)\n");
        return;
    }
    Room* room = get_room_by_id(room_id_to_join);
    if (!room) {
        send_to_client(client->sock, "SMSG:존재하지 않는 방입니다.\n");
        return;
    }
    if (room->user_count >= MAX_USERS_PER_ROOM) {
        send_to_client(client->sock, "SMSG:방이 가득 찼습니다.\n");
        return;
    }

    leave_room(client); // 혹시 로비가 아닌 다른 상태였다면 정리
    add_client_to_room(room, client);
    char smsg[BUFFER_SIZE];
    snprintf(smsg, BUFFER_SIZE, "SMSG:방 '%s'(ID:%d)에 입장했습니다. 그림판 UI를 준비하세요.\n", room->name, room->id);
    send_to_client(client->sock, smsg);
    notify_room_join(room, client);
}

void leave_room(ClientInfo* client) {
    if (client->roomID == -1) {
        send_to_client(client->sock, "SMSG:현재 어떤 방에도 참여하고 있지 않습니다.\n");
        return;
    }
    Room* room = get_room_by_id(client->roomID);
    if (room) {
        remove_client_from_room_struct(room, client->sock);
        notify_room_leave(room, client);
        // TODO: 방이 비었으면 방 삭제 로직
    }
    client->roomID = -1; // 로비 상태로 변경
    send_to_client(client->sock, "SMSG:방에서 퇴장했습니다. 로비로 돌아갑니다.\n");
}

void list_rooms_to_client(ClientInfo* client) {
    char list_msg[BUFFER_SIZE] = "ROOMLIST:";
    if (room_count == 0) {
        strcat(list_msg, "현재 생성된 방이 없습니다.\n");
    } else {
        for (int i = 0; i < room_count; i++) {
            if (rooms[i].user_count > 0) { // 유저가 있는 방만 표시 (선택적)
                char room_info[128];
                snprintf(room_info, 128, "%d-%s(%d);", rooms[i].id, rooms[i].name, rooms[i].user_count);
                if (strlen(list_msg) + strlen(room_info) < BUFFER_SIZE - 2) { // -2 for \n and \0
                    strcat(list_msg, room_info);
                } else {
                    // 버퍼 오버플로우 방지
                    strcat(list_msg, "...\n"); // 너무 많은 방 정보
                    break;
                }
            }
        }
        if (list_msg[strlen(list_msg)-1] == ';') list_msg[strlen(list_msg)-1] = '\n'; // 마지막 ;를 \n으로
        else strcat(list_msg, "\n"); // 방이 하나도 없거나, ;로 끝나지 않은 경우
    }
    send_to_client(client->sock, list_msg);
}

void list_users_in_room_to_client(ClientInfo* client) {
    if (client->roomID == -1) {
        send_to_client(client->sock, "SMSG:방에 먼저 입장해주세요.\n");
        return;
    }
    Room* room = get_room_by_id(client->roomID);
    if (!room) { // 있을 수 없는 상황이지만 방어 코드
        send_to_client(client->sock, "SMSG:오류 - 현재 방 정보를 찾을 수 없습니다.\n");
        return;
    }
    char list_msg[BUFFER_SIZE] = "USERLIST:";
    for (int i = 0; i < room->user_count; i++) {
        char user_info[NICK_SIZE + 2]; // 닉네임 + ; + \0
        snprintf(user_info, NICK_SIZE + 2, "%s;", room->users[i]->nickname);
         if (strlen(list_msg) + strlen(user_info) < BUFFER_SIZE - 2) {
            strcat(list_msg, user_info);
        } else {
            strcat(list_msg, "...\n");
            break;
        }
    }
    if (list_msg[strlen(list_msg)-1] == ';') list_msg[strlen(list_msg)-1] = '\n';
    else strcat(list_msg, "\n");
    send_to_client(client->sock, list_msg);
}


// --- 명령어 처리 함수 (이전 답변의 process_command와 유사하게 구성) ---
void process_client_command(ClientInfo* client, const char* cmd_line_full) {
    // "/nick new_nick", "/create room_name", "/join room_id", "/exit", "/roomlist", "/userlist", "/w target_nick message"
    // cmd_line_full은 "CMD:" 다음의 실제 명령어 부분
    // 파싱 로직 개선 필요
    char cmd[BUFFER_SIZE], arg1[BUFFER_SIZE], arg2[BUFFER_SIZE * 2]; // arg2는 메시지 담을 수 있도록 크게
    memset(cmd, 0, sizeof(cmd));
    memset(arg1, 0, sizeof(arg1));
    memset(arg2, 0, sizeof(arg2));

    // 간단한 파싱 (공백 기준)
    sscanf(cmd_line_full, "%s %[^\n]", cmd, arg1); // 첫번째 공백까지 cmd, 나머지는 arg1
    if (strcmp(cmd, "/w") == 0) { // 귓속말은 파싱을 다르게
        sscanf(cmd_line_full, "%s %s %[^\n]", cmd, arg1, arg2); // /w target msg
    } else { // 다른 명령어는 arg1에 모든 인자
         sscanf(cmd_line_full, "%s %[^\n]", cmd, arg1); // /cmd arguments
    }


    printf("[서버 CMD] %s (from %s): %s %s %s\n", cmd_line_full, client->nickname, cmd, arg1, arg2);


    if (strcmp(cmd, "/nick") == 0) {
        if (strlen(arg1) > 0 && strlen(arg1) < NICK_SIZE) {
            // TODO: 닉네임 중복 검사 (같은 방 또는 전체 서버)
            char old_nick[NICK_SIZE];
            strcpy(old_nick, client->nickname);
            int old_has_nick = client->has_nickname;

            strcpy(client->nickname, arg1);
            client->has_nickname = 1;
            send_to_client(client->sock, "SMSG:닉네임이 설정되었습니다.\n");

            if(client->roomID != -1){ // 방에 있다면
                if(!old_has_nick) { // 처음 닉네임 설정한 경우
                     notify_room_join(get_room_by_id(client->roomID), client);
                } else { // 닉네임 변경한 경우
                    char nick_change_msg[BUFFER_SIZE];
                    snprintf(nick_change_msg, BUFFER_SIZE, "CHAT:[알림] '%s'님이 '%s'(으)로 닉네임을 변경했습니다.\n", old_nick, client->nickname);
                    broadcast_to_room(client->roomID, nick_change_msg, -1); // 모두에게 알림
                }
            }

        } else {
            send_to_client(client->sock, "SMSG:닉네임 형식이 올바르지 않습니다. (1~31자)\n");
        }
    } else if (strcmp(cmd, "/create") == 0) {
        if (!client->has_nickname) { send_to_client(client->sock, "SMSG:먼저 /nick 으로 닉네임을 설정하세요.\n"); return; }
        create_room(client, arg1);
    }
    else if (strcmp(cmd, "/join") == 0) {
        if (!client->has_nickname) { send_to_client(client->sock, "SMSG:먼저 /nick 으로 닉네임을 설정하세요.\n"); return; }
        if (strlen(arg1) == 0) { send_to_client(client->sock, "SMSG:입장할 방 번호 또는 방 이름을 입력하세요.\n"); return; }

        int room_id_to_join = atoi(arg1); // 숫자로 변환 시도
        Room* room_to_join_ptr = NULL;

        if (room_id_to_join != 0) { // 숫자로 성공적으로 변환되었거나, "0"번 방을 허용하지 않는 경우
            room_to_join_ptr = get_room_by_id(room_id_to_join);
        } else { // 숫자로 변환 실패 (문자열일 가능성) 또는 0번 방 ID를 사용하지 않는 경우
            room_to_join_ptr = get_room_by_name(arg1); // 방 이름으로 검색 (새 함수 필요)
        }

        if (!room_to_join_ptr) {
            send_to_client(client->sock, "SMSG:존재하지 않는 방이거나 잘못된 입력입니다.\n");
            return;
        }
        join_room(client, room_to_join_ptr->id); // 실제 join_room에는 ID를 넘김
    } else if (strcmp(cmd, "/exit") == 0) {
        leave_room(client);
    } else if (strcmp(cmd, "/roomlist") == 0) {
        list_rooms_to_client(client);
    } else if (strcmp(cmd, "/userlist") == 0) {
        list_users_in_room_to_client(client);
    } else if (strcmp(cmd, "/w") == 0) { // /w <닉네임> <메시지>
        if (!client->has_nickname) { send_to_client(client->sock, "SMSG:먼저 /nick 으로 닉네임을 설정하세요.\n"); return; }
        if (client->roomID == -1) { send_to_client(client->sock, "SMSG:귓속말은 방 안에서만 가능합니다.\n"); return; }
        if (strlen(arg1) == 0 || strlen(arg2) == 0) { send_to_client(client->sock, "SMSG:사용법: /w <대상닉네임> <메시지>\n"); return; }

        ClientInfo* target_client = NULL;
        Room* current_room = get_room_by_id(client->roomID);
        if(current_room){
            for(int i=0; i < current_room->user_count; ++i){
                if(strcmp(current_room->users[i]->nickname, arg1) == 0){
                    target_client = current_room->users[i];
                    break;
                }
            }
        }
        if(target_client && target_client->sock != client->sock){
            char whisper_msg[BUFFER_SIZE];
            snprintf(whisper_msg, BUFFER_SIZE, "CHAT:[귓속말 from %s] %s\n", client->nickname, arg2);
            send_to_client(target_client->sock, whisper_msg);
            snprintf(whisper_msg, BUFFER_SIZE, "SMSG:[귓속말 to %s] %s\n", target_client->nickname, arg2); // 자신에게도 확인 메시지
            send_to_client(client->sock, whisper_msg);
        } else if (target_client && target_client->sock == client->sock) {
            send_to_client(client->sock, "SMSG:자기 자신에게 귓속말을 보낼 수 없습니다.\n");
        }
        else {
            send_to_client(client->sock, "SMSG:해당 닉네임의 사용자를 현재 방에서 찾을 수 없습니다.\n");
        }
    }
    else {
        send_to_client(client->sock, "SMSG:알 수 없는 명령어입니다. (/help 로 도움말 확인 - 추후 구현)\n");
    }
}


// --- 메인 함수 ---
int main() {
    int listen_sock, new_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len;
    fd_set master_fds, read_fds; // select를 위한 fd 집합
    int fd_max;                  // 가장 큰 파일 디스크립터

    // clients 배열 초기화
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i] = NULL;
    }

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) { perror("socket"); exit(EXIT_FAILURE); }

    // SO_REUSEADDR 옵션 설정 (서버 즉시 재시작 시 bind 에러 방지)
    int opt = 1;
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt"); close(listen_sock); exit(EXIT_FAILURE);
    }


    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); close(listen_sock); exit(EXIT_FAILURE);
    }
    if (listen(listen_sock, MAX_CLIENTS) < 0) { // SOMAXCONN 대신 MAX_CLIENTS
        perror("listen"); close(listen_sock); exit(EXIT_FAILURE);
    }

    FD_ZERO(&master_fds);
    FD_SET(listen_sock, &master_fds);
    fd_max = listen_sock;

    printf("[서버] 통합 캐치마인드 서버 실행 중 (포트: %d)...\n", SERVER_PORT);

    while (1) {
        read_fds = master_fds; // 중요: select는 read_fds를 변경하므로 매번 복사
        if (select(fd_max + 1, &read_fds, NULL, NULL, NULL) < 0) {
            // EINTR (시그널에 의한 중단)은 무시할 수 있음
            if (errno == EINTR) continue;
            perror("select");
            break; // 심각한 오류 시 종료
        }

        for (int i = 0; i <= fd_max; i++) { // 모든 가능한 fd 검사
            if (FD_ISSET(i, &read_fds)) { // 읽기 이벤트 발생
                if (i == listen_sock) { // 새로운 연결 요청
                    client_addr_len = sizeof(client_addr);
                    new_sock = accept(listen_sock, (struct sockaddr*)&client_addr, &client_addr_len);
                    if (new_sock < 0) {
                        perror("accept");
                    } else {
                        if (new_sock >= MAX_CLIENTS) { // fd가 너무 큰 경우 (드묾)
                             printf("[서버] 새 연결 fd (%d)가 MAX_CLIENTS (%d)를 초과. 연결 거부.\n", new_sock, MAX_CLIENTS);
                             send_to_client(new_sock, "SMSG:서버 리소스 부족. 연결 거부.\n");
                             close(new_sock);
                        } else if (clients[new_sock] != NULL) { // 이미 사용 중인 fd 슬롯 (이론상 발생 안 함)
                            printf("[서버] 경고: 이미 사용 중인 fd 슬롯 (%d)에 새 연결 시도.\n", new_sock);
                            close(new_sock);
                        }
                        else {
                            FD_SET(new_sock, &master_fds); // 새 소켓을 master_fds에 추가
                            if (new_sock > fd_max) fd_max = new_sock;

                            clients[new_sock] = (ClientInfo*)malloc(sizeof(ClientInfo));
                            if(clients[new_sock] == NULL) {perror("malloc for client failed"); close(new_sock); FD_CLR(new_sock, &master_fds); continue;}

                            clients[new_sock]->sock = new_sock;
                            strcpy(clients[new_sock]->nickname, "익명");
                            clients[new_sock]->roomID = -1;
                            clients[new_sock]->has_nickname = 0;
                            clients[new_sock]->input_len = 0;
                            memset(clients[new_sock]->input_buffer, 0, INPUT_BUFFER_SIZE);


                            printf("[서버] 새 클라이언트 연결: %s (sock %d)\n", inet_ntoa(client_addr.sin_addr), new_sock);
                            send_to_client(new_sock, "SMSG:서버에 연결되었습니다. 먼저 /nick <원하는닉네임> 으로 닉네임을 설정해주세요.\nSMSG:/roomlist 로 방 목록을 보거나 /create <방이름> 으로 새 방을 만들 수 있습니다.\n");
                        }
                    }
                } else { // 기존 클라이언트로부터 데이터 수신
                    ClientInfo* client = get_client_by_sock(i); // i가 클라이언트 소켓 fd
                    if (!client) continue; // 이미 연결 종료된 클라이언트일 수 있음 (방어 코드)

                    char recv_chunk[BUFFER_SIZE];
                    int bytes_received = recv(i, recv_chunk, BUFFER_SIZE -1, 0);

                    if (bytes_received <= 0) { // 연결 종료 또는 오류
                        if (bytes_received == 0) {
                            printf("[서버] 클라이언트 %s (sock %d) 정상 종료.\n", client->nickname, i);
                        } else {
                            perror("recv from client failed");
                        }
                        handle_client_disconnection(i);
                        FD_CLR(i, &master_fds); // master_fds에서 제거
                    } else { // 데이터 수신 성공
                        recv_chunk[bytes_received] = '\0';

                        // 메시지 프레이밍: 클라이언트의 개별 버퍼에 추가
                        if (client->input_len + bytes_received < INPUT_BUFFER_SIZE) {
                            strncat(client->input_buffer, recv_chunk, bytes_received);
                            client->input_len += bytes_received;
                        } else {
                            // 버퍼 오버플로우 처리 (예: 연결 종료 또는 경고 후 버퍼 비우기)
                            fprintf(stderr, "[서버 경고] 클라이언트 %s (sock %d) 입력 버퍼 오버플로우 시도.\n", client->nickname, i);
                            client->input_len = 0; // 버퍼 비우기 (간단한 처리)
                            memset(client->input_buffer, 0, INPUT_BUFFER_SIZE);
                            // 또는 연결 종료: handle_client_disconnection(i); FD_CLR(i, &master_fds); continue;
                        }

                        // 완성된 메시지(들) 처리 ('\n' 기준)
                        char *msg_start = client->input_buffer;
                        char *newline_char;
                        while ((newline_char = strchr(msg_start, '\n')) != NULL) {
                            *newline_char = '\0'; // 메시지 분리 (개행 문자를 널 문자로)
                            char single_message[INPUT_BUFFER_SIZE];
                            strcpy(single_message, msg_start); // 완성된 메시지 복사

                            printf("[서버 수신] %s (from %s, sock %d)\n", single_message, client->nickname, client->sock);

                            // 메시지 타입에 따라 처리
                            if (strncmp(single_message, "CMD:", 4) == 0) {
                                process_client_command(client, single_message + 4);
                            } else if (strncmp(single_message, "CHAT:", 5) == 0) {
                                if (client->roomID != -1 && client->has_nickname) {
                                    char chat_msg_to_broadcast[BUFFER_SIZE];
                                    snprintf(chat_msg_to_broadcast, BUFFER_SIZE, "CHAT:[%s] %s\n", client->nickname, single_message + 5);
                                    broadcast_to_room(client->roomID, chat_msg_to_broadcast, client->sock);
                                } else {
                                    send_to_client(client->sock, "SMSG:채팅을 하려면 방에 입장하고 닉네임을 설정해야 합니다.\n");
                                }
                            } else if (strncmp(single_message, "DRAW_LINE:", 10) == 0 ||
                                       strncmp(single_message, "DRAW_POINT:", 11) == 0 ||
                                       strcmp(single_message, "DRAW_CLEAR") == 0) {
                                if (client->roomID != -1 && client->has_nickname) {
                                    char draw_data_to_broadcast[BUFFER_SIZE];
                                    snprintf(draw_data_to_broadcast, BUFFER_SIZE, "%s\n", single_message);
                                    broadcast_to_room(client->roomID, draw_data_to_broadcast, client->sock);
                                } else {
                                    send_to_client(client->sock, "SMSG:그림을 그리려면 방에 입장하고 닉네임을 설정해야 합니다.\n");
                                }
                            } else if (strlen(single_message) > 0) { // 빈 줄이 아닌 알 수 없는 메시지
                                 send_to_client(client->sock, "SMSG:알 수 없는 형식의 메시지입니다.\n");
                            }
                            msg_start = newline_char + 1; // 다음 메시지 시작 위치로
                        }
                        // 처리하고 남은 데이터를 버퍼 앞으로 이동
                        if (msg_start > client->input_buffer && strlen(msg_start) > 0) {
                            char temp_remaining[INPUT_BUFFER_SIZE];
                            strcpy(temp_remaining, msg_start);
                            memset(client->input_buffer, 0, INPUT_BUFFER_SIZE);
                            strcpy(client->input_buffer, temp_remaining);
                            client->input_len = strlen(client->input_buffer);
                        } else { // 남은 데이터가 없으면 버퍼 초기화
                            memset(client->input_buffer, 0, INPUT_BUFFER_SIZE);
                            client->input_len = 0;
                        }
                    }
                }
            }
        }
    }

    // 서버 종료 시 모든 클라이언트 소켓 닫기 및 메모리 해제 (실제로는 시그널 핸들러 등에서 호출)
    for (int i = 0; i <= fd_max; i++) {
        if (FD_ISSET(i, &master_fds) && i != listen_sock) {
            handle_client_disconnection(i);
        }
    }
    close(listen_sock);
    printf("[서버] 서버 종료.\n");
    return 0;
}
