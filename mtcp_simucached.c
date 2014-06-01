#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#include "debug.h"
/*----------------------------------------------------------------------------*/
#include <stdint.h>
#define USEC_PER_SEC 1000000
#define READ_CHUNK 16384
/*----------------------------------------------------------------------------*/
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#define MAX_FLOW_NUM  (10000)

#define RCVBUF_SIZE (2*1024)
#define SNDBUF_SIZE (8*1024)

#define MAX_EVENTS (MAX_FLOW_NUM * 3)

#define HTTP_HEADER_LEN 1024
#define URL_LEN 128

#define MAX_CPUS 16
#define MAX_FILES 30

#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#define HT_SUPPORT FALSE

/*----------------------------------------------------------------------------*/
typedef struct conn conn;
struct conn
{
	 int fd;
	 //enum conn_states state;
 	 int bytes_to_eat;
  	 char buffer[READ_CHUNK+1];
 	 int buffer_idx;

	   /* data for the mwrite state */
    	  struct iovec *iov;
    	  int    iovsize;   /* number of elements allocated in iov[] */
    	  int    iovused;   /* number of elements used in iov[] */
};

enum conn_states{
    IDLE = 0,
    GOBBLE,
    LAST_STATE
  };

/*----------------------------------------------------------------------------*/
struct thread_context
{
	mctx_t mctx;
	int ep;
};
/*----------------------------------------------------------------------------*/
static int num_cores;
static int core_limit;
static int value_size;
static pthread_t app_thread[MAX_CPUS];
static int done[MAX_CPUS];

/*----------------------------------------------------------------------------*/
void 
CloseConnection(struct thread_context *ctx, int sockid)
{
	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_DEL, sockid, NULL);
	mtcp_close(ctx->mctx, sockid);
}


