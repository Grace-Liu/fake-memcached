#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <unistd.h>

#include <string>
#include <sstream>

#include "log.h"
#include "simucached.h"
#include "thread.h"
#include "work.h"

#define MAX_EVENTS 2048

__thread char devnull[READ_CHUNK];

void* thread_main(void* data) {
	  Thread *td = (Thread *) data;
	  struct thread_context *ctx = td->ctx;
	  mctx_t mctx = ctx->mctx;
	  int efd = ctx->efd;
	  int nevents;
	  int i, ret;
	  int do_accept;
		

	  // We use an IOV for GET replies so we can cheaply insert the
	  // requested "key" without having to manipulate a full reply buffer.
	  //
	  // iov[0] = "VALUE "
	  // iov[1] = key
	  // iov[2] = " 0 <value length>\r\n<value>\r\nEND\r\n"

	  struct iovec iovs[3];

	  iovs[0].iov_base = (char*) "VALUE ";
	  iovs[0].iov_len = strlen((char*) iovs[0].iov_base);
	  iovs[1].iov_base = (char*) "key";
	  iovs[1].iov_len = strlen((char*) iovs[1].iov_base);

	  std::stringstream tailstream;
	  tailstream << " 0 " << args.value_size_arg << "\r\n";
	  tailstream << std::string().append(args.value_size_arg, 'f');
	  tailstream << "\r\nEND\r\n";
	  std::string tail = tailstream.str();

	  iovs[2].iov_base = (char*) tail.c_str();
	  iovs[2].iov_len = strlen(tail.c_str());

	 //  struct epoll_event events[MAX_EVENTS];
	 struct mtcp_epoll_event events[MAX_EVENTS];

	  //int next_thread = 0;
	  int listener= open_listen_socket(ctx, args.port_arg);

	  while (!done[core]) { //???
	       nevents = mtcp_epoll_wait(ctx->mctx, efd, events, MAX_EVENTS, -1);
	   	if (nevents < 0) {
				if (errno != EINTR)
					perror("mtcp_epoll_wait");
				break;
	       }

		do_accept = FALSE;
		
	    	for (i = 0; i < nevents; i++) {
			if (events[i].data.sockid == listener) {
					/* if the event is for the listener, accept connection */
					do_accept = TRUE;
			} else  {
			      Connection *conn = (Connection *) events[i].data.ptr;
			      int fd = conn->fd;

			      // ***
			      // Zero-effort protocol parser.  Assumes every command ends with
			      // a \r\n and replies with a generic GET reply.
			      // ***

			      if (args.no_parse_given) {
				 int ret = mtcp_read(ctx->mctx, fd, conn->buffer, sizeof(conn->buffer));
			        if (ret <= 0) {
			          if (ret == EAGAIN) W("read() returned EAGAIN");
					   mtcp_close(ctx->mctx, fd);
				          delete conn;
				          continue;
			        }

			        conn->buffer_idx = ret;
			        conn->buffer[conn->buffer_idx] = '\0';

			        char *start = conn->buffer;

			        // Locate a \r\n
			        char *crlf = NULL;
			        while (start < &conn->buffer[conn->buffer_idx]) {
			          crlf = strstr(start, "\r\n");

			          if (crlf == NULL) break; // No \r\n found.

			          int length = crlf - start;

			         if (mtcp_writev(ctx->mctx, fd, iovs, 3) == EAGAIN) W("writev() returned EAGAIN");
			          start += length + 2;
			        }

			        continue;
			      }

			      // ***
			      // Minimal-effort protocol parser.
			      //
			      // Searches for a \r\n in the input buffer and then attempts to
			      // parse the given command.  Incomplete commands (i.e. if no
			      // \r\n can be found) will be kept at the head of the
			      // connection's buffer.  If the buffer becomes full (READ_CHUNK
			      // bytes), it is discarded; thus, commands will be clipped if
			      // they are bigger than a READ_CHUNK.
			      //
			      // After finding a \r\n, this parser understands GET and SET
			      // commands.  Following a GET command, it replies with a fake
			      // VALUE for the given key.  Following a SET command, the
			      // connection switches to "GOBBLE" state where it swallows X
			      // bytes from the socket (where X was the size specified in the
			      // SET command).  After it finishes gobbling these bytes, it
			      // switches back to normal mode.
			      // ***

			      if (conn->state == Connection::GOBBLE) {
				     int ret = mtcp_read(ctx->mctx, fd, devnull,
			                       	conn->bytes_to_eat > READ_CHUNK ?
			                       	READ_CHUNK : conn->bytes_to_eat);
			        if (ret <= 0) {
			          mtcp_close(ctx->mctx, fd);
			          delete conn;
			          continue;
			        }

			        conn->bytes_to_eat -= ret;
			        if (conn->bytes_to_eat <= 0) {
			          conn->state = Connection::IDLE;
					mtcp_write(ctx->mctx, fd, "STORED\r\n", 8);
			        }
			      } else {
			        // Read into the connection's input buffer.  buffer_idx points
			        // to the current tail of the buffer.
				 int ret = mtcp_read(ctx->mctx, fd, &conn->buffer[conn->buffer_idx],
			                       sizeof(conn->buffer) - conn->buffer_idx - 1);	
			        if (ret <= 0) { // EOF or error.
			          mtcp_close(ctx->mctx, fd);
			          delete conn;
			          continue;
			        }

			        conn->buffer_idx += ret;
			        // NUL-terminate to protect string operations below.
			        conn->buffer[conn->buffer_idx] = '\0';

			        char *start = conn->buffer;

			        // Search for \r\n (end of command).
			        char *crlf = NULL;
			        while (start < &conn->buffer[conn->buffer_idx]) {
			          crlf = strstr(start, "\r\n");
			          if (crlf == NULL) break; // No \r\n found, we're finished.

			          int length = crlf - start;
			          start[length] = '\0'; // Mark the end of a command.

			          if (!strncasecmp(start, "get", 3)) {
			            // Slice the key out of the command.
			            // FIXME: This won't parse "GET  foo" correctly.
			            char *key = strchr(start, ' ');
			            if (key != NULL && *++key != '\0') {
			              char *end = strchr(key, ' ');
			              if (end != NULL) *end = '\0'; // Only take 1 key.

			              iovs[1].iov_base = key;
			              iovs[1].iov_len = strlen(key);
			              work();
					mtcp_writev(ctx->mctx, fd, iovs, 3);
						  
			            } else {
			              W("Failed to parse GET command: %s", start);
			              mtcp_write(ctx->mctx, fd, "ERROR\r\n", 7);
			            }
			 
			            start += length + 2;
			          } else if (!strncasecmp(start, "set", 3)) {
			            int setsize = -1;
			            if (sscanf(start, "%*s %*s %*d %*d %d", &setsize) &&
			                setsize >= 0) {
			              start += length + 2;
			              int remaining = &conn->buffer[conn->buffer_idx] - start;

			              // Case 1: All of the SET data is in the buffer. Eat it
			              // immediately.  Case 2: We don't have enough data in
			              // the buffer to complete the SET. Switch to GOBBLE
			              // state and start eating bytes.
			              if (setsize + 2 <= remaining) {
			                start += setsize + 2;
			                mtcp_write(ctx->mctx, fd, "STORED\r\n", 8);
			              } else {
			                conn->state = Connection::GOBBLE;
			                conn->bytes_to_eat = setsize + 2 - remaining;
			                start = &conn->buffer[conn->buffer_idx];
			                break;
			              }
			            } else {
			              W("Failed to parse SET command: %s", start);
			                 mtcp_write(ctx->mctx, fd, "ERROR\r\n", 7);
			              start += length + 2;
			            }
			          } else {
			            D("Unknown command: %s", start);
					mtcp_write(ctx->mctx, fd, "ERROR\r\n", 7);	
			            start += length + 2;
			          }
			        }

			        // Reset buffer_idx if we run out of buffer space or we've
			        // successfully parsed everything in the buffer.
			        if (((start == conn->buffer &&
			              conn->buffer_idx >= sizeof(conn->buffer) - 1)) ||
			            &conn->buffer[conn->buffer_idx] == start) {
			          conn->buffer_idx = 0;
			        } else {
			          // If there is any data left in the buffer (i.e. an
			          // incomplete command), move it to the front.
			          int shift = &conn->buffer[conn->buffer_idx] - start;
			          memmove(conn->buffer, start, shift);
			          conn->buffer_idx = shift;
		       	 }
			    }

			if (do_accept) {
				while (1) {
					ret = AcceptConnection(ctx, listener);
					if (ret < 0)
						break;
				}
			}		  
	  	   }
		  }	
	  	}
		/* destroy mtcp context: this will kill the mtcp thread */
		mtcp_destroy_context(mctx);
		pthread_exit(NULL);

		return NULL;
	  
	  }

	/*----------------------------------------------------------------------------*/
	int 
	AcceptConnection(struct thread_context *ctx, int listener)
	{
		//int next_thread = 0;
		mctx_t mctx = ctx->mctx;
		struct mtcp_epoll_event ev;
		int c;

		c = mtcp_accept(mctx, listener, NULL, NULL);

		if (c >= 0) {
			if (c >= MAX_FLOW_NUM) {
				TRACE_ERROR("Invalid socket id %d.\n", c);
				return -1;
			}


			int optval = 1;
			if (mtcp_setsockopt(ctx->mctx, c, IPPROTO_TCP, TCP_NODELAY,
                   		(void *) &optval, sizeof(optval)))
      			DIE("setsockopt(TCP_NODELAY) failed: %s", strerror(errno));

			TRACE_APP("New connection %d accepted.\n", c);
			Connection* conn = new Connection(c);
			ev.events = MTCP_EPOLLIN;
			ev.data.sockid = c;
			ev.data.ptr = conn;
			mtcp_setsock_nonblock(ctx->mctx, c);

			mtcp_epoll_ctl(mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, c, &ev); //???
			TRACE_APP("Socket %d registered.\n", c);

		} else {
			if (errno != EAGAIN) {
				TRACE_ERROR("mtcp_accept() error %s\n", 
						strerror(errno));
			}
		}
		//next_thread = (next_thread + 1) % args.threads_arg;

		return c;
	}
	/*----------------------------------------------------------------------------*/




	  while (1) {
	    sa_len = sizeof(sa);
	    int newfd = mtcp_accept(ctx->mctx, listen_socket, &sa, &sa_len);
	    if (newfd < 0) DIE("accept() failed: %s", strerror(errno));

	    int optval = 1;
	     if (mtcp_setsockopt(ctx->mctx, newfd, IPPROTO_TCP, TCP_NODELAY,
	                   (void *) &optval, sizeof(optval)))               
	      DIE("setsockopt(TCP_NODELAY) failed: %s", strerror(errno));

	    set_nonblocking(newfd);

	    Connection* conn = new Connection(newfd);

	    //struct epoll_event ev;
	    struct mtcp_epoll_event ev;
	    //ev.events = EPOLLIN;
	    ev.events = MTCP_EPOLLIN;
	    ev.data.ptr = conn;//???

	    if (mtcp_epoll_ctl(ctx->mctx, td[next_thread].efd, EPOLL_CTL_ADD, newfd, &ev)) //??
	      DIE("epoll_ctl(%d, EPOLL_CTRL_ADD, %d) failed: %s",
	          td[next_thread].efd, newfd, strerror(errno));

	    next_thread = (next_thread + 1) % args.threads_arg;
  }
