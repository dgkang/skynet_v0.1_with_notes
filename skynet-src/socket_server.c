﻿#include "socket_server.h"
#include "socket_poll.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define MAX_INFO 128		     // MAX_SOCKET will be 2^MAX_SOCKET_P

#define MAX_SOCKET_P 16
#define MAX_EVENT 64          	 // 用于epoll_wait的第三个参数 每次返回事件的多少
#define MIN_READ_BUFFER 64    	 // 最小分配的读缓冲大小 为了减少read的调用 尽可能分配大的读缓冲区 muduo是用了reav来减少read系统调用次数的 它可以准备多块缓冲区
#define SOCKET_TYPE_INVALID 0 	 // 无效的sock fe
#define SOCKET_TYPE_RESERVE 1 	 // 预留已经被申请 即将被使用
#define SOCKET_TYPE_PLISTEN 2 	 // listen fd但是未加入epoll管理
#define SOCKET_TYPE_LISTEN 3     // 监听到套接字已经加入epoll管理
#define SOCKET_TYPE_CONNECTING 4 // 尝试连接的socket fd
#define SOCKET_TYPE_CONNECTED 5  // 已经建立连接的socket 主动conn或者被动accept的套接字 已经加入epoll管理
#define SOCKET_TYPE_HALFCLOSE 6  // 已经在应用层关闭了fd 但是数据还没有发送完 还没有close
#define SOCKET_TYPE_PACCEPT 7    // accept返回的fd 未加入epoll
#define SOCKET_TYPE_BIND 8       // 其他类型的fd 如 stdin stdout等

#define MAX_SOCKET (1<<MAX_SOCKET_P) // 1 << 16 -> 64K

// 发送缓冲区构成一个链表
struct write_buffer {
	struct write_buffer * next;
	char *ptr; // 指向当前未发送的数据首部
	int sz;
	void *buffer;
};

// 应用层的socket
struct socket {
	int fd;   			// 对应内核分配的fd
	int id;   			// 应用层维护的一个与fd对应的id 实际上是在socket池中的id
	int type; 			// socket类型或者状态
	int size; 			// 下一次read操作要分配的缓冲区大小
	int64_t wb_size;    // 发送缓冲区未发送的数据
	uintptr_t opaque;   // 在skynet中用于保存服务的handle
	struct write_buffer * head; // 发送缓冲区链表头指针和尾指针
	struct write_buffer * tail; //
};

struct socket_server {
	int recvctrl_fd;  				// 管道读端
	int sendctrl_fd;                // 管道写端
	int checkctrl; 					// 释放检测命令
	poll_fd event_fd;               // epoll fd
	int alloc_id;                   // 应用层分配id 用的
	int event_n;      				// epoll_wait 返回的事件数
	int event_index;  				// 当前处理的事件序号
	struct event ev[MAX_EVENT];     // epoll_wait返回的事件集
	struct socket slot[MAX_SOCKET]; // 应用层预先分配的socket
	char buffer[MAX_INFO];          // 临时数据的保存 比如保存对等方的地址信息等
	fd_set rfds; 					// 用于select的fd集
};

// 以下6个结构用于控制包体结构
struct request_open {
	int id;
	int port;
	uintptr_t opaque;
	char host[1];
};

struct request_send {
	int id;
	int sz;
	char * buffer;
};

struct request_close {
	int id;
	uintptr_t opaque;
};

struct request_listen {
	int id;
	int fd;
	uintptr_t opaque;
	char host[1];
};

struct request_bind {
	int id;
	int fd;
	uintptr_t opaque;
};

struct request_start {
	int id;
	uintptr_t opaque;
};

// 控制命令请求包
struct request_package {
	uint8_t header[8];	// 6 bytes dummy 6个字节未用 [0-5] [6]是type [7]长度 长度指的是包体的长度
	union {
		char buffer[256];
		struct request_open open;
		struct request_send send;
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_start start;
	} u;
	uint8_t dummy[256];
};

union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

#define MALLOC malloc
#define FREE free

static void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}

