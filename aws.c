#include "aws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <fcntl.h>
#include <sys/sendfile.h>

#include "util.h"
#include "debug.h"
#include "sock_util.h"
#include "w_epoll.h"
#include "http_parser.h"
#include <libaio.h>
#include <sys/eventfd.h>

#include <errno.h>

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

/* HTTP parser */
static http_parser request_parser;

/* request_path buffer */
static char request_path[BUFSIZ];

/* server-client communication states */
enum connection_state {
	STATE_REQUEST_RECEIVED_PARTIALLY,
	STATE_REQUEST_RECEIVED,
	STATE_HEADER_SENT_PARTIALLY = 1,
	STATE_HEADER_SENT,
	STATE_ASYNC_READ_STARTED,
	STATE_ASYNC_READ_FINISHED,
	STATE_CONNECTION_CLOSED
};

/* requested file states based on its path */
enum file_state {
	NO_STATE,
	STATE_FILE_ERROR,
	STATE_FILE_STATIC,
	STATE_FILE_DYNAMIC
};

/* structure acting as a connection handler */
struct connection {
	int sockfd;

	char recv_buffer[BUFSIZ];
	size_t recv_len;

	char send_buffer[BUFSIZ];
	size_t send_len;

	size_t transfered_bytes;

	char file_path[BUFSIZ];
	size_t file_size;
	int fd;

	enum connection_state state;
	enum file_state f_state;

	io_context_t context;
	int event_fd;
	struct iocb *iocb;
	struct iocb **piocb;
	char **aio_buffers;
	size_t *buffers_data_size;
	size_t submitted_aio_reads;
	size_t finished_aio_reads;
	size_t total_aio_reads;
	int current_buffer_idx;
};

/*
 * Callback is invoked by HTTP request parser when parsing request path.
 * Request path is stored in global request_path variable.
 */

static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &request_parser);

    /* used a buffer because buf is constant */
	char buffer[BUFSIZ];

	memcpy(buffer, buf, len);
	char *ptr = strtok(buffer, " ");

	memset(request_path, 0, BUFSIZ);
	sprintf(request_path, ".%s", ptr);

	return 0;
}

/* Use mostly null settings except for on_path callback. */
static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};

/*
 * Initialize connection structure on given socket.
 */

static struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));

	DIE(conn == NULL, "malloc");

	conn->recv_len = 0;
	conn->send_len = 0;
	conn->transfered_bytes = 0;
	conn->sockfd = sockfd;
	conn->f_state = NO_STATE;

	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
	memset(conn->file_path, 0, BUFSIZ);

	return conn;
}

/*
 * Remove connection handler.
 */

static void connection_remove(struct connection *conn)
{
	/* remove socket from epoll */
	int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);

	DIE(rc < 0, "w_epoll_remove_ptr");

	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

/*
 * Handle a new connection request on the server socket.
 */

static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	/* set the socket to non-blocking */
	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

	/* instantiate new connection handler */
	conn = connection_create(sockfd);

	/* add socket to epoll */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

/*
 * Receive (HTTP) request. Don't parse it, just read data in buffer
 * and print it.
 */

static enum connection_state receive_request(struct connection *conn)
{
	ssize_t bytes_recv;
	char abuffer[64];
	int rc;

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	bytes_recv = recv(conn->sockfd,
					  conn->recv_buffer + conn->transfered_bytes,
					  BUFSIZ - conn->transfered_bytes,
					  0);

	if (bytes_recv < 0) {		/* error in communication */
		goto remove_connection;
	}
	if (bytes_recv == 0) {		/* connection closed */
		goto remove_connection;
	}

	conn->transfered_bytes += bytes_recv;

	if (conn->transfered_bytes < 4
	 || strncmp(conn->recv_buffer + conn->transfered_bytes - 4, "\r\n\r\n", 4) != 0) {
		conn->state = STATE_REQUEST_RECEIVED_PARTIALLY;
		return STATE_REQUEST_RECEIVED_PARTIALLY;
	}

	conn->recv_len = conn->transfered_bytes;
	conn->transfered_bytes = 0;

	dprintf("Received message: %s\n", conn->recv_buffer);

	/* init HTTP_REQUEST parser */
	http_parser_init(&request_parser, HTTP_REQUEST);

	http_parser_execute(&request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);

