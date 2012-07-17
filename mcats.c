#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <assert.h>
#include <limits.h>
#include <fcntl.h>

#define BUFSIZE 1024

static size_t g_wait_ms = 0;
// set = ~set
void fd_set_not(fd_set* set)
{
	for (size_t i = 0; i < FD_SETSIZE/__NFDBITS; i++) {
		__FDS_BITS(set)[i] = ~__FDS_BITS(set)[i];
	}
}

// res = a & b
void fd_set_and(fd_set* res, fd_set const* a, fd_set const* b)
{
	for (size_t i = 0; i < FD_SETSIZE/__NFDBITS; i++) {
		__FDS_BITS(res)[i] = __FDS_BITS(a)[i] & __FDS_BITS(b)[i];
	}
}

bool fd_is_empty(fd_set const* set)
{
	for (size_t i = 0; i < FD_SETSIZE/__NFDBITS; i++) {
		if (__FDS_BITS(set)[i] != 0) {
			return false;
		}
	}
	return true;
}

int* create_sockets(size_t n, int* phighest_fd, fd_set* ret_fd_set)
{
	int* sockets = (int*) malloc(n*sizeof(int));
	if (sockets == NULL) {
		return NULL;
	}
	FD_ZERO(ret_fd_set);
	int max_fd = 0;
	for (size_t i = 0; i < n; i++) {
		int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
		if (s == -1) {
			perror("socket");
			free(sockets);
			return NULL;
		}
		if (s > max_fd) {
			max_fd = s;
		}
		FD_SET(s, ret_fd_set);
		sockets[i] = s;
	}
	*phighest_fd = max_fd;
	return sockets;
}

void write_stdin_all_sockets(int* sockets, size_t n, fd_set* all_read_set)
{
	char buf[BUFSIZE];
	size_t sum = 0;
	while (true) {
		int r = read(STDIN_FILENO, buf, BUFSIZE);
		if (r < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				break;
			}
			perror("read stdin");
			exit(1);
		}
		if (r == 0) {
			// stdin has been closed. Do not "select" it anymore.
			fprintf(stderr, "stdin: pipeline closed.\n");
			FD_CLR(0, all_read_set);
			break;
		}

		// Write buf into sockets
		//if (g_wait_ms == 0) {
			for (size_t i = 0; i < n; i++) {
				if (sockets[i] >= 0) {
					if (write(sockets[i], buf, r) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
						perror("write socket");
						sockets[i] = -1;
					}
				}
			}
		/*}
		else {
			for (int ibuf = 0; ibuf < r; ibuf++) {
				for (size_t i = 0; i < n; i++) {
					if (sockets[i] >= 0) {
						if (write(sockets[i], &buf[ibuf], 1) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
							perror("write socket");
							sockets[i] = -1;
						}
					}
				}
				usleep(g_wait_ms*1000);
			}
		}*/

		sum += r;
	}
	if (sum > 0) {
		fprintf(stderr, "%lu bytes read from stdin.\n", sum);
	}
}

int read_sockets_to_files(int* sockets, size_t n, fd_set const* fdset, int* res_files, fd_set* all_read_set)
{
	char buf[BUFSIZE];
	int nclosed = 0;
	for (size_t i = 0; i < n; i++) {
		int s = sockets[i];
		if (s == -1) {
			continue;
		}
		if (!(FD_ISSET(s, fdset))) {
			continue;
		}

		// Process this socket !
		size_t sum = 0;
		while (true) {
			int r;
			//if (g_wait_ms == 0) {
				r = read(s, buf, BUFSIZE);
			/*}
			else {
				r;
				int rem = BUFSIZE;
				usleep(g_wait_ms*1000);
				while (((r = read(s, buf, 1)) > 0) && rem > 0) {
					rem--;
					usleep(g_wait_ms*1000);
				}
			}*/

			if (r < 0) {
				if (errno == EWOULDBLOCK || errno == EAGAIN) {
					break;
				}
				perror("read socket");
				sockets[i] = -1;
				break;
			}
			if (r == 0) {
				// Socket has been closed. Do not "select" it anymore.
				fprintf(stderr, "Sock %d: connection closed.\n", s);
				FD_CLR(s, all_read_set);
				sockets[i] = -1;
				nclosed++;
				break;
			}
			write(res_files[i], buf, r);
			sum += r;
		}
		if (sum > 0) {
			fprintf(stderr, "Sock %d: %lu bytes received.\n", s, sum);
		}
	}
	return nclosed;
}