// 从socket池中获取一个空的socket 并为其分配一个id 2^31-1
// 在socket池中的位置 池的大小是64K socket_id的范围远大与64K
static int
reserve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		int id = __sync_add_and_fetch(&(ss->alloc_id), 1); // 原子的++
		// 小于0 已经最大了 说明 从0开始
		if (id < 0) {
			id = __sync_and_and_fetch(&(ss->alloc_id), 0x7fffffff);
		}
		struct socket *s = &ss->slot[id % MAX_SOCKET];// 从socket池中取出socket
		if (s->type == SOCKET_TYPE_INVALID) {
			// 如果相等就交换成 SOCKET_TYPE_RESERVE 设置为已用
			// 这里由于没有加锁 可能多个线程操作 所以还需要判断一次
			if (__sync_bool_compare_and_swap(&s->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {
				return id;
			}
			else {
				--i;// retry
			}
		}
	}
	return -1;
}

struct socket_server * 
socket_server_create() {
	int i;
	int fd[2];
	poll_fd efd = sp_create(); // epoll_create
	if (sp_invalid(efd)) {
		fprintf(stderr, "socket-server: create event pool failed.\n");
		return NULL;
	}

	// 创建一个管道
	if (pipe(fd)) {
		sp_release(efd);
		fprintf(stderr, "socket-server: create socket pair failed.\n");
		return NULL;
	}

	// epoll关注管道读端的可读事件
	if (sp_add(efd, fd[0], NULL)) {
		// add recvctrl_fd to event poll
		fprintf(stderr, "socket-server: can't add server fd to event pool.\n");
		close(fd[0]);
		close(fd[1]);
		sp_release(efd);
		return NULL;
	}

	struct socket_server *ss = MALLOC(sizeof(*ss));
	ss->event_fd = efd;
	ss->recvctrl_fd = fd[0]; // 管道读端
	ss->sendctrl_fd = fd[1]; // 管道写端
	ss->checkctrl = 1;

	// 初始化64K个socket
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		s->type = SOCKET_TYPE_INVALID;
		s->head = NULL;
		s->tail = NULL;
	}
	ss->alloc_id = 0;
	ss->event_n = 0;
	ss->event_index = 0;
	FD_ZERO(&ss->rfds); // 用于select的fd置为空 主要是用于命令通道
	assert(ss->recvctrl_fd < FD_SETSIZE);

	return ss;
}

static void
force_close(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	if (s->type == SOCKET_TYPE_INVALID) {
		return;
	}
	assert(s->type != SOCKET_TYPE_RESERVE);

	struct write_buffer *wb = s->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		FREE(tmp->buffer); // 释放节点指向的内存
		FREE(tmp); // 释放节点本身
	}
	s->head = s->tail = NULL;

	// 强制关闭的时候 如果type不为SOCKET_TYPE_PACCEPT SOCKET_TYPE_PACCEPT这2个是没有加入epoll管理的
	if (s->type != SOCKET_TYPE_PACCEPT && s->type != SOCKET_TYPE_PACCEPT) {
		sp_del(ss->event_fd, s->fd); // 从epoll中取消关注fd
	}
	if (s->type != SOCKET_TYPE_BIND) {
		close(s->fd);
	}
	s->type = SOCKET_TYPE_INVALID;
}

// 资源释放
void 
socket_server_release(struct socket_server *ss) {
	int i;
	struct socket_message dummy;
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		if (s->type != SOCKET_TYPE_RESERVE) {
			force_close(ss, s , &dummy);
		}
	}
	close(ss->sendctrl_fd); // 关闭管道
	close(ss->recvctrl_fd);
	sp_release(ss->event_fd); // free event fd
	FREE(ss);
}

// 从socket池中分配一个fd出来
static struct socket *
new_fd(struct socket_server *ss, int id, int fd, uintptr_t opaque, bool add) {
	struct socket * s = &ss->slot[id % MAX_SOCKET];
	assert(s->type == SOCKET_TYPE_RESERVE);

	if (add) {
		if (sp_add(ss->event_fd, fd, s)) { // 加入到epoll来管理这个fd
			s->type = SOCKET_TYPE_INVALID;
			return NULL;
		}
	}

	s->id = id;
	s->fd = fd;
	s->size = MIN_READ_BUFFER;
	s->opaque = opaque;
	s->wb_size = 0;
	assert(s->head == NULL);
	assert(s->tail == NULL);
	return s;
}