/*----------------------------------------------------------------------------*/
conn *conn_init(int fd) {
	conn *c = malloc( sizeof(conn *));
	if(c==NULL) {
		printf("Error on create conn \n");	
	}
	c->fd = fd;
	//c->state = IDLE;
	c->bytes_to_eat = 0;
	c->buffer_idx = 0;
	
    return c;
}
/*----------------------------------------------------------------------------*/
int 
AcceptConnection(struct thread_context *ctx, int listener)
{
	//printf("test3:%ld\n", gettid());
	mctx_t mctx = ctx->mctx;
	struct mtcp_epoll_event ev;
	int c;
	conn *newconn;

	c = mtcp_accept(mctx, listener, NULL, NULL);

	if (c >= 0) {
		if (c >= MAX_FLOW_NUM) {
			TRACE_ERROR("Invalid socket id %d.\n", c);
			return -1;
		}
		TRACE_APP("New connection %d accepted.\n", c);

		//printf("New connection %d accepted.\n", c);
		newconn = conn_init(c);
		ev.data.ptr = newconn;
		ev.events = MTCP_EPOLLIN;
		ev.data.sockid = c;
		mtcp_setsock_nonblock(ctx->mctx, c);
		mtcp_epoll_ctl(mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, c, &ev);
		TRACE_APP("Socket %d registered.\n", c);

	} else {
		if (errno != EAGAIN) {
			TRACE_ERROR("mtcp_accept() error %s\n", 
					strerror(errno));
		}
	}

	return c;
}
/*----------------------------------------------------------------------------*/
struct thread_context *
InitializeServerThread(int core)
{
	struct thread_context *ctx;

	/* affinitize application thread to a CPU core */
#if HT_SUPPORT
	mtcp_core_affinitize(core + (num_cores / 2));
#else
	mtcp_core_affinitize(core);
#endif /* HT_SUPPORT */

	ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));
	if (!ctx) {
		TRACE_ERROR("Failed to create thread context!\n");
		return NULL;
	}

	/* create mtcp context: this will spawn an mtcp thread */
	ctx->mctx = mtcp_create_context(core);
	if (!ctx->mctx) {
		TRACE_ERROR("Failed to create mtcp context!\n");
		return NULL;
	}

	/* create epoll descriptor */
	ctx->ep = mtcp_epoll_create(ctx->mctx, MAX_EVENTS);
	if (ctx->ep < 0) {
		TRACE_ERROR("Failed to create epoll descriptor!\n");
		return NULL;
	}

	return ctx;
}
/*----------------------------------------------------------------------------*/
int 
CreateListeningSocket(struct thread_context *ctx)
{
	int listener;
	struct mtcp_epoll_event ev;
	struct sockaddr_in saddr;
	int ret;

	/* create socket and set it as nonblocking */
	listener = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		TRACE_ERROR("Failed to create listening socket!\n");
		return -1;
	}
	ret = mtcp_setsock_nonblock(ctx->mctx, listener);
	if (ret < 0) {
		TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
		return -1;
	}

	/* bind to port 80 */
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port = htons(11211);
	ret = mtcp_bind(ctx->mctx, listener, 
			(struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
	if (ret < 0) {
		TRACE_ERROR("Failed to bind to the listening socket!\n");
		return -1;
	}

	/* listen (backlog: 4K) */
	ret = mtcp_listen(ctx->mctx, listener, 4096);
	if (ret < 0) {
		TRACE_ERROR("mtcp_listen() failed!\n");
		return -1;
	}
	
	/* wait for incoming accept events */
	ev.events = MTCP_EPOLLIN;
	ev.data.sockid = listener;
	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, listener, &ev);

	return listener;
}
/*----------------------------------------------------------------------------*/
void *
RunServerThread(void *arg)
{
//	printf("Runserver %d\n",gettid());
	
	int core = *(int *)arg;
	struct thread_context *ctx;
	mctx_t mctx;
	int listener;
	int ep;
	struct mtcp_epoll_event *events;
	int nevents;
	int i, ret;
	int do_accept;

	int fd=-1;
	struct iovec iovs[3];
	int len;
	char s[value_size+10];
	conn *c;

	iovs[0].iov_base = (char*) "VALUE ";
  	iovs[0].iov_len = strlen((char*) iovs[0].iov_base);
  	iovs[1].iov_base = (char*) "key";
 	iovs[1].iov_len = strlen((char*) iovs[1].iov_base);

	sprintf(s, "0%d\r\n\0", value_size);
	len = strlen(s);
	for(i=0; i< value_size; i++)
       	 s[len+i] = 'f';
	s[i] = '\0';
	strcat(s, "\r\nEND\r\n");

	iovs[2].iov_base = (char*) s;
 	iovs[2].iov_len = strlen(s);
  
        char send [100];

	sprintf(send, "VALUE key 0 5\r\naaaaa\r\nEND\r\n");	
	/* initialization */
	ctx = InitializeServerThread(core);
	if (!ctx) {
		TRACE_ERROR("Failed to initialize server thread.\n");
		exit(-1);
	}
	mctx = ctx->mctx;
	ep = ctx->ep;

	events = (struct mtcp_epoll_event *)
			calloc(MAX_EVENTS, sizeof(struct mtcp_epoll_event));
	if (!events) {
		TRACE_ERROR("Failed to create event struct!\n");
		exit(-1);
	}

	listener = CreateListeningSocket(ctx);
	if (listener < 0) {
		TRACE_ERROR("Failed to create listening socket.\n");
		exit(-1);
	}

	while (!done[core]) {
		nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
		if (nevents < 1) {
			if (errno != EINTR)
				perror("mtcp_epoll_wait");
			break;
		}

		do_accept = FALSE;
		for (i = 0; i < nevents; i++) {
			c = (conn *)events[i].data.ptr;
			fd = c->fd;
			printf("fd=%d\n", fd);
			if (events[i].data.sockid == listener) {
				/* if the event is for the listener, accept connection */
				do_accept = TRUE;
				//printf("run: if condition\n");

			} else if (events[i].events) {
		 		//ret = mtcp_read(mctx, fd, c->buffer, sizeof(c->buffer));
		 		ret = mtcp_read(mctx, events[i].data.sockid, c->buffer, sizeof(c->buffer));
				printf("run: ret=%d\n", ret);
				if (ret <= 0) {
				          if (ret == EAGAIN) printf("read() returned EAGAIN");
				          close(fd);
				          //free(c);
				          continue;
			        }

			        c->buffer_idx = ret;
			        c->buffer[c->buffer_idx] = '\0';

			        char *start = c->buffer;

			        // Locate a \r\n
			        char *crlf = NULL;
			        while (start < &c->buffer[c->buffer_idx]) {
					  //printf("while loop\n");
				          crlf = strstr(start, "\r\n");

				          if (crlf == NULL) break; // No \r\n found.

				          int length = crlf - start;

					  events[i].events = MTCP_EPOLLIN | MTCP_EPOLLOUT;
					  mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, events[i].data.sockid, &events[i]);	
				         // if (mtcp_writev(mctx, events[i].data.sockid, iovs, 3) == EAGAIN) 
				          if (mtcp_write(mctx, events[i].data.sockid, send, strlen(send)) == EAGAIN) 
						  printf("writev() returned EAGAIN\n");
				          start += length + 2;		`
			        }
 			} else {
				assert(0);
		   	}	
		}

		/* if do_accept flag is set, accept connections */
		if (do_accept) {
			while (1) {
				ret = AcceptConnection(ctx, listener);
				if (ret < 0)
					break;
			}
		}

	}

	/* destroy mtcp context: this will kill the mtcp thread */
	mtcp_destroy_context(mctx);
	pthread_exit(NULL);

	return NULL;
}
/*----------------------------------------------------------------------------*/
void
SignalHandler(int signum)
{
	int i;

	for (i = 0; i < core_limit; i++) {
		if (app_thread[i] == pthread_self()) {
			//TRACE_INFO("Server thread %d got SIGINT\n", i);
			done[i] = TRUE;
		} else {
			if (!done[i]) {
				pthread_kill(app_thread[i], signum);
			}
		}
	}
}
/*----------------------------------------------------------------------------*/

int 
main(int argc, char **argv)
{
	int fd;
	int ret;

	int cores[MAX_CPUS];
	int i;

	num_cores = GetNumCPUs();
	core_limit = num_cores;

	if (argc < 2) {
		TRACE_ERROR("$%s enter thread number\n", argv[0]);
		return FALSE;
	}

	for (i = 0; i < argc - 1; i++) {
		if (strcmp(argv[i], "-N") == 0) {
			core_limit = atoi(argv[i + 1]);
			if (core_limit > num_cores) {
				TRACE_CONFIG("CPU limit should be smaller than the "
						"number of CPUS: %d\n", num_cores);
				return FALSE;
			}
		}
		if (strcmp(argv[i], "-V") == 0) {
			value_size = atoi(argv[i + 1]);
			printf("Value_size=%d\n", value_size);
		}
		else 
			value_size = 64;
	}
        //printf("test1: %ld\n", gettid());
	/* initialize mtcp */
	ret = mtcp_init("epserver.conf");
	if (ret) {
		TRACE_ERROR("Failed to initialize mtcp\n");
		exit(EXIT_FAILURE);
	}
	/* register signal handler to mtcp */
	mtcp_register_signal(SIGINT, SignalHandler);

	TRACE_INFO("Application initialization finished.\n");

	for (i = 0; i < core_limit; i++) {
		cores[i] = i;
		done[i] = FALSE;

		if (pthread_create(&app_thread[i], 
					NULL, RunServerThread, (void *)&cores[i])) {
			perror("pthread_create");
			TRACE_ERROR("Failed to create server thread.\n");
			exit(-1);
		}
	}

	for (i = 0; i < core_limit; i++) {
		pthread_join(app_thread[i], NULL);
	}

	mtcp_destroy();
	return 0;
}

