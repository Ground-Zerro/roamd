#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>

#include "roamd.h"
#include "mesh.h"

#define SSH_KEY		"/etc/roamd/id"
#define SSH_USER	"root"
#define KNOWN_HOSTS	"/root/.ssh/known_hosts"
#define RUN_ARGV_MAX	24
#define SCP_TIMEOUT	120000

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int mesh_run(char *const argv[], int in_fd, char *out, size_t len, int timeout_ms)
{
	int out_fd[2];
	struct pollfd pfd;
	size_t used = 0;
	int status = -1;
	int64_t deadline;
	pid_t pid;

	if (out && len)
		out[0] = 0;

	if (pipe(out_fd))
		return -1;

	pid = fork();
	if (pid < 0) {
		close(out_fd[0]);
		close(out_fd[1]);

		return -1;
	}

	if (!pid) {
		int null_rd = in_fd < 0 ? open("/dev/null", O_RDONLY) : -1;
		int null_wr = open("/dev/null", O_WRONLY);

		dup2(out_fd[1], STDOUT_FILENO);

		if (in_fd >= 0) {
			dup2(in_fd, STDIN_FILENO);
			close(in_fd);
		} else if (null_rd >= 0) {
			dup2(null_rd, STDIN_FILENO);
			close(null_rd);
		}

		if (null_wr >= 0) {
			dup2(null_wr, STDERR_FILENO);
			close(null_wr);
		}

		close(out_fd[0]);
		close(out_fd[1]);
		execv(argv[0], argv);
		_exit(127);
	}

	close(out_fd[1]);

	pfd.fd = out_fd[0];
	pfd.events = POLLIN;
	deadline = now_ms() + timeout_ms;

	for (;;) {
		char buf[1024];
		int64_t left = deadline - now_ms();
		ssize_t got;
		int ready;

		if (left <= 0)
			break;

		ready = poll(&pfd, 1, (int)left);

		if (ready < 0) {
			if (errno == EINTR)
				continue;

			break;
		}

		if (!ready)
			break;

		got = read(out_fd[0], buf, sizeof(buf));

		if (got < 0) {
			if (errno == EINTR)
				continue;

			break;
		}

		if (!got)
			break;

		if (!out || used + got + 1 >= len)
			continue;

		memcpy(out + used, buf, got);
		used += got;
		out[used] = 0;
	}

	close(out_fd[0]);

	for (;;) {
		struct timespec ts = { .tv_nsec = 50000000 };
		pid_t done = waitpid(pid, &status, WNOHANG);

		if (done == pid)
			break;

		if (done < 0 && errno != EINTR)
			return -1;

		if (now_ms() >= deadline) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);

			return -1;
		}

		nanosleep(&ts, NULL);
	}

	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool mesh_port_open(const char *addr, uint16_t port, int timeout_ms)
{
	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
				  .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV };
	struct addrinfo *res = NULL;
	char service[8];
	bool open = false;
	int fd;

	snprintf(service, sizeof(service), "%u", port);

	if (getaddrinfo(addr, service, &hints, &res) || !res)
		return false;

	fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (fd >= 0) {
		if (!connect(fd, res->ai_addr, res->ai_addrlen)) {
			open = true;
		} else if (errno == EINPROGRESS) {
			struct pollfd pfd = { .fd = fd, .events = POLLOUT };
			int ready;

			do {
				ready = poll(&pfd, 1, timeout_ms);
			} while (ready < 0 && errno == EINTR);

			if (ready > 0) {
				socklen_t len = sizeof(int);
				int err = 0;

				if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) && !err)
					open = true;
			}
		}

		close(fd);
	}

	freeaddrinfo(res);

	return open;
}

static void argv_push(char **argv, unsigned int *n, char *value)
{
	if (*n + 1 < RUN_ARGV_MAX)
		argv[(*n)++] = value;
}

void mesh_node_forget(const char *addr)
{
	char pattern[MESH_ADDR_MAX + 16];
	char *argv[RUN_ARGV_MAX];
	unsigned int n = 0;

	if (access(KNOWN_HOSTS, W_OK))
		return;

	snprintf(pattern, sizeof(pattern), "/^%s /d;/^\\[%s\\]/d", addr, addr);

	argv_push(argv, &n, "/bin/sed");
	argv_push(argv, &n, "-i");
	argv_push(argv, &n, pattern);
	argv_push(argv, &n, KNOWN_HOSTS);
	argv[n] = NULL;

	mesh_run(argv, -1, NULL, 0, 5000);
}

int mesh_ssh(const char *addr, const char *cmd, char *out, size_t len, int timeout_ms)
{
	char target[MESH_ADDR_MAX + 8];
	char *argv[RUN_ARGV_MAX];
	unsigned int n = 0;

	snprintf(target, sizeof(target), "%s@%s", SSH_USER, addr);

	argv_push(argv, &n, "/usr/bin/ssh");
	argv_push(argv, &n, "-y");
	argv_push(argv, &n, "-y");

	if (!access(SSH_KEY, R_OK)) {
		argv_push(argv, &n, "-i");
		argv_push(argv, &n, SSH_KEY);
	}

	argv_push(argv, &n, target);
	argv_push(argv, &n, (char *)cmd);
	argv[n] = NULL;

	return mesh_run(argv, -1, out, len, timeout_ms);
}

int mesh_ssh_pass(const char *addr, const char *cmd, char *out, size_t len, int timeout_ms)
{
	char target[MESH_ADDR_MAX + 8];
	char *argv[RUN_ARGV_MAX];
	unsigned int n = 0;

	snprintf(target, sizeof(target), "%s@%s", SSH_USER, addr);

	argv_push(argv, &n, "/usr/bin/ssh");
	argv_push(argv, &n, "-y");
	argv_push(argv, &n, "-y");
	argv_push(argv, &n, target);
	argv_push(argv, &n, (char *)cmd);
	argv[n] = NULL;

	return mesh_run(argv, -1, out, len, timeout_ms);
}

bool mesh_scp(const char *src, const char *addr, const char *dst)
{
	char target[MESH_ADDR_MAX + 8], cmd[256];
	char *argv[RUN_ARGV_MAX];
	unsigned int n = 0;
	int fd, rc;

	fd = open(src, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;

	snprintf(target, sizeof(target), "%s@%s", SSH_USER, addr);
	snprintf(cmd, sizeof(cmd), "cat > '%s'", dst);

	argv_push(argv, &n, "/usr/bin/ssh");
	argv_push(argv, &n, "-y");
	argv_push(argv, &n, "-y");

	if (!access(SSH_KEY, R_OK)) {
		argv_push(argv, &n, "-i");
		argv_push(argv, &n, SSH_KEY);
	}

	argv_push(argv, &n, target);
	argv_push(argv, &n, cmd);
	argv[n] = NULL;

	rc = mesh_run(argv, fd, NULL, 0, SCP_TIMEOUT);
	close(fd);

	return rc == 0;
}

bool mesh_scp_from(const char *addr, const char *src, const char *dst)
{
	char cmd[256], *data;
	size_t len;
	FILE *f;

	snprintf(cmd, sizeof(cmd), "cat '%s'", src);

	data = calloc(1, 16384);
	if (!data)
		return false;

	if (mesh_ssh(addr, cmd, data, 16384, 30000)) {
		free(data);

		return false;
	}

	len = strlen(data);
	if (!len) {
		free(data);

		return false;
	}

	f = fopen(dst, "w");
	if (!f) {
		free(data);

		return false;
	}

	fwrite(data, 1, len, f);
	fclose(f);
	free(data);

	return true;
}
