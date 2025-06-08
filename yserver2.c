// gcc yserver2.c -o yserver2 -pthread
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>
#include <time.h>

#define SERVER_PORT 12345
#define MAX_CLIENTS FD_SETSIZE
#define MAX_ROOMS 10
#define MAX_USERS_PER_ROOM 40
#define BUFFER_SIZE 1024
#define NICK_SIZE 32
#define ROOM_NAME_SIZE 32
#define INPUT_BUFFER_SIZE (BUFFER_SIZE * 2)
#define MAX_WORDS_PER_ROOM 100

typedef struct ClientInfo ClientInfo;
typedef struct Room Room;

struct ClientInfo {
	int sock;
	char nickname[NICK_SIZE];
	int roomID;
	int has_set_initial_nick;
	char input_buffer[INPUT_BUFFER_SIZE];
	int input_len;
};

struct Room {
	int id;
	char name[ROOM_NAME_SIZE];
	ClientInfo* users[MAX_USERS_PER_ROOM];
	int user_count;
	char current_answer[64];
	int drawer_sock;
	int round_active;
	time_t round_start_time;
	int round_duration_seconds;
	const char* word_list[MAX_WORDS_PER_ROOM];
	int word_count;
	ClientInfo* current_drawer;
};

ClientInfo* clients[MAX_CLIENTS];
Room rooms[MAX_ROOMS];
int room_count = 0;
int next_room_id = 1;

// --- �Լ� ������Ÿ�� ---
void start_new_round(Room* room);
void send_to_all_clients_in_room(Room* room, const char* msg);
void send_to_client(int sock, const char* msg);
ClientInfo* get_client_by_sock(int sock);
Room* get_room_by_id(int id);
Room* get_room_by_name(const char* name);
int is_nickname_taken(const char* nick);
void add_client_to_room(Room* room, ClientInfo* client);
void remove_client_from_room(ClientInfo* client, Room* room);
void handle_client_disconnection(int client_sock);
void process_client_message(ClientInfo* client, const char* message_line);
void handle_nick_command(ClientInfo* client, const char* nick_arg);
void handle_create_room_command(ClientInfo* client, const char* room_name_arg);
void handle_join_room_command(ClientInfo* client, const char* room_identifier_arg);
void handle_list_rooms_command(ClientInfo* client);
void handle_exit_room_command(ClientInfo* client);
void send_to_all_clients_in_room(Room* room, const char* msg) {
	if (!room) return;
	for (int i = 0; i < room->user_count; ++i) {
		send_to_client(room->users[i]->sock, msg);
	}
}

void broadcast_message_to_room(ClientInfo* sender, const char* original_message_payload) {
	if (sender->roomID == -1 || !sender->has_set_initial_nick) {
		send_to_client(sender->sock, "SMSG:�޽����� �������� �濡 �����ϰ� �г����� �����ؾ� �մϴ�.\n");
		return;
	}

	Room* room = get_room_by_id(sender->roomID);
	if (!room) return;

	// �����ڰ� ������ �Է��� ��� ����
	if (room->round_active &&
		sender == room->current_drawer &&
		strcasecmp(original_message_payload, room->current_answer) == 0) {
		send_to_client(sender->sock, "SMSG:�����ڴ� ������ �Է��� �� �����ϴ�.\n");
		return;
	}

	// �Ϲ� ����ڰ� ������ ���� ���
	if (room->round_active &&
		strcasecmp(original_message_payload, room->current_answer) == 0) {

		char correct_msg[BUFFER_SIZE];
		snprintf(correct_msg, BUFFER_SIZE, "ROOM_EVENT:[%s]���� ������ �������ϴ�! ������ '%s'�����ϴ�.\n",
			sender->nickname, room->current_answer);

		for (int i = 0; i < room->user_count; i++) {
			send_to_client(room->users[i]->sock, correct_msg);
		}

		send_to_client(sender->sock, "SMSG:�����Դϴ�!\n"); // ���� �޽���
		send_to_all_clients_in_room(room, "CLEAR\n");       // �׸��� Ŭ���� ����

		room->round_active = 0;
		room->current_drawer = sender; // �� ������ ����
		start_new_round(room);         // ���� ���� ����

		return;
	}

	// ���� ó��
	send_to_client(sender->sock, "SMSG:�����Դϴ�.\n");

	// �Ϲ� ä�� �޽��� ����
	char msg_to_broadcast[BUFFER_SIZE];
	snprintf(msg_to_broadcast, BUFFER_SIZE, "MSG:[%s] %s\n", sender->nickname, original_message_payload);
	for (int i = 0; i < room->user_count; i++) {
		if (room->users[i]->sock != sender->sock) {
			send_to_client(room->users[i]->sock, msg_to_broadcast);
		}
	}
}