// return -1 when connecting
static int
open_socket(struct socket_server *ss, struct request_open * request, struct socket_message *result, bool blocking) {
	int id = request->id;
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	struct socket *ns;
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;
	char port[16];
	sprintf(port, "%d", request->port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	// getaddrinfo支持ipv6
	status = getaddrinfo( request->host, port, &ai_hints, &ai_list );
	if ( status != 0 ) {
		goto _failed;
	}
	int sock= -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if ( sock < 0 ) {
			continue;
		}
		socket_keepalive(sock);
		if (!blocking) {
			sp_nonblocking(sock);
		}
		status = connect( sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		if ( status != 0 && errno != EINPROGRESS) {
			close(sock);
			sock = -1;
			continue;
		}
		if (blocking) {
			sp_nonblocking(sock);
		}
		break;
	}

	if (sock < 0) {
		goto _failed;
	}

	ns = new_fd(ss, id, sock, request->opaque, true);
	if (ns == NULL) {
		close(sock);
		goto _failed;
	}

	// inet_ntop ipv4 ipv6
	if(status == 0) { // conn ok
		ns->type = SOCKET_TYPE_CONNECTED;
		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (ai_ptr->ai_family == AF_INET) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;
		if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
			result->data = ss->buffer;
		}
		freeaddrinfo( ai_list );
		return SOCKET_OPEN;
	} else {
		ns->type = SOCKET_TYPE_CONNECTING; // 非阻塞套接字处于连接中 应该关注它的可写事件epoll关注它是否连接成功
		sp_write(ss->event_fd, ns->fd, ns, true);
	}

	freeaddrinfo( ai_list );
	return -1;
_failed:
	freeaddrinfo( ai_list );
	ss->slot[id % MAX_SOCKET].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERROR;
}

static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	while (s->head) {
		struct write_buffer * tmp = s->head;
		for (;;) {
			int sz = write(s->fd, tmp->ptr, tmp->sz);
			if (sz < 0) {
				switch(errno) {
				case EINTR:
					continue;
				case EAGAIN:
					return -1;
				}
				force_close(ss,s, result);
				return SOCKET_CLOSE;
			}
			s->wb_size -= sz;
			if (sz != tmp->sz) {
				tmp->ptr += sz;
				tmp->sz -= sz;
				return -1;
			}
			break;
		}
		s->head = tmp->next;
		FREE(tmp->buffer);
		FREE(tmp);
	}
	s->tail = NULL;
	sp_write(ss->event_fd, s->fd, s, false); // 取消关注

	// 这里 在关闭的时候 处理不够完善 因为第二次发送的时候直接关闭了socket 没有继续发送完毕 所以skynet不适合大流量的网络应用
	if (s->type == SOCKET_TYPE_HALFCLOSE) {
		force_close(ss, s, result);
		return SOCKET_CLOSE;
	}

	return -1;
}

static void
append_sendbuffer(struct socket *s, struct request_send * request, int n) {
	struct write_buffer * buf = MALLOC(sizeof(*buf));
	buf->ptr = request->buffer+n;
	buf->sz = request->sz - n;
	buf->buffer = request->buffer;
	buf->next = NULL;
	s->wb_size += buf->sz;
	if (s->head == NULL) {
		s->head = s->tail = buf;
	} else {
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);
		s->tail->next = buf;
		s->tail = buf;
	}
}

static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[id % MAX_SOCKET];
	if (s->type == SOCKET_TYPE_INVALID || s->id != id 
		|| s->type == SOCKET_TYPE_HALFCLOSE
		|| s->type == SOCKET_TYPE_PACCEPT) {
		FREE(request->buffer);
		return -1;
	}
	assert(s->type != SOCKET_TYPE_PLISTEN && s->type != SOCKET_TYPE_LISTEN);
	// 应用层缓冲区 没有数据直接发送
	if (s->head == NULL) {
		int n = write(s->fd, request->buffer, request->sz);
		if (n<0) {
			switch(errno) {
			case EINTR:
			case EAGAIN: // 内核缓冲区满了
				n = 0;
				break;
			default:
				fprintf(stderr, "socket-server: write to %d (fd=%d) error.",id,s->fd);
				force_close(ss,s,result);
				return SOCKET_CLOSE;
			}
		}
		// 可以把数据拷贝到内核缓冲区中
		if (n == request->sz) {
			FREE(request->buffer);
			return -1;
		}
		// 将未发送的部分添加到应用层缓冲区中 关注可写事件
		append_sendbuffer(s, request, n);
		sp_write(ss->event_fd, s->fd, s, true);
	} else { // 将未发送的数据添加到应用层缓冲区 应用层缓冲区已经有数据了
		append_sendbuffer(s, request, 0);
	}
	return -1;
}

