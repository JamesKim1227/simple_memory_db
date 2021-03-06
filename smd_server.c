/* smd server  */

#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <errno.h>
#include <stdlib.h>
#include <execinfo.h>
#include <unistd.h>
#include <fcntl.h>

#include <apr_general.h>
#include <apr_hash.h>
#include <apr_strings.h>

/* MACRO */
#define CMD_UNKNOWN 	0
#define CMD_SET     	1
#define CMD_GET     	2
#define CMD_SAVE		3
#define CMD_QUIT    	4
#define CMD_SLAVE		5
#define CMD_REGISTER	6
#define CMD_PING		7

#define SMD_ADD_EVENT   0
#define SMD_MOD_EVENT   1
#define SMD_DEL_EVENT   2

/* structures */
struct smd_event_loop;

typedef void smd_event_handler(int fd, void *data);

typedef struct smd_epoll_state {
	int epoll_fd;
	struct epoll_event *epoll_events;
} smd_epoll_state;

// To do : need to implement client instead of using client_data in smd_event just as a result buffer.
typedef struct client {
	int fd;
	apr_pool_t *client_mp;
	void *buf;
} client;

typedef struct smd_event {
	int mask;
	smd_event_handler *read_event_handler;
	smd_event_handler *write_event_handler;
	void *client_data;
	char *ip;
	int port;
} smd_event;

typedef struct smd_event_loop {
	smd_event *events;
	smd_epoll_state *epoll_state;
} smd_event_loop;

typedef struct smd_slave {
	int fd;
	char ip[16];
	int port;
	apr_pool_t *slave_mp;
	void *data;
} smd_slave;

struct smd_server {
	int port;
	int tcp_backlog;    /* TCP listen backlog  */
	int fd;
	char *master_host;
	int master_port;

	smd_slave slaves[1024];
	int slave_idx;

	smd_event_loop *event_loop;

	apr_pool_t *memory_pool;
	apr_hash_t *hash_table;
};


/* function definition */
void set_event(int fd, struct sockaddr_in *client_info, int flag, smd_event_handler *read_handler, smd_event_handler *write_handler); 
int process_command(int fd, char *buf);
void send_result_to_client(int fd, void *dataa);

/* gobal varialbes */
struct smd_server server;


smd_event_loop *smd_create_event_loop(int size) {
	smd_event_loop *event_loop      = NULL;
	smd_epoll_state *epoll_state    = NULL;
	smd_event *event                = NULL;

	event_loop = (smd_event_loop *)apr_palloc(server.memory_pool, sizeof(smd_event_loop));
	if (event_loop == NULL) { 
		goto error;
	}

	/* epoll create */
	epoll_state = (smd_epoll_state *)apr_palloc(server.memory_pool, sizeof(smd_epoll_state));
	if (epoll_state == NULL) {
		goto error;
	}

	epoll_state->epoll_fd = epoll_create(size);
	if (epoll_state->epoll_fd == -1) {
		goto error;
	}

	epoll_state->epoll_events = (struct epoll_event*)apr_palloc(server.memory_pool, sizeof(struct epoll_event) * 10);

	event_loop->epoll_state = epoll_state;

	event = (smd_event *)apr_palloc(server.memory_pool, sizeof(smd_event)*1024);
	if (event == NULL) {
		goto error;
	}

	event_loop->events = event;

	return event_loop;

error:
	return NULL;
}

void init_server_config(char *port) {
	server.port         = atoi(port);
	server.tcp_backlog  = 512;

	server.event_loop   = NULL;

	server.memory_pool  = NULL;
	server.hash_table   = NULL;

	server.slave_idx = 0;
}

