#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void reportErrorAndExit(int fd, int error) {
	(void)write(fd, &error, sizeof(error));
	_exit(127);
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s PROGRAM [ARG ...]\n", argv[0]);
		return 2;
	}

	int statusPipe[2];
	if (pipe(statusPipe) != 0) {
		perror("pipe");
		return 1;
	}
	if (fcntl(statusPipe[1], F_SETFD, FD_CLOEXEC) != 0) {
		perror("fcntl(FD_CLOEXEC)");
		return 1;
	}

	pid_t firstChild = fork();
	if (firstChild < 0) {
		perror("fork");
		return 1;
	}
	if (firstChild == 0) {
		close(statusPipe[0]);
		if (setsid() < 0)
			reportErrorAndExit(statusPipe[1], errno);
		(void)signal(SIGHUP, SIG_IGN);

		pid_t grandchild = fork();
		if (grandchild < 0)
			reportErrorAndExit(statusPipe[1], errno);
		if (grandchild > 0) {
			close(statusPipe[1]);
			_exit(0);
		}

		umask(022);
		if (chdir("/tmp") != 0)
			reportErrorAndExit(statusPipe[1], errno);
		int nullFd = open("/dev/null", O_RDWR | O_CLOEXEC);
		if (nullFd < 0)
			reportErrorAndExit(statusPipe[1], errno);
		if (dup2(nullFd, STDIN_FILENO) < 0 || dup2(nullFd, STDOUT_FILENO) < 0 ||
			dup2(nullFd, STDERR_FILENO) < 0)
			reportErrorAndExit(statusPipe[1], errno);
		if (nullFd > STDERR_FILENO)
			close(nullFd);

		execv(argv[1], &argv[1]);
		reportErrorAndExit(statusPipe[1], errno);
	}

	close(statusPipe[1]);
	int childError = 0;
	ssize_t bytes;
	do {
		bytes = read(statusPipe[0], &childError, sizeof(childError));
	} while (bytes < 0 && errno == EINTR);
	close(statusPipe[0]);
	(void)waitpid(firstChild, NULL, 0);
	if (bytes == 0)
		return 0;
	if (bytes == (ssize_t)sizeof(childError)) {
		errno = childError;
		perror("detached exec");
	} else if (bytes < 0) {
		perror("detached launch status");
	} else {
		fprintf(stderr, "detached launch returned a truncated status\n");
	}
	return 1;
}