static int
listen_socket(struct socket_server *ss, struct request_listen * request, struct socket_message *result) {
	int id = request->id;
	int listen_fd = request->fd;
	struct socket *s = new_fd(ss, id, listen_fd, request->opaque, false); // 没有加入epoll
	if (s == NULL) {
		goto _failed;
	}
	s->type = SOCKET_TYPE_PLISTEN;
	return -1;
_failed:
	close(listen_fd);
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	ss->slot[id % MAX_SOCKET].type = SOCKET_TYPE_INVALID;

	return SOCKET_ERROR;
}

static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[id % MAX_SOCKET];
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {
		result->id = id;
		result->opaque = request->opaque;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_CLOSE;
	}
	if (s->head) { 
		int type = send_buffer(ss,s,result); // 关闭socket的时候 如果有未发送的数据 先发送出去
		if (type != -1)
			return type;
	}
	if (s->head == NULL) {
		force_close(ss,s,result);
		result->id = id;
		result->opaque = request->opaque;
		return SOCKET_CLOSE;
	}
	s->type = SOCKET_TYPE_HALFCLOSE;

	return -1;
}

static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	struct socket *s = new_fd(ss, id, request->fd, request->opaque, true);
	if (s == NULL) {
		result->data = NULL;
		return SOCKET_ERROR;
	}
	sp_nonblocking(request->fd);
	s->type = SOCKET_TYPE_BIND;
	result->data = "binding";
	return SOCKET_OPEN;
}

static int
start_socket(struct socket_server *ss, struct request_start *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	result->data = NULL;
	struct socket *s = &ss->slot[id % MAX_SOCKET];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return SOCKET_ERROR;
	}
	if (s->type == SOCKET_TYPE_PACCEPT || s->type == SOCKET_TYPE_PLISTEN) {
		if (sp_add(ss->event_fd, s->fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return SOCKET_ERROR;
		}
		s->type = (s->type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN;
		s->opaque = request->opaque;
		result->data = "start";
		return SOCKET_OPEN;
	} else if (s->type == SOCKET_TYPE_CONNECTED) {
		s->opaque = request->opaque;
		result->data = "transfer";
		return SOCKET_OPEN;
	}
	return -1;
}

static void
block_readpipe(int pipefd, void *buffer, int sz) {
	for (;;) {
		int n = read(pipefd, buffer, sz);
		if (n<0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "socket-server : read pipe error %s.",strerror(errno));
			return;
		}
		// must atomic read from a pipe
		assert(n == sz);
		return;
	}
}

// 判断管道是否有命名 使用select来管理 没有使用epoll时为了提高命令的检测频率
static int
has_cmd(struct socket_server *ss) {
	struct timeval tv = {0,0};
	int retval;

	FD_SET(ss->recvctrl_fd, &ss->rfds);

	retval = select(ss->recvctrl_fd+1, &ss->rfds, NULL, NULL, &tv);
	if (retval == 1) {
		return 1;
	}
	return 0;
}

// return type
// 有命令 把命令解析出来 根据相应的类型调用相应的函数