void broadcast_draw_data_to_room(ClientInfo* sender, const char* draw_message_line_with_newline) {
	if (sender->roomID == -1 || !sender->has_set_initial_nick) {
		return;
	}

	Room* room = get_room_by_id(sender->roomID);
	if (!room) return;

	// �����ڰ� �ƴ� ��� �׸��� ���� ����
	if (room->drawer_sock != sender->sock) {
		send_to_client(sender->sock, "SMSG:�����ڸ� �׸��� �׸� �� �ֽ��ϴ�.\n");
		return;
	}

	// broadcast to others
	for (int i = 0; i < room->user_count; i++) {
		if (room->users[i]->sock != sender->sock) {
			send_to_client(room->users[i]->sock, draw_message_line_with_newline);
		}
	}
}


const char* global_word_list[] = { "Dog", "Car", "Hamburger", "Cap", "Chair", "Cat", "Book", "Cow", "ant", "Spider" };
int global_word_list_size = sizeof(global_word_list) / sizeof(global_word_lsit[0]);;

void start_new_round(Room* room) {
	if (!room || room->user_count == 0 || word_list_size == 0) return;

	ClientInfo* drawer = NULL;

	// ���� �����ڰ� ������ �濡 �ִ��� Ȯ��
	int found = 0;
	if (room->current_drawer != NULL) {
		for (int i = 0; i < room->user_count; ++i) {
			if (room->users[i] == room->current_drawer) {
				found = 1;
				break;
			}
		}
	}
	// ��ȿ�� �����ڰ� ������ ���� ���� ����
	if (!found) {
		int drawer_index = rand() % room->user_count;
		drawer = room->users[drawer_index];
		room->current_drawer = drawer;
	}
	else {
		drawer = room->current_drawer;
	}
	// ���� �ܾ� ����
	int word_index = rand() % word_list_size;
	strncpy(room->current_answer, word_list[word_index], sizeof(room->current_answer) - 1);
	room->current_answer[sizeof(room->current_answer) - 1] = '\0';

	// ����� �ܾ� ����
	for (int i = word_index; i < room->word_count - 1; i++) {
		room->word_list[i] = room->word_list[i + 1];
	}
	room->word_count--;
	room->drawer_sock = drawer->sock;
	room->round_active = 1;
	room->round_start_time = time(NULL);
	room->round_duration_seconds = 60;

	// Ÿ�̸� �޽��� ����
	char timer_msg[64];
	snprintf(timer_msg, sizeof(timer_msg), "TIMER:%d\n", room->round_duration_seconds);
	send_to_all_clients_in_room(room, timer_msg);

	// ���þ� ���� �� ������ �˸�
	char msg[BUFFER_SIZE];
	snprintf(msg, BUFFER_SIZE, "SMSG:���þ�� '%s'�Դϴ�. �׷��ּ���.\n", room->current_answer);
	send_to_client(drawer->sock, msg);

	snprintf(msg, BUFFER_SIZE, "ROOM_EVENT:[%s]���� �������Դϴ�. ������ ����������!\n", drawer->nickname);
	for (int i = 0; i < room->user_count; ++i) {
		if (room->users[i]->sock != drawer->sock) {
			send_to_client(room->users[i]->sock, msg);
		}
	}
}




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
			return 1;
		}
	}
	return 0;
}

