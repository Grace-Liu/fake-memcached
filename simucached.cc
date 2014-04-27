#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
//#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "cmdline.h"
#include "log.h"
#include "simucached.h"
#include "thread.h"
#include "work.h"

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#include "debug.h"

#define MAX_FLOW_NUM  (10000)
#define RCVBUF_SIZE (2*1024)
#define SNDBUF_SIZE (8*1024)
#define MAX_EVENTS (MAX_FLOW_NUM * 3)

#define MAX_CPUS 12
#define HT_SUPPORT FALSE

/*----------------------------------------------------------------------------*/
struct thread_context
{
	mctx_t mctx;
	int efd;
};
/*----------------------------------------------------------------------------*/
static int num_cores;
static int core_limit;
//static pthread_t app_thread[MAX_CPUS];
Thread td[MAX_CPUS];
static int done[MAX_CPUS];
static int finished;
/*----------------------------------------------------------------------------*/


/* simucached

   Model A
   One listener, one epoll set per child.

     The main thread responsibilities:
     * Create epoll set for each child.
     * Spawn children.
     * Open socket for listening.
     * Add new connections to children epoll sets, round-robin.

     Child thread responsibilities:
     * Wait for events from epoll set.
     * Close hung-up connections, housekeep.
     * Read from fds, parse commands, generate responses.

   Model 2
   One listener, one epoll set total.

   Model 3
   One listener, one epoll set per child, fds assigned to random pair of sets.
  
 */

using namespace std;

gengetopt_args_info args;

static int open_listen_socket(struct thread_context *ctx, int port) {
  /*struct sockaddr_in sa;
  int optval = 1;

  int fd = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0);
  if (fd < 0) DIE("socket() failed: %s", strerror(errno));

  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) //???
    DIE("setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));

  bzero(&sa, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = INADDR_ANY;
  sa.sin_port = htons(port);

  if (mtcp_bind(ctx->mctx, fd, (struct sockaddr *) &sa, sizeof(sa)) < 0)
    DIE("bind(port=%d) failed: %s", port, strerror(errno));

  if (mtcp_listen(ctx->mctx, fd, 1024) < 0)
    DIE("listen() failed: %s", strerror(errno));

  return fd;*/

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

	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port = htons(port);
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

static void set_nonblocking(struct thread_context *ctx, int fd) {
    int ret; 
    ret = mtcp_setsock_nonblock(ctx->mctx, fd);
	if (ret < 0) {
		TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
		return -1;
	}
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
void
SignalHandler(int signum)
{
	int i;

	for (i = 0; i < core_limit; i++) {
		if (td[i]->pt == pthread_self()) {
			//TRACE_INFO("Server thread %d got SIGINT\n", i);
			done[i] = TRUE;
		} else {
			if (!done[i]) {
				pthread_kill(td[i]->pt, signum);
			}
		}
	}
}
/*----------------------------------------------------------------------------*/

int main(int argc, char **argv) {
	  int fd;
	  int ret;
	  int cores[MAX_CPUS];
	  int i;

	  num_cores = GetNumCPUs();
	  core_limit = args.threads_arg;
	  if (core_limit > num_cores) {
		TRACE_CONFIG("CPU limit should be smaller than the "
							"number of CPUS: %d\n", num_cores);
		return FALSE;
	  }

	  if (cmdline_parser(argc, argv, &args) != 0) DIE("cmdline_parser failed");

	  for (unsigned int i = 0; i < args.verbose_given; i++)
	    log_level = (log_level_t) ((int) log_level - 1);
	  if (args.quiet_given) log_level = QUIET;

	  if (args.work_given && !args.calibration_arg) {
	    I("Calibrating busy-work loop.");
	    args.calibration_arg = work_per_sec(10000000);
	    I("calibration = %d", args.calibration_arg);
	  }

	 // signal(SIGPIPE, SIG_IGN); //??

	   ret = mtcp_init("epserver.conf");
	   if (ret) {
		TRACE_ERROR("Failed to initialize mtcp\n");
		exit(EXIT_FAILURE);
	   }
	   mtcp_register_signal(SIGINT, SignalHandler);
	   TRACE_INFO("Application initialization finished.\n");

	  V("%s v%s ready to roll",
	    CMDLINE_PARSER_PACKAGE_NAME, CMDLINE_PARSER_VERSION);

	  //Thread td[args.threads_arg]; 
	  for (int i = 0; i < core_limit; i++) {
		  cores[i] = i;
		  done[i] = FALSE;
	         td[i]->ctx = InitializeServerThread(core[i]);
		  //spawn_thread(&td[i]);
		  if (pthread_create(&td[i]->pt, NULL, thread_main, td))
   			 DIE("pthread_create() failed: %s", strerror(errno));
	  }	
	    
	   for (i = 0; i < core_limit; i++) {
		pthread_join(td[i]->pt, NULL);
	   }

	   mtcp_destroy();
	   return 0;
  
}