int lookup_command(char *buf) {
	if (buf == NULL) return -1;

	if (strncasecmp("set", buf, 3) == 0) {
		return CMD_SET;
	} else if (strncasecmp("get", buf, 3) == 0) {
		return CMD_GET;
	} else if (strncasecmp("save", buf, 4) == 0) {
		return CMD_SAVE;
	} else if (strncasecmp("slave", buf, 5) == 0) {
		return CMD_SLAVE;
	} else if (strncasecmp("register", buf, 8) == 0) {
		return CMD_REGISTER;
	} else if (strncasecmp("ping", buf, 4) == 0) {
		return CMD_PING;
	} else if (strncasecmp("quit", buf, 4) == 0) {
		return CMD_QUIT;
	}

	return CMD_UNKNOWN;
}

void smd_send_data_to_slaves(char *key, char *value) {
	int i;
	char buf[1024];

	for (i=0; i<server.slave_idx; i++) {
		snprintf(buf, 1023, "set %s %s\n", key, value);
		buf[1023] = '\0';
		send_result_to_client(server.slaves[i].fd, buf);
	}
}


void smd_set_value(char *key, void *value) {
	apr_hash_set(server.hash_table, apr_pstrdup(server.memory_pool, key), APR_HASH_KEY_STRING, apr_pstrdup(server.memory_pool, value));

	smd_send_data_to_slaves(key, value);
}

void *smd_get_value(char *key) {
	return apr_hash_get(server.hash_table, key, APR_HASH_KEY_STRING);
}

void read_query_from_client(int fd, void *data) {
	int nread;
	char buf[1024] ={0,};

	nread = read(fd, buf, 1023);
	if (nread == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) return;

		printf("read error: %s\n", strerror(errno));
		set_event(fd, NULL, SMD_DEL_EVENT, NULL, NULL);
		close(fd);
		return;
	} else if (nread == 0) { // connection closed
		set_event(fd, NULL, SMD_DEL_EVENT, NULL, NULL);
		close(fd);
		return;
	}

	process_command(fd, buf);  
}

void send_result_to_client(int fd, void *data) {
	int nsend;

	if (data == NULL) return;

	nsend = write(fd, data, strlen(data));
	if (nsend == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) return;
		printf("send error: %s\n", strerror(errno));
		set_event(fd, NULL, SMD_DEL_EVENT, NULL, NULL);
		close(fd);
		return;
	}
}

int save_data_to_file() {
	apr_hash_index_t *hi;
	void *key, *val;
	FILE *fp;

	fp = fopen("smd_data", "w+");
	if (!fp) return -1;

	hi = apr_hash_first(server.memory_pool, server.hash_table);
	while (hi) {
		val = NULL;
		apr_hash_this(hi, (const void**)&key, NULL, &val);

		if (key && val)
			fprintf(fp, "set %s %s\n", (char*)key, (char*)val);

		hi = apr_hash_next(hi);
	}
	fclose(fp);

	return 0;
}

void load_data_from_file() {
	FILE *fp;
	char *line = NULL;
	size_t len = 0;

	fp = fopen("smd_data", "r");
	if (!fp) return;

	while (getline(&line, &len, fp) != -1) {
		int cmd;
		char *command, *key, *value;
		char *ptr;

		command = strtok_r(line, " ", &ptr);
		key = strtok_r(NULL, " ", &ptr);

		cmd = lookup_command(command);
		if (cmd == -1) {
			printf("Invalid Command\n");
			continue;
		}

		switch (cmd) {
			case CMD_SET:
				value = strtok_r(NULL, " ", &ptr);
				smd_set_value(key, value);
				break;
		}
	}
	if (line) free(line);

	printf("smd_data file successfully loaded\n");
	return;
}

int smd_save() {
	int pid;

	pid = fork();
	if (pid == -1) {
		return -1;
	} else if (pid == 0) {
		// child
		save_data_to_file();	
		exit(0);
	}

	return 0;
}

