﻿#ifndef SKYNET_H
#define SKYNET_H

#include <stddef.h>
#include <stdint.h>

#define PTYPE_TEXT 0    		// 文本协议
#define PTYPE_RESPONSE 1        // response to client with session, session may be packed into package
#define PTYPE_MULTICAST 2
#define PTYPE_CLIENT 3          // 客户端消息
#define PTYPE_SYSTEM 4 			// 协议控制命令
#define PTYPE_HARBOR 5 			// harbor harbor type 即远程消息
#define PTYPE_SOCKET 6 			// 本地的socket消息

// read lualib/skynet.lua lualib/simplemonitor.lua
#define PTYPE_RESERVED_ERROR 7	 // ERRO tell client the session is broken

// read lualib/skynet.lua lualib/mqueue.lua
#define PTYPE_RESERVED_QUEUE 8
#define PTYPE_RESERVED_DEBUG 9
#define PTYPE_RESERVED_LUA 10

#define PTYPE_TAG_DONTCOPY 0x10000
#define PTYPE_TAG_ALLOCSESSION 0x20000

struct skynet_context;

void skynet_error(struct skynet_context * context, const char *msg, ...);

const char * skynet_command(struct skynet_context * context, const char * cmd , const char * parm);

uint32_t skynet_queryname(struct skynet_context * context, const char * name);

int skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * msg, size_t sz);

int skynet_sendname(struct skynet_context * context, const char * destination , int type, int session, void * msg, size_t sz);

void skynet_forward(struct skynet_context *, uint32_t destination);

int skynet_isremote(struct skynet_context *, uint32_t handle, int * harbor);

typedef int (*skynet_cb)(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz);

void skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb);

#endif