static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {
	int fd = ss->recvctrl_fd;
	// the length of message is one byte, so 256+8 buffer size is enough.
	uint8_t buffer[256];
	uint8_t header[2];
	block_readpipe(fd, header, sizeof(header));
	int type = header[0];
	int len = header[1];
	block_readpipe(fd, buffer, len);
	// ctrl command only exist in local fd, so don't worry about endian.
	switch (type) {
	case 'S':
		return start_socket(ss,(struct request_start *)buffer, result);
	case 'B':
		return bind_socket(ss,(struct request_bind *)buffer, result);
	case 'L':
		return listen_socket(ss,(struct request_listen *)buffer, result);
	case 'K':
		return close_socket(ss,(struct request_close *)buffer, result);
	case 'O':
		return open_socket(ss, (struct request_open *)buffer, result, false);
	case 'X':
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_EXIT;
	case 'D':
		return send_socket(ss, (struct request_send *)buffer, result);
	default:
		fprintf(stderr, "socket-server: Unknown ctrl %c.\n",type);
		return -1;
	};

	return -1;
}

// return -1 (ignore) when error
static int
forward_message(struct socket_server *ss, struct socket *s, struct socket_message * result) {
	int sz = s->size;
	char * buffer = MALLOC(sz);
	int n = (int)read(s->fd, buffer, sz);
	if (n<0) {
		FREE(buffer);
		switch(errno) {
		case EINTR:
			break;
		case EAGAIN:
			fprintf(stderr, "socket-server: EAGAIN capture.\n");
			break;
		default:
			// close when error
			force_close(ss, s, result);
			return SOCKET_ERROR;
		}
		return -1;
	}
	if (n==0) { // peer close
		FREE(buffer);
		force_close(ss, s, result);
		return SOCKET_CLOSE;
	}

	// half close 半关闭丢掉消息
	if (s->type == SOCKET_TYPE_HALFCLOSE) {
		// discard recv data
		FREE(buffer);
		return -1;
	}

	if (n == sz) {
		s->size *= 2;
	} else if (sz > MIN_READ_BUFFER && n*2 < sz) {
		s->size /= 2;
	}

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = buffer;
	return SOCKET_DATA;
}

// 尝试连接中的套接字可写事件 可能失败
static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	int error;
	socklen_t len = sizeof(error);  
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  
	if (code < 0 || error) {  
		force_close(ss,s, result);
		return SOCKET_ERROR;
	} else {
		s->type = SOCKET_TYPE_CONNECTED;
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		sp_write(ss->event_fd, s->fd, s, false); //  取消关注可写事件
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		if (getpeername(s->fd, &u.s, &slen) == 0) {
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
				result->data = ss->buffer;
				return SOCKET_OPEN;
			}
		}
		result->data = NULL;
		return SOCKET_OPEN;
	}
}

// return 0 when failed
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	union sockaddr_all u;
	socklen_t len = sizeof(u);
	int client_fd = accept(s->fd, &u.s, &len);
	if (client_fd < 0) {
		return 0;
	}
	int id = reserve_id(ss);
	if (id < 0) {
		close(client_fd);
		return 0;
	}
	socket_keepalive(client_fd);
	sp_nonblocking(client_fd);
	struct socket *ns = new_fd(ss, id, client_fd, s->opaque, false);
	if (ns == NULL) {
		close(client_fd);
		return 0;
	}
	ns->type = SOCKET_TYPE_PACCEPT;
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = id;
	result->data = NULL;

	// 将对等方的ip port保存下来
	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
		result->data = ss->buffer;
	}

	return 1;
}


// return type
// 有命令的话 优先检测命令// 没有命令的时候
int 
socket_server_poll(struct socket_server *ss, struct socket_message * result, int * more) {
	for (;;) {
		if (ss->checkctrl) {
			if (has_cmd(ss)) { // has_cmd内部调用select函数 判断管道是否有命令  使用select来管理 没有使用epoll时为了提高命令的检测频率
				int type = ctrl_cmd(ss, result); // 处理命令
				if (type != -1)
					return type;
				else
					continue;
			}
			else {
				ss->checkctrl = 0;
			}
		}
		if (ss->event_index == ss->event_n) { // 当前的处理序号最大了 即处理完了 继续等待事件的到来
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
			ss->checkctrl = 1;
			if (more) {
				*more = 0;
			}
			ss->event_index = 0;
			if (ss->event_n <= 0) {
				ss->event_n = 0;
				return -1;
			}
		}
		struct event *e = &ss->ev[ss->event_index++];
		struct socket *s = e->s;
		if (s == NULL) {
			// dispatch pipe message at beginning
			continue;
		}
		switch (s->type) {
		case SOCKET_TYPE_CONNECTING: // connecting
			return report_connect(ss, s, result);
		case SOCKET_TYPE_LISTEN:
			if (report_accept(ss, s, result)) {
				return SOCKET_ACCEPT;
			} 
			break;
		case SOCKET_TYPE_INVALID:
			fprintf(stderr, "socket-server: invalid socket\n");
			break;
		default:
			if (e->write) { // 可写事件 从应用层读取数据
				int type = send_buffer(ss, s, result);
				if (type == -1)
					break;
				return type;
			}
			if (e->read) { // 可读事件 读取消息
				int type = forward_message(ss, s, result);
				if (type == -1)
					break;
				return type;
			}
			break;
		}
	}
}