int request_to_master() {
	struct sockaddr_in master_addr;
	int master_sock;
	int ret;
	char buf[8];

	if ((master_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		return -1;
	}

	memset((void*)&master_addr, 0x00, sizeof(master_addr));
	master_addr.sin_family = AF_INET;
	master_addr.sin_addr.s_addr = inet_addr(server.master_host);
	master_addr.sin_port = htons(server.master_port);

	if (connect(master_sock, (struct sockaddr*)&master_addr, sizeof(master_addr)) == -1) {
		printf("connect error\n");
		goto error;
	}

	sprintf(buf, "REGISTER %d", server.port);
	if ((ret = write(master_sock, buf, strlen(buf))) <= 0) {
		printf("write error\n");
		goto error;
	}

	if ((ret = read(master_sock, buf, 8)) <= 0) {
		printf("read error\n");
		goto error;
	}

	printf("buf: %s\n", buf);
	if (strncmp(&buf[0], "1", 1)) {
		printf("result error\n");
		goto error;
	}

	close(master_sock);
	return 0;
error:
	close(master_sock);
	printf("Err: %s\n", strerror(errno));
	return -1;
}

int smd_set_slave(char *ip, char *port) {
	int ret;

	server.master_host = apr_pstrdup(server.memory_pool, ip);
	server.master_port = atoi(port);
	
	ret = request_to_master();

	return ret;
}

int smd_connect_to_slave(int idx) {
	struct sockaddr_in addr;
	int sock;
	int ret;
	char buf[8];

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		return -1;
	}

	printf("[%s]:%s():%d  %s, %d\n", __FILE__, __FUNCTION__, __LINE__, server.slaves[idx].ip, server.slaves[idx].port);
	memset((void*)&addr, 0x00, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(server.slaves[idx].ip);
	addr.sin_port = htons(server.slaves[idx].port);

	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		printf("connect error\n");
		goto error;
	}

	printf("[%s]:%s():%d  \n", __FILE__, __FUNCTION__, __LINE__);
	if ((ret = write(sock, "PING", strlen("PONG"))) <= 0) {
		printf("write error\n");
		goto error;
	}

	printf("[%s]:%s():%d  \n", __FILE__, __FUNCTION__, __LINE__);
	if ((ret = read(sock, buf, 8)) <= 0) {
		printf("read error\n");
		goto error;
	}

	printf("[%s]:%s():%d  %s\n", __FILE__, __FUNCTION__, __LINE__, buf);
	if (strncmp(buf, "PONG", 4)) {
		printf("result error\n");
		goto error;
	}

	return sock;
error:
	close(sock);
	printf("Err: %s\n", strerror(errno));
	return -1;
}

int smd_register_slave(char *ip, char *port) {
	
	strncpy(server.slaves[server.slave_idx].ip, ip, 16);
	server.slaves[server.slave_idx].ip[15] = '\0';

	server.slaves[server.slave_idx].port = atoi(port);
	
	printf("[%s]:%s():%d slave info[%d] ip:%s, port:%d\n", __FILE__, __FUNCTION__, __LINE__, 
			server.slave_idx,
			server.slaves[server.slave_idx].ip,
			server.slaves[server.slave_idx].port);

	server.slaves[server.slave_idx].fd = smd_connect_to_slave(server.slave_idx);

	server.slave_idx++;

	return 0;
}

int smd_full_sync(char *ip, char *port) {
	int pid;

	pid = fork();
	if (pid == -1) {
		return -1;
	} else if (pid == 0) {
		// child
		int i, fd = -1;
		apr_hash_index_t *hi;
		void *key, *val;
		char found = 0;
		char buf[1024];

		for (i=0; i<server.slave_idx; i++) {
			printf("[%s]:%s():%d slave info[%d] ip:%s, port:%d\n", __FILE__, __FUNCTION__, __LINE__, 
					i,
					server.slaves[i].ip,
					server.slaves[i].port);
			if (strcmp(server.slaves[i].ip, ip) == 0 &&
					server.slaves[i].port == atoi(port)) {
				found = 1;
				fd = server.slaves[i].fd;
				break;
			}
		}

		if (found == 0 || fd < 0) {
			printf("Can't find one of slaves\n");
			exit(0);
		}


		hi = apr_hash_first(server.memory_pool, server.hash_table);
		while (hi) {
			val = NULL;
			apr_hash_this(hi, (const void**)&key, NULL, &val);

			if (key && val) {
				snprintf(buf, 1023, "set %s %s\n", (char*)key, (char*)val);
				buf[1023] = '\0';
				send_result_to_client(fd, buf);
			}

			hi = apr_hash_next(hi);
		}
		printf("Full Sync completed\n");
		exit(0);
	}

	return 0;
}