void add_client_to_room(Room* room, ClientInfo* client) {
	if (room->user_count < MAX_USERS_PER_ROOM) {
		room->users[room->user_count++] = client;
		client->roomID = room->id;
		char smsg[BUFFER_SIZE];
		snprintf(smsg, BUFFER_SIZE, "SMSG:�� '%s'(ID:%d)�� �����߽��ϴ�. �׸��� UI�� Ȱ��ȭ�˴ϴ�.\n", room->name, room->id);
		send_to_client(client->sock, smsg);

		char enter_msg[BUFFER_SIZE];
		snprintf(enter_msg, BUFFER_SIZE, "ROOM_EVENT:[%s]���� �����߽��ϴ�.\n", client->nickname);
		for (int i = 0; i < room->user_count; i++) {
			if (room->users[i]->sock != client->sock) {
				send_to_client(room->users[i]->sock, enter_msg);
			}
		}
	}
	else {
		send_to_client(client->sock, "SMSG:���� ���� á���ϴ�.\n");
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
		snprintf(leave_msg, BUFFER_SIZE, "ROOM_EVENT:[%s]���� �����߽��ϴ�.\n", client->nickname);
		for (int i = 0; i < room->user_count; i++) {
			send_to_client(room->users[i]->sock, leave_msg);
		}
		printf("[����] %s���� �� '%s'(ID:%d)���� ����.\n", client->nickname, room->name, room->id);
	}
	client->roomID = -1;
	if (room->user_count == 0 && room_count > 0) {
		printf("[����] �� '%s'(ID:%d)��(��) �� �����˴ϴ�.\n", room->name, room->id);
		int room_idx = -1;
		for (int i = 0; i < room_count; ++i) if (rooms[i].id == room->id) room_idx = i;

		if (room_idx != -1) {
			for (int i = room_idx; i < room_count - 1; ++i) {
				rooms[i] = rooms[i + 1];
			}
			room_count--;
			// rooms[room_count]�� ������ �ʱ�ȭ�� �ʿ�� ����. room_count�� �پ����Ƿ� ���� �ȵ�.
		}
	}
}

void handle_client_disconnection(int client_sock) {
	ClientInfo* client = get_client_by_sock(client_sock);
	if (!client) return;

	printf("[����] Ŭ���̾�Ʈ %s (sock %d) ���� ���� ó�� ����.\n", client->has_set_initial_nick ? client->nickname : "�͸�", client_sock);
	if (client->roomID != -1) {
		Room* room = get_room_by_id(client->roomID);
		if (room) {
			remove_client_from_room(client, room);
		}
	}
	close(client_sock);
	free(clients[client_sock]);
	clients[client_sock] = NULL;
	printf("[����] Ŭ���̾�Ʈ (sock %d) ���ҽ� ���� �Ϸ�.\n", client_sock);
}