// 向管道的写端写入数据
static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	request->header[6] = (uint8_t)type;
	request->header[7] = (uint8_t)len;
	for (;;) {
		int n = write(ss->sendctrl_fd, &request->header[6], len+2); // 向管道的写端写入数据
		if (n<0) {
			if (errno != EINTR) {
				fprintf(stderr, "socket-server : send ctrl command error %s.\n", strerror(errno));
			}
			continue;
		}
		assert(n == len+2);
		return;
	}
}

static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
	int len = strlen(addr);
	if (len + sizeof(req->u.open) > 256) {
		fprintf(stderr, "socket-server : Invalid addr %s.\n",addr);
		return 0;
	}
	int id = reserve_id(ss);
	req->u.open.opaque = opaque;
	req->u.open.id = id;
	req->u.open.port = port;
	memcpy(req->u.open.host, addr, len);
	req->u.open.host[len] = '\0';

	return len;
}

// 非阻塞的连接
int 
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;
	int len = open_request(ss, &request, opaque, addr, port);
	send_request(ss, &request, 'O', sizeof(request.u.open) + len);
	return request.u.open.id;
}

// 直接调用open_socket 阻塞连接服务器 这里阻塞连接服务器后 还是交给epoll来管理读写 非阻塞的读写
int 
socket_server_block_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;
	struct socket_message result;
	open_request(ss, &request, opaque, addr, port);
	int ret = open_socket(ss, &request.u.open, &result, true);
	if (ret == SOCKET_OPEN) {
		return result.id;
	} else {
		return -1;
	}
}

// return -1 when error
int64_t 
socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[id % MAX_SOCKET];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		return -1;
	}
	assert(s->type != SOCKET_TYPE_RESERVE);

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	send_request(ss, &request, 'D', sizeof(request.u.send));
	return s->wb_size;
}

void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}

void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

// 创建套接字绑定监听
static int
do_listen(const char * host, int port, int backlog) {
	// only support ipv4
	// todo: support ipv6 by getaddrinfo
	uint32_t addr = INADDR_ANY;
	if (host[0]) {
		addr=inet_addr(host);
	}
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		return -1;
	}
	int reuse = 1;
	// 地址重复利用
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) {
		goto _failed;
	}

	struct sockaddr_in my_addr;
	memset(&my_addr, 0, sizeof(struct sockaddr_in));
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(port);
	my_addr.sin_addr.s_addr = addr;
	if (bind(listen_fd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1) {
		goto _failed;
	}
	if (listen(listen_fd, backlog) == -1) {
		goto _failed;
	}
	return listen_fd;
_failed:
	close(listen_fd);
	return -1;
}

int 
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {
	int fd = do_listen(addr, port, backlog);
	if (fd < 0) {
		return -1;
	}
	struct request_package request;
	int id = reserve_id(ss); // 从应用层分配一个id 从socket池中
	request.u.listen.opaque = opaque;
	request.u.listen.id = id;
	request.u.listen.fd = fd;
	send_request(ss, &request, 'L', sizeof(request.u.listen));
	return id;
}

int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	struct request_package request;
	int id = reserve_id(ss);
	request.u.bind.opaque = opaque;
	request.u.bind.id = id;
	request.u.bind.fd = fd;
	send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}

void 
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.start.id = id;
	request.u.start.opaque = opaque;
	send_request(ss, &request, 'S', sizeof(request.u.start));
}