int process_command(int fd, char *buf) {
	int cmd, ret;
	void *data;
	char *command, *key, *value;
	char *ptr;

	printf("[%s]:%s():%d  buf:%s\n", __FILE__, __FUNCTION__, __LINE__, buf);
	if (buf == NULL) return -1;

	command = strtok_r(buf, " \n", &ptr);
	key = strtok_r(NULL, " \n", &ptr);

	cmd = lookup_command(command);
	if (cmd == -1) {
		printf("Invalid Command\n");
		return -1;
	}

	smd_event *e = &server.event_loop->events[fd];

	if (cmd == CMD_SET || cmd == CMD_GET || cmd == CMD_SLAVE) {
		if (key == NULL) {
			send_result_to_client(fd, "Key is empty");
			return -1;
		}
	}

	switch (cmd) {
		case CMD_SET:
			value = strtok_r(NULL, " \n", &ptr);
			if (value == NULL) {
				send_result_to_client(fd, "Value is empty");
				return -1;
			}
			smd_set_value(key, value);
			send_result_to_client(fd, "OK");
			break;
		case CMD_GET:
			data = smd_get_value(key);
			e->client_data = apr_psprintf(server.memory_pool, "%s", data?(char*)data : "");
			break;
		case CMD_SAVE:
			smd_save();
			send_result_to_client(fd, "SAVE SUCCESS");
			break;
		case CMD_SLAVE:
			value = strtok_r(NULL, " \n", &ptr);
			if (value == NULL) {
				send_result_to_client(fd, "Value is empty");
				return -1;
			}
			ret = smd_set_slave(key, value);
			if (ret == 0) {
				send_result_to_client(fd, "OK");
			} else {
				send_result_to_client(fd, "Failed");
			}
			break;
		case CMD_REGISTER:
			printf("MASTER REGISTER, ip: %s, port: %s\n", e->ip, key);
			send_result_to_client(fd, "1");
			close(fd);
			
			smd_register_slave(e->ip, key);

			smd_full_sync(e->ip, key);

			break;
		case CMD_PING:
			e->client_data = apr_psprintf(server.memory_pool, "%s", "PONG");
			break;
		case CMD_QUIT:
			send_result_to_client(fd, "closing connection...");
			set_event(fd, NULL, SMD_DEL_EVENT, NULL, NULL);
			sleep(1);
			close(fd);
			break;
		case CMD_UNKNOWN:
			e->client_data = apr_psprintf(server.memory_pool, "%s", "unknown command");
			break;
		default:
			return -1;
	}

	return 0;
}


void accept_handler(int fd, void *data) {
	socklen_t addr_len;
	int client_fd;
	struct sockaddr_in client_addr;

	addr_len = sizeof(client_addr);

	client_fd = accept(server.fd, (struct sockaddr*)&client_addr, &addr_len);
	if (client_fd == -1) {
		goto error;
	}

	/* add event  */
	set_event(client_fd, &client_addr, SMD_ADD_EVENT, read_query_from_client, send_result_to_client);

	return;
error:
	printf("Error: %s\n", strerror(errno));
	apr_pool_destroy(server.memory_pool);
	apr_terminate();
	exit(1);
}