void handle_nick_command(ClientInfo* client, const char* nick_arg) {
	if (strlen(nick_arg) == 0 || strlen(nick_arg) >= NICK_SIZE) {
		send_to_client(client->sock, "SMSG:�г��� ������ �ùٸ��� �ʽ��ϴ�. (1~31��)\n");
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
	snprintf(smsg, BUFFER_SIZE, "SMSG:�г����� '%s'(��)�� �����Ǿ����ϴ�.\n", client->nickname);
	send_to_client(client->sock, smsg);

	if (client->roomID != -1 && was_initial_nick_set) {
		char nick_change_broadcast[BUFFER_SIZE];
		snprintf(nick_change_broadcast, BUFFER_SIZE, "ROOM_EVENT:['%s'���� '%s'(��)�� �г����� �����߽��ϴ�.]\n", old_nick, client->nickname);
		Room* room = get_room_by_id(client->roomID);
		if (room) {
			for (int i = 0; i < room->user_count; ++i) {
				if (room->users[i]->sock != client->sock) send_to_client(room->users[i]->sock, nick_change_broadcast);
			}
		}
	}
}

void handle_create_room_command(ClientInfo* client, const char* room_name_arg) {
	if (!client->has_set_initial_nick) {
		send_to_client(client->sock, "ERR_NICK_REQUIRED:�� ���� ���� /nick <�г���> ���� �ʿ�\n");
		return;
	}
	if (client->roomID != -1) {
		send_to_client(client->sock, "SMSG:�̹� �ٸ� �濡 �������Դϴ�. ���� �������ּ���. (/exit)\n");
		return;
	}
	if (room_count >= MAX_ROOMS) {
		send_to_client(client->sock, "SMSG:�� �̻� ���� ���� �� �����ϴ�.\n");
		return;
	}
	if (strlen(room_name_arg) == 0 || strlen(room_name_arg) >= ROOM_NAME_SIZE) {
		send_to_client(client->sock, "SMSG:�� �̸��� ��ȿ���� �ʽ��ϴ�.\n");
		return;
	}
	if (get_room_by_name(room_name_arg) != NULL) {
		send_to_client(client->sock, "SMSG:�̹� �����ϴ� �� �̸��Դϴ�.\n");
		return;
	}

	Room* new_room = &rooms[room_count];
	new_room->id = next_room_id++;
	strncpy(new_room->name, room_name_arg, ROOM_NAME_SIZE - 1);
	new_room->name[ROOM_NAME_SIZE - 1] = '\0';
	new_room->user_count = 0;

	// �溰 �ܾ� ����Ʈ �ʱ�ȭ
	for (int i = 0; i < global_word_list_size; i++) {
		new_room->word_list[i] = global_word_list
			[i];
	}
	new_room->word_count = global_word_list_size;

	room_count++;

	add_client_to_room(new_room, client);
	start_new_round(new_room);
}

void handle_join_room_command(ClientInfo* client, const char* room_identifier_arg) {
	if (!client->has_set_initial_nick) {
		send_to_client(client->sock, "ERR_NICK_REQUIRED:�� ���� ���� /nick <�г���> ���� �ʿ�\n");
		return;
	}
	if (client->roomID != -1) {
		send_to_client(client->sock, "SMSG:�̹� �ٸ� �濡 �������Դϴ�. ���� �������ּ���. (/exit)\n");
		return;
	}
	if (strlen(room_identifier_arg) == 0) {
		send_to_client(client->sock, "SMSG:������ �� ��ȣ �Ǵ� �� �̸��� �Է��ϼ���.\n");
		return;
	}

	Room* room_to_join = NULL;
	int room_id_attempt = atoi(room_identifier_arg);
	if (room_id_attempt > 0) {
		room_to_join = get_room_by_id(room_id_attempt);
	}
	if (!room_to_join) {
		room_to_join = get_room_by_name(room_identifier_arg);
	}

	if (!room_to_join) {
		send_to_client(client->sock, "SMSG:�������� �ʴ� ���̰ų� �߸��� �Է��Դϴ�.\n");
		return;
	}

	add_client_to_room(room_to_join, client);

	// ���� �� �ڵ� ���� ���� ���� Ȯ��
	if (room_to_join->user_count >= 2 && !room_to_join->round_active && word_list_size > 0) {
		printf("[����] �� �� �̻��� ���������Ƿ� ���� ����\n");
		start_new_round(room_to_join);
	}
}


void handle_list_rooms_command(ClientInfo* client) {
	char list_buffer[BUFFER_SIZE] = "ROOMLIST:";
	int current_len = strlen(list_buffer);
	int first_room = 1;

	if (room_count == 0) {
		strcat(list_buffer, "���� ������ ���� �����ϴ�.\n");
	}
	else {
		for (int i = 0; i < room_count; i++) {
			if (rooms[i].id > 0) {
				char room_detail[128];
				if (!first_room) {
					if (current_len + 1 < BUFFER_SIZE - 1) {
						strcat(list_buffer, ";"); current_len += 1;
					}
					else break;
				}
				snprintf(room_detail, sizeof(room_detail), "%d-%s(%d)", rooms[i].id, rooms[i].name, rooms[i].user_count);
				if (current_len + strlen(room_detail) < BUFFER_SIZE - 1) {
					strcat(list_buffer, room_detail);
					current_len += strlen(room_detail);
					first_room = 0;
				}
				else {
					if (current_len + 4 < BUFFER_SIZE - 1) strcat(list_buffer, "..."); current_len += 3;
					break;
				}
			}
		}
		if (list_buffer[current_len - 1] == ';') list_buffer[current_len - 1] = '\n';
		else if (first_room && room_count > 0) {} // ���� ������ ��ȿ�� ���� ���� �ƹ��͵� ������ ���
		else strcat(list_buffer, "\n");
	}
	send_to_client(client->sock, list_buffer);
}

void handle_exit_room_command(ClientInfo* client) {
	if (client->roomID == -1) {
		send_to_client(client->sock, "SMSG:���� � �濡�� �����ϰ� ���� �ʽ��ϴ�.\n");
		return;
	}
	Room* room = get_room_by_id(client->roomID);
	if (room) {
		remove_client_from_room(client, room);
	}
	send_to_client(client->sock, "SMSG:�濡�� �����߽��ϴ�. �κ�� ���ư��ϴ�.\n");
}

void process_client_message(ClientInfo* client, const char* message_line_no_newline) { // ���� ���� �޽��� ����
	printf("[���� ����] (sock %d, nick %s, room %d): %s\n", client->sock, client->nickname, client->roomID, message_line_no_newline);

	if (strncmp(message_line_no_newline, "NICK:", 5) == 0) {
		handle_nick_command(client, message_line_no_newline + 5);
	}
	else if (strncmp(message_line_no_newline, "CREATE_ROOM:", 12) == 0) {
		handle_create_room_command(client, message_line_no_newline + 12);
	}
	else if (strncmp(message_line_no_newline, "JOIN_ROOM:", 10) == 0) {
		handle_join_room_command(client, message_line_no_newline + 10);
	}
	else if (strcmp(message_line_no_newline, "LIST_ROOMS") == 0) {
		handle_list_rooms_command(client);
	}
	else if (strcmp(message_line_no_newline, "EXIT_ROOM") == 0) {
		handle_exit_room_command(client);
	}
	else if (strncmp(message_line_no_newline, "MSG:", 4) == 0) {
		broadcast_message_to_room(client, message_line_no_newline + 4);
	}
	else if (strncmp(message_line_no_newline, "DRAW_POINT:", 11) == 0 ||
		strncmp(message_line_no_newline, "DRAW_LINE:", 10) == 0 ||
		strcmp(message_line_no_newline, "DRAW_CLEAR") == 0) {
		char draw_message_with_newline[BUFFER_SIZE];
		snprintf(draw_message_with_newline, BUFFER_SIZE, "%s\n", message_line_no_newline); // ���� �߰��ؼ� ����
		broadcast_draw_data_to_room(client, draw_message_with_newline);
	}
	else if (strcmp(message_line_no_newline, "QUIT") == 0) {
		printf("[����] Ŭ���̾�Ʈ %s (sock %d)�� QUIT ��û. ���� ���� ó�� ����.\n", client->nickname, client->sock);
		// ���� ���� ����� recv <= 0 �� �� handle_client_disconnection���� ó����
	}
	else {
		send_to_client(client->sock, "SMSG:�� �� ���� ������ �޽��� �Ǵ� ��ɾ��Դϴ�.\n");
	}
}


int main() {
	srand(time(NULL));
	int listen_sock, new_sock_fd;
	struct sockaddr_in server_address, client_address;
	socklen_t client_address_len;
	fd_set master_fd_set, read_fd_set;
	int max_fd;

	for (int i = 0; i < MAX_CLIENTS; i++) clients[i] = NULL;
	for (int i = 0; i < MAX_ROOMS; i++) { rooms[i].id = 0; rooms[i].user_count = 0; }

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
	if (listen(listen_sock, MAX_CLIENTS) < 0) { // SOMAXCONN ��� MAX_CLIENTS
		perror("listen"); close(listen_sock); exit(EXIT_FAILURE);
	}

	FD_ZERO(&master_fd_set);
	FD_SET(listen_sock, &master_fd_set);
	max_fd = listen_sock;

	printf("[����] ĳġ���ε� ���� ���� ���� �� (��Ʈ: %d)...\n", SERVER_PORT);

	while (1) {
		read_fd_set = master_fd_set;
		if (select(max_fd + 1, &read_fd_set, NULL, NULL, NULL) < 0) {
			if (errno == EINTR) continue;
			perror("select error"); break;
		}
		for (int i = 0; i < MAX_ROOMS; ++i) {
			Room* room = &rooms[i];
			if (room->id > 0 && room->round_active && room->user_count > 0) {
				time_t now = time(NULL);
				if (now - room->round_start_time >= room->round_duration_seconds) {
					// �ð� �ʰ�: ���ο� ���� ����
					char msg[] = "ROOM_EVENT:�ð� �ʰ�! �����ڰ� �ڵ����� ����˴ϴ�.\n";
					send_to_all_clients_in_room(room, msg);
					room->round_active = 0; // ���� ���� ����
					room->current_drawer = NULL;
					start_new_round(room);  // ���� ���� ����
				}
			}
		}
		for (int sock_fd = 0; sock_fd <= max_fd; sock_fd++) {
			if (FD_ISSET(sock_fd, &read_fd_set)) {
				if (sock_fd == listen_sock) {
					client_address_len = sizeof(client_address);
					new_sock_fd = accept(listen_sock, (struct sockaddr*)&client_address, &client_address_len);
					if (new_sock_fd < 0) { perror("accept"); continue; }

					if (new_sock_fd >= MAX_CLIENTS) {
						fprintf(stderr, "[����] �ִ� Ŭ���̾�Ʈ ��(%d) ����. ���� �ź� (fd %d).\n", MAX_CLIENTS, new_sock_fd);
						send_to_client(new_sock_fd, "SMSG:������ ���� á���ϴ�.\n");
						close(new_sock_fd);
						continue;
					}

					clients[new_sock_fd] = (ClientInfo*)malloc(sizeof(ClientInfo));
					if (!clients[new_sock_fd]) { perror("malloc for client failed"); close(new_sock_fd); continue; }

					clients[new_sock_fd]->sock = new_sock_fd;
					strcpy(clients[new_sock_fd]->nickname, "�͸�");
					clients[new_sock_fd]->roomID = -1;
					clients[new_sock_fd]->has_set_initial_nick = 0;
					clients[new_sock_fd]->input_len = 0;
					memset(clients[new_sock_fd]->input_buffer, 0, INPUT_BUFFER_SIZE);

					FD_SET(new_sock_fd, &master_fd_set);
					if (new_sock_fd > max_fd) max_fd = new_sock_fd;
					printf("[����] �� Ŭ���̾�Ʈ ����: %s (sock %d)\n", inet_ntoa(client_address.sin_addr), new_sock_fd);
					send_to_client(new_sock_fd, "SMSG:������ ����Ǿ����ϴ�. ������Դϴ�. ���� /nick <���ϴ´г���> ���� �г����� �������ּ���.\nSMSG:�г��� ���� �� /list, /create <���̸�>, /join <��ID/�̸�> ��ɾ ����� �� �ֽ��ϴ�.\n");

				}
				else {
					ClientInfo* client = get_client_by_sock(sock_fd);
					if (!client) continue;

					char recv_temp_buffer[BUFFER_SIZE];
					int nbytes = recv(sock_fd, recv_temp_buffer, BUFFER_SIZE - 1, 0);

					if (nbytes <= 0) {
						if (nbytes == 0) printf("[����] Ŭ���̾�Ʈ %s (sock %d) ���� ���� ����.\n", client->has_set_initial_nick ? client->nickname : "�͸�", sock_fd);
						else perror("recv from client failed");
						FD_CLR(sock_fd, &master_fd_set);
						handle_client_disconnection(sock_fd);
					}
					else {
						recv_temp_buffer[nbytes] = '\0';
						if (client->input_len + nbytes < INPUT_BUFFER_SIZE) {
							strncat(client->input_buffer, recv_temp_buffer, nbytes);
							client->input_len += nbytes;
						}
						else {
							fprintf(stderr, "[����] Ŭ���̾�Ʈ (sock %d) �Է� ���� �����÷ο�.\n", sock_fd);
							client->input_len = 0; memset(client->input_buffer, 0, INPUT_BUFFER_SIZE);
						}

						char* line_start = client->input_buffer;
						char* newline_ptr;
						while ((newline_ptr = strchr(line_start, '\n')) != NULL) {
							*newline_ptr = '\0';
							char single_line_msg[INPUT_BUFFER_SIZE];
							strcpy(single_line_msg, line_start);

							process_client_message(client, single_line_msg); // ���� ���� ���� �޽��� ����

							line_start = newline_ptr + 1;
						}
						if (line_start > client->input_buffer && strlen(line_start) > 0) {
							char temp_remaining_data[INPUT_BUFFER_SIZE];
							strcpy(temp_remaining_data, line_start);
							memset(client->input_buffer, 0, INPUT_BUFFER_SIZE);
							strcpy(client->input_buffer, temp_remaining_data);
							client->input_len = strlen(client->input_buffer);
						}
						else {
							client->input_len = 0;
							memset(client->input_buffer, 0, INPUT_BUFFER_SIZE);
						}
					}
				}
			}
		}
	}
	for (int i = 0; i <= max_fd; ++i) if (FD_ISSET(i, &master_fd_set) && clients[i] && i != listen_sock) handle_client_disconnection(i);
	close(listen_sock);
	printf("[����] ���� ����.\n");
	return 0;
}