	conn->state = STATE_REQUEST_RECEIVED;
	return STATE_REQUEST_RECEIVED;

remove_connection:
	/* remove current connection */
	connection_remove(conn);
	return STATE_CONNECTION_CLOSED;
}

static void path_check(struct connection *conn)
{
	/* make sure that after .dat comes '\0' */
	char *ptr = strstr(request_path, ".dat");

	if (ptr != NULL)
		ptr[4] = '\0';

	dprintf("Path checked: %s\n", request_path);

	/* try to open the requested file */
	conn->fd = open(request_path, O_RDONLY);

	/* if it cannot open the requested file send error message */
	if (conn->fd == -1) {
		memset(conn->send_buffer, 0, BUFSIZ);
		sprintf(conn->send_buffer, "HTTP/1.0 404 Error open fd!\r\n\r\n");
		conn->f_state = STATE_FILE_ERROR;
		conn->send_len = strlen(conn->send_buffer);
		return;
	}

	struct stat info;

	fstat(conn->fd, &info);
	conn->file_size = info.st_size;
	memset(conn->file_path, 0, BUFSIZ);
	sprintf(conn->file_path, "%s", request_path);

	/* check if it's static or dynamic*/

	size_t static_folder_len = strlen(AWS_ABS_STATIC_FOLDER);
	size_t dynamic_folder_len = strlen(AWS_ABS_DYNAMIC_FOLDER);

	if (strncmp(request_path, AWS_ABS_STATIC_FOLDER, static_folder_len) == 0) {
		memset(conn->send_buffer, 0, BUFSIZ);
		sprintf(conn->send_buffer, "HTTP/1.0 200 Static!\r\n\r\n");
		conn->f_state = STATE_FILE_STATIC;
		conn->send_len = strlen(conn->send_buffer);
		return;
	}

	if (strncmp(request_path, AWS_ABS_DYNAMIC_FOLDER, dynamic_folder_len) == 0) {
		memset(conn->send_buffer, 0, BUFSIZ);
		sprintf(conn->send_buffer, "HTTP/1.0 200 Dynamic!\r\n\r\n");
		conn->f_state = STATE_FILE_DYNAMIC;
		conn->send_len = strlen(conn->send_buffer);
	}
}

/*
 * Handle a client request on a client connection.
 */

static void handle_client_request(struct connection *conn)
{
	int rc;
	enum connection_state ret_state;

	ret_state = receive_request(conn);

	if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_REQUEST_RECEIVED_PARTIALLY)
		return;

	path_check(conn);

	/* add socket to epoll for out events */
	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);//inout
	DIE(rc < 0, "w_epoll_add_ptr_out");
}

/*
 * Send HTTP header as a first response to the client's request.
 */