void init_server_socket() {
	struct sockaddr_in server_addr;

	/* TCP listen  */
	if ((server.fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		goto error;
	}

	memset((void*)&server_addr, 0x00, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(server.port);

	int option = 1;
	setsockopt(server.fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

	if (bind(server.fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
		goto error;
	}

	if (listen(server.fd, server.tcp_backlog) == -1) {
		goto error;
	}

	return;

error:
	printf("Error: %s\n", strerror(errno));
	apr_pool_destroy(server.memory_pool);
	apr_terminate();
	exit(1);
}

void set_event(int fd, struct sockaddr_in *client_info, int flag, smd_event_handler *read_handler, smd_event_handler *write_handler) {
	int op;
	struct epoll_event ee = {0};
	smd_epoll_state *state = server.event_loop->epoll_state;

	ee.events = 0;
	ee.events |= EPOLLIN;

	if (flag == SMD_ADD_EVENT) op = EPOLL_CTL_ADD;
	else if (flag == SMD_MOD_EVENT) op = EPOLL_CTL_MOD;
	else if (flag == SMD_DEL_EVENT) op = EPOLL_CTL_DEL;

	ee.data.fd = fd;
	if (epoll_ctl(state->epoll_fd, op, fd, &ee) == -1) {
		goto error;
	}

	int flags = fcntl(fd,F_GETFL,0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	smd_event *e = &server.event_loop->events[fd];

	e->read_event_handler = read_handler;
	e->write_event_handler = write_handler;
	e->client_data = NULL;

	if (client_info == NULL) return;

	e->ip = apr_pstrdup(server.memory_pool, inet_ntoa(client_info->sin_addr));
	e->port = ntohs(client_info->sin_port);

	return;
error:
	printf("Error: %s\n", strerror(errno));
	apr_pool_destroy(server.memory_pool);
	apr_terminate();
	exit(1);
}

void handler(int sig) {
	void *array[10];
	size_t size;

	size = backtrace(array, 10);

	fprintf(stderr, "Error: signal %d\n", sig);
	backtrace_symbols_fd(array, size, STDERR_FILENO);
	exit(1);
}

void set_signal() {
	signal(SIGSEGV, handler);
	signal(SIGINT, handler);
}

void init_server(char *port) {

	set_signal();

	init_server_config(port);

	apr_initialize();

	apr_pool_create(&server.memory_pool, NULL);

	server.hash_table = apr_hash_make(server.memory_pool);

	server.event_loop = smd_create_event_loop(1024);
	if (server.event_loop == NULL) {
		goto error;
	}

	init_server_socket();

	/* add event  */
	set_event(server.fd, NULL, SMD_ADD_EVENT, accept_handler, NULL);

	load_data_from_file();
	return;
error:
	printf("Error: %s\n", strerror(errno));
	apr_pool_destroy(server.memory_pool);
	apr_terminate();
	exit(1);
}

void run() {
	int num_events, i;
	smd_epoll_state *state = NULL;

	while (1) {
		state = server.event_loop->epoll_state;

		num_events = epoll_wait(state->epoll_fd, (struct epoll_event*)state->epoll_events, 10, -1);

		if (num_events == -1) {
			printf("Error: %s\n", strerror(errno));
			exit(1);
		}

		for (i=0; i<num_events; i++) {
			struct epoll_event *ee = &state->epoll_events[i];
			smd_event *e = &server.event_loop->events[ee->data.fd];

			if (e->read_event_handler) {
				e->read_event_handler(ee->data.fd, e->client_data);
			}

			if (e->write_event_handler) {
				e->write_event_handler(ee->data.fd, e->client_data);
			}
		}
	}
}

void destroy_server() {
	apr_pool_destroy(server.memory_pool);
	apr_terminate();
}

int main(int argc, char **argv) {

	init_server(argv[1]);

	/* running loop  */
	run(server.event_loop);

	/* free memory */
	destroy_server();

	return 0;
}