int main(int argc, char **argv)
{
	if (argc <= 2) {
		fprintf(stderr, "Usage: %s host port [n] [wait_msec]\n", argv[0]);
		return 1;
	}

	const char* host_str = argv[1];

	size_t nconn = 1;
	if (argc >= 4) {
		nconn = atoll(argv[3]);
	}

	size_t wait_ms = 0;
	if (argc >= 5) {
		wait_ms = atoll(argv[4]);
	}
	g_wait_ms = wait_ms;

	uint16_t port = atoi(argv[2]);
	if (port == 0) {
		return 1;
	}

	struct hostent* hp = gethostbyname(host_str);
	if (hp == NULL) {
		perror("gethostbyname");
		return 1;
	}

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	memcpy(&sin.sin_addr, hp->h_addr_list[0], hp->h_length);
	sin.sin_family = hp->h_addrtype;
	sin.sin_port = htons(port);

	// Create sockets
	int max_sock = 0;
	fd_set set_socks;
	int* sockets = create_sockets(nconn, &max_sock, &set_socks);
	if (sockets == NULL) {
		return 1;
	}

	fprintf(stderr, "[*] Initializing %lu connections to %s:%d...\n[*] ", nconn, host_str, port);
	for (size_t i = 0; i < nconn; i++) {
		int r = connect(sockets[i], (struct sockaddr*) &sin, sizeof(struct sockaddr_in));
		if (r < 0 && errno != EINPROGRESS) {
			perror("connect");
			free(sockets);
			return errno;
		}
		fprintf(stderr, ".");
	}

	fprintf(stderr, "\n[*] %lu connections to %s:%d have been initiated... Waiting for completion...\n[*] ", nconn, host_str, port);

	size_t nconn_success = 0;
	size_t nconn_ret = 0;
	int* sockets_success = (int*) malloc(nconn*sizeof(int));
	fd_set fd_rem_socks = set_socks;
	FD_ZERO(&set_socks);
	int success_max_sock = 0;
	while (nconn_ret < nconn) {
		fd_set fd_sel_socks = fd_rem_socks;
		int ret_sel = select(max_sock+1, NULL, &fd_sel_socks, NULL, NULL);
		if (ret_sel < 0) {
			perror("select");
			free(sockets);
			return errno;
		}

		for (size_t i = 0; i < nconn; i++) {
			int socket = sockets[i];
			if (FD_ISSET(socket, &fd_sel_socks)) {
				// Call(s) to connect has return. Check whether it has succedded !
				nconn_ret++;
				assert(nconn_ret <= nconn);
				int conn_err = 0;
				socklen_t tmp_size = sizeof(int);
				getsockopt(socket, SOL_SOCKET, SO_ERROR, &conn_err, &tmp_size);
				if (conn_err != 0) {
					fprintf(stderr, "Error for a connection: %s\n", strerror(conn_err));
					close(socket);
				}
				else {
					sockets_success[nconn_success] = socket;
					nconn_success++;
					FD_SET(socket, &set_socks);
					if (socket > success_max_sock) {
						success_max_sock = socket;
					}
					fprintf(stderr, ".");
				}
			}
		}
		// fd_rem_socks &= ~fd_sel_socks;
		fd_set_not(&fd_sel_socks);
		fd_set_and(&fd_rem_socks, &fd_rem_socks, &fd_sel_socks);
	}
	fprintf(stderr, "\n[*] %lu connections were successfull.\n", nconn_success);
	/*if (wait_ms > 0) {
		fprintf(stderr, "\n[*] Waiting %lu ms...\n", wait_ms);
		usleep(wait_ms*1000);
	}*/
	free(sockets);
	sockets = sockets_success;
	max_sock = success_max_sock;

	if (nconn_success == 0) {
		return 1;
	}

	// Open result files
	int* res_files = (int*) malloc(nconn_success*sizeof(int));
	for (size_t i = 0; i < nconn_success; i++) {
		char path[PATH_MAX];
		snprintf(path, PATH_MAX-1, "sock.out.%lu", i);
		int fd_file = open(path, O_CREAT|O_TRUNC|O_WRONLY);
		if (fd_file < 0) {
			perror("open");
			return 1;
		}
		fchmod(fd_file, 0640);
		res_files[i] = fd_file;
	}

	// Set stdin to non-blocking mode.
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

	// Forward all data from stdin to all connections.
	// Write data from all connections to their respective fd.
	
	FD_SET(STDIN_FILENO, &set_socks);
	fd_set read_set_fd;
	int nsocks_alive = nconn_success;
	while (true) {
		read_set_fd = set_socks;
		int ret_sel = select(max_sock+1, &read_set_fd, NULL, NULL, NULL);
		if (ret_sel < 0) {
			perror("select");
			return 1;
		}

		if (FD_ISSET(STDIN_FILENO, &read_set_fd)) {
			write_stdin_all_sockets(sockets, nconn_success, &set_socks);
			ret_sel--;
		}
		if (ret_sel > 0) {
			int nsocks_closed = read_sockets_to_files(sockets, nconn_success, &read_set_fd, res_files, &set_socks);
			nsocks_alive -= nsocks_closed;
			if (nsocks_alive == 0) {
				break;
			}
		}
	}

	for (size_t i = 0; i < nconn_success; i++) {
		close(res_files[i]);
		close(sockets[i]);
	}

	free(sockets);
	free(res_files);

	return 0;
}