static enum connection_state send_header(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	bytes_sent = send(conn->sockfd,
					  conn->send_buffer + conn->transfered_bytes,
					  conn->send_len - conn->transfered_bytes,
					  0);

	if (bytes_sent < 0)
		goto remove_connection;

	if (bytes_sent == 0)
		goto remove_connection;

	conn->transfered_bytes += bytes_sent;

	if(conn->transfered_bytes < conn->send_len) {
		conn->state = STATE_HEADER_SENT_PARTIALLY;
		return STATE_HEADER_SENT_PARTIALLY;
	}

	dprintf("Sending message: %s\n", conn->send_buffer);

	conn->state = STATE_HEADER_SENT;

	return STATE_HEADER_SENT;

remove_connection:
	/* remove current connection */
	close(conn->fd);
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Starts the asynchronous read of the requested dynamic file from the disk.
 */

static void start_readfile_async(struct connection *conn)
{
	int rc;

	conn->finished_aio_reads = 0;
	conn->submitted_aio_reads = 0;
	conn->current_buffer_idx = 0;

	/* setup the io context */
	memset(&conn->context, 0, sizeof(io_context_t));
	rc = io_setup(1, &conn->context);
	DIE(rc < 0, "io_setup");

	int bytes_read = 0, bytes_to_read;

	/* setup the non-blocking eventfd */
	conn->event_fd = eventfd(0, EFD_NONBLOCK);

	/* prepare all needed iocb structures and the buffers */
	int async_reads = conn->file_size / BUFSIZ + (conn->file_size % BUFSIZ == 0 ? 0 : 1);

	/* set the total number of async I/O readings that must be made */
	conn->total_aio_reads = async_reads;

	conn->iocb = (struct iocb *) malloc(async_reads * sizeof(struct iocb));
	DIE(conn->iocb == NULL, "iocb alloc");

	conn->piocb = (struct iocb **) malloc(async_reads * sizeof(struct iocb *));
	DIE(conn->piocb == NULL, "piocb alloc");

	conn->aio_buffers = (char **) malloc(async_reads * sizeof(char *));
	DIE(conn->aio_buffers == NULL, "aio buffers alloc");

	conn->buffers_data_size = (size_t *) malloc(async_reads * sizeof(size_t));
	DIE(conn->buffers_data_size == NULL, "buffers_len alloc");

	/* setup each iocb for a reading async I/O operation */
	for (int i = 0; i < async_reads; i++) {
		conn->aio_buffers[i] = (char *) malloc(BUFSIZ * sizeof(char));
		DIE(conn->aio_buffers == NULL, "aio_buffers i alloc");

		memset(conn->aio_buffers[i], 0, BUFSIZ);

		conn->piocb[i] = &conn->iocb[i];

		bytes_to_read = (conn->file_size - bytes_read <= BUFSIZ) ? (conn->file_size - bytes_read) : BUFSIZ;
		conn->buffers_data_size[i] = bytes_to_read;

		/* prepare the iocb structure for async reading and set it to notify the eventfd */
		io_prep_pread(conn->piocb[i], conn->fd, conn->aio_buffers[i], bytes_to_read, bytes_read);
		io_set_eventfd(conn->piocb[i], conn->event_fd);

		bytes_read += bytes_to_read;
	}

	/* remove socket from epoll */
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_conn");

	/* submit all operations */
	rc = io_submit(conn->context, async_reads, conn->piocb);
	DIE(rc < 0, "io_submit");

	/* set the return value as the number of submitted async I/O readings */
	conn->submitted_aio_reads = rc;

	/* add the eventfd to the epoll */
	rc = w_epoll_add_ptr_in(epollfd, conn->event_fd, conn);
	DIE(rc < 0, "w_epoll_add_efd");
}

/*
 * Frees the aio_buffers related memory.
 */
static void free_buffers(struct connection *conn)
{
	for (int i = 0; i < conn->total_aio_reads; i++)
		free(conn->aio_buffers[i]);

	free(conn->aio_buffers);
	free(conn->buffers_data_size);
}

int main(void)
{
	int rc;

	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT,
		DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	dlog(LOG_INFO, "Server waiting for connections on port %d\n",
		AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/*
		 * switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */

		if (rev.data.fd == listenfd) {
			dlog(LOG_DEBUG, "New connection\n");
			if (rev.events & EPOLLIN)
				handle_new_connection();
			continue;
		}

		if (rev.events & EPOLLIN) {
			struct connection *conn = rev.data.ptr;

			if (conn->f_state == NO_STATE) {
				/* socket notified for receiving the request */
				dlog(LOG_DEBUG, "New message\n");
				handle_client_request(conn);
			} else {
				/*
				 * when a notification comes on eventfd
				 * compute the number of finished async I/O operations.
				 */
				struct io_event events[conn->submitted_aio_reads];

				u_int64_t efd_val;

				int rc = read(conn->event_fd, &efd_val, sizeof(efd_val));

				DIE(rc < 0, "read event_fd");

				rc = io_getevents(conn->context, efd_val, efd_val, events, NULL);
				DIE(rc != efd_val, "io_getevents");

				conn->finished_aio_reads += efd_val;

				if (conn->finished_aio_reads == conn->submitted_aio_reads
				 && conn->submitted_aio_reads == conn->total_aio_reads) {
					/*
					 * if all submitted operations are done
					 * and all operations were submitted
					 * it means that the file is completely read.
					 */

					/* remove the eventfd from the epoll */
					rc = w_epoll_remove_ptr(epollfd, conn->event_fd, conn);
					DIE(rc < 0, "w_epoll_remove_efd");

					conn->state = STATE_ASYNC_READ_FINISHED;

					/* add the socket to the epoll to send the file */
					rc = w_epoll_add_ptr_out(epollfd, conn->sockfd, conn);
					DIE(rc < 0, "w_epoll_add_ptr_out");
					continue;
				}

				if (conn->finished_aio_reads == conn->submitted_aio_reads
				 && conn->submitted_aio_reads < conn->total_aio_reads) {
					/*
					 * if not all async I/O operations were submitted
					 * submit again.
					 */
					rc = io_submit(conn->context,
						 conn->total_aio_reads - conn->submitted_aio_reads,
						 conn->piocb + conn->submitted_aio_reads);
					DIE(rc < 0, "io_submit");
					conn->submitted_aio_reads += rc;
				}
			}
			continue;
		}

		if (rev.events & EPOLLOUT) {
			struct connection *conn = rev.data.ptr;

			/* socket notified for sending response */
			dlog(LOG_DEBUG, "Ready to send message\n");
			enum connection_state ret_state;
			int bytes_sent;
			int rc;

			switch (conn->f_state) {

			case STATE_FILE_ERROR:
				/* in case of an error only send the HHTTP header */
				ret_state = send_header(conn);
				if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_HEADER_SENT_PARTIALLY)
					continue;

				close(conn->fd);

				connection_remove(conn);
				break;

			case STATE_FILE_STATIC:
				/*
				 * in case of a static file, send the header
				 * then send the file with zero-copying mechanism
				 */
				switch (conn->state) {

				case STATE_REQUEST_RECEIVED:
					/* or STATE_HEADER_SENT_PARTIALLY */
					ret_state = send_header(conn);

					if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_HEADER_SENT_PARTIALLY)
						continue;

					conn->state = STATE_HEADER_SENT;
					conn->transfered_bytes = 0;
					break;

				case STATE_HEADER_SENT:
					bytes_sent = sendfile(conn->sockfd,
										  conn->fd,
										  NULL,
										  conn->file_size - conn->transfered_bytes);

					DIE(bytes_sent < 0, "sending file zero-copying");

					conn->transfered_bytes += bytes_sent;

					if (conn->file_size - conn->transfered_bytes == 0 && bytes_sent == 0) {
						/* if the whole file was sent, close the connection */
						dprintf("Succesfully sent %ld bytes from file: %s\n",
								 conn->transfered_bytes,
								 conn->file_path);

						close(conn->fd);

						connection_remove(conn);
					}

					break;

				default:
					break;
				}

				break;

			case STATE_FILE_DYNAMIC:
				/*
				 * in case of a dynamic file, send the header
				 * then read the file with async I/O mechanism.
				 */
				switch (conn->state) {

				case STATE_REQUEST_RECEIVED:
					/* or STATE_HEADER_SENT_PARTIALLY */
					ret_state = send_header(conn);

					if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_HEADER_SENT_PARTIALLY)
						continue;

					conn->state = STATE_HEADER_SENT;
					conn->transfered_bytes = 0;
					break;

				case STATE_HEADER_SENT:
					/* when header is completely sent, start the async reading */
					start_readfile_async(conn);
					conn->state = STATE_ASYNC_READ_STARTED;
					break;

				case STATE_ASYNC_READ_FINISHED:
					/*
					 * when async read is finished, start sending the buffers
					 * in order, considering that socket are non-blocking
					 */
					bytes_sent = send(conn->sockfd,
									  conn->aio_buffers[conn->current_buffer_idx] + conn->transfered_bytes,
									  conn->buffers_data_size[conn->current_buffer_idx] - conn->transfered_bytes,
									  0);

					if (bytes_sent <= 0) {
						/* destroy the io context */
						rc = io_destroy(conn->context);
						DIE(rc < 0, "io_destroy");

						close(conn->fd);

						free_buffers(conn);
						connection_remove(conn);
					}

					/*
					 * add the number of bytes sent with this call
					 * if there are still bytes in the current buffer, continue
					 */
					conn->transfered_bytes += bytes_sent;
					if (conn->transfered_bytes < conn->buffers_data_size[conn->current_buffer_idx])
						continue;

					/* if the current buffer is completely sent, go for the next one */
					conn->current_buffer_idx++;
					conn->transfered_bytes = 0;

					/*
					 * if all buffers were sent, close the connection
					 * and remove the socket from epoll
					 */
					if (conn->current_buffer_idx == conn->total_aio_reads) {
						dprintf("Succesfully sent %ld bytes from file: %s\n",
								 conn->file_size,
								 conn->file_path);

						/* destroy the io context */
						rc = io_destroy(conn->context);
						DIE(rc < 0, "io_destroy");

						close(conn->fd);

						free_buffers(conn);
						connection_remove(conn);
					}
					break;

				default:
					break;
				}

				break;

			default:
				break;
			}
		}
	}

	return 0;
}
