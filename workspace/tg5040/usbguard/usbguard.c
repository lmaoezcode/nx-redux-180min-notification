// Bound an ADB pause to one USB transaction. The independent guardian is
// armed BEFORE SIGSTOP; it survives worker death and the loss of USB/ADB.
// No mount, storage export, ConfigFS write, firmware edit or role change here.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GUARD_LOCK "/tmp/nx_usb_guard.lock"
#define MODE_LOCK "/tmp/nx_usb_mode.lock"
#define GUARD_LOG "/tmp/nx-usb-guard.txt"
#define LIMIT_MS 20000
struct Identity { pid_t pid; unsigned long long start; char state; char exe[512]; };
static volatile sig_atomic_t interrupted;
static int journal = -1;
static bool test_kill_guardian;
static void stop_requested(int sig) { (void)sig; interrupted = 1; }
static int64_t now_ms(void) {
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void pause_ms(unsigned ms) {
	struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
	(void)nanosleep(&t, NULL);
}
static void note(const char *stage, pid_t pid) {
	if (journal >= 0) dprintf(journal, "mono_ms=%lld guard=%ld stage=%s target=%ld\n",
		(long long)now_ms(), (long)getpid(), stage, (long)pid);
}
static bool identity(pid_t pid, struct Identity *out, bool need_exe) {
	char path[96], line[4096];
	if (pid <= 1) return false;
	snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
	FILE *f = fopen(path, "r");
	if (!f) return false;
	bool ok = fgets(line, sizeof(line), f) != NULL;
	fclose(f);
	char *end = ok ? strrchr(line, ')') : NULL;
	if (!end || end[1] != ' ') return false;
	char *context = NULL, *part = strtok_r(end + 2, " ", &context);
	for (unsigned field = 3; part; ++field, part = strtok_r(NULL, " ", &context)) {
		if (field == 3) out->state = *part;
		if (field == 22) {
			char *tail; errno = 0;
			out->start = strtoull(part, &tail, 10);
			if (errno || *tail) return false;
			break;
		}
	}
	if (!part) return false;
	out->pid = pid;
	if (!need_exe) return true;
	snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
	ssize_t n = readlink(path, out->exe, sizeof(out->exe) - 1);
	if (n < 1 || (size_t)n >= sizeof(out->exe) - 1) return false;
	out->exe[n] = 0;
	return true;
}
static bool same(const struct Identity *id, bool need_exe) {
	struct Identity current = {0};
	return identity(id->pid, &current, need_exe) && current.start == id->start &&
		(!need_exe || !strcmp(current.exe, id->exe));
}
static bool stopped(const struct Identity *id) {
	char path[96];
	if (!same(id, true)) return false;
	snprintf(path, sizeof(path), "/proc/%ld/task", (long)id->pid);
	DIR *dir = opendir(path);
	if (!dir) return false;
	bool ok = true; unsigned count = 0;
	struct dirent *e;
	while ((e = readdir(dir))) {
		char *end; long tid = strtol(e->d_name, &end, 10);
		if (*end || tid < 2) continue;
		struct Identity task = {0};
		if (!identity((pid_t)tid, &task, false) || (task.state != 'T' && task.state != 't')) ok = false;
		++count;
	}
	closedir(dir);
	return ok && count;
}
static bool discover(struct Identity *adb) {
	DIR *dir = opendir("/proc");
	if (!dir) return false;
	struct dirent *e; unsigned count = 0;
	while ((e = readdir(dir))) {
		char *end; long pid = strtol(e->d_name, &end, 10);
		if (*end || pid < 2) continue;
		struct Identity id = {0};
		if (identity((pid_t)pid, &id, true) && !strcmp(id.exe, "/bin/adbd")) { *adb = id; ++count; }
	}
	closedir(dir);
	return count == 1 && adb->state != 'T' && adb->state != 't';
}
static bool resume(const struct Identity *adb) {
	if (!same(adb, true) || kill(adb->pid, SIGCONT)) { note("adb_resume_failed", adb->pid); return false; }
	int64_t limit = now_ms() + 500;
	while (stopped(adb) && now_ms() < limit) pause_ms(10);
	bool ok = same(adb, true) && !stopped(adb);
	note(ok ? "adb_resumed" : "adb_resume_not_verified", adb->pid);
	return ok;
}
static void terminate_worker(const struct Identity *worker) {
	// Only our child session, matched by PID + start-time, not named processes.
	if (!same(worker, false)) return;
	(void)kill(-worker->pid, SIGTERM);
	int64_t limit = now_ms() + 500;
	while (same(worker, false) && now_ms() < limit) {
		struct Identity id = {0};
		if (!identity(worker->pid, &id, false) || id.state == 'Z') break;
		pause_ms(10);
	}
	struct Identity id = {0};
	if (same(worker, false) && identity(worker->pid, &id, false) && id.state != 'Z')
		(void)kill(-worker->pid, SIGKILL);
}
static void remove_owned_mode_lock(pid_t worker) {
	char path[] = MODE_LOCK "/pid"; long owner = 0;
	FILE *f = fopen(path, "r");
	if (!f) return;
	bool ours = fscanf(f, "%ld", &owner) == 1 && owner == (long)worker;
	fclose(f);
	if (ours) { (void)unlink(path); (void)rmdir(MODE_LOCK); }
}
static void guardian(const struct Identity *adb, const struct Identity *worker,
		int control, int ack, unsigned deadline_ms) {
	(void)setsid();
	signal(SIGHUP, SIG_IGN); signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN);
	// Drop inherited terminals; all diagnostics are in the RAM journal.
	int nullfd = open("/dev/null", O_RDWR);
	if (nullfd >= 0) { for (int i = 0; i < 3; ++i) (void)dup2(nullfd, i); if (nullfd > 2) close(nullfd); }
	char ready = 'R';
	if (write(ack, &ready, 1) != 1) _exit(1); // No ADB pause has happened yet.
	int64_t deadline = now_ms() + deadline_ms;
	bool completed = false;
	while (now_ms() < deadline) {
		struct pollfd p = {control, POLLIN | POLLHUP, 0};
		int rc = poll(&p, 1, 100);
		if (rc < 0 && errno == EINTR) continue;
		if (rc < 0) break;
		if (rc > 0) { char c; completed = read(control, &c, 1) == 1 && c == 'D'; break; }
	}
	if (!completed) { note("worker_aborted_or_deadline", worker->pid); terminate_worker(worker); }
	bool ok = resume(adb);
	remove_owned_mode_lock(worker->pid);
	char result = ok ? 'Y' : 'N'; (void)write(ack, &result, 1);
	_exit(ok ? 0 : 1);
}
static int transaction(const struct Identity *adb, char **command, unsigned limit_ms) {
	if (adb->state == 'T' || adb->state == 't') return 1; // Never resume another owner's pause.
	int gate[2], control[2], ack[2];
	if (pipe2(gate, O_CLOEXEC) || pipe2(control, O_CLOEXEC) || pipe2(ack, O_CLOEXEC)) return 1;
	pid_t worker_pid = fork();
	if (worker_pid < 0) return 1;
	if (!worker_pid) {
		close(gate[1]); close(control[0]); close(control[1]); close(ack[0]); close(ack[1]);
		if (setsid() < 0) _exit(125);
		signal(SIGHUP, SIG_IGN); signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
		char c; if (read(gate[0], &c, 1) != 1 || c != 'G') _exit(125);
		close(gate[0]);
		execv(command[0], command); _exit(127);
	}
	close(gate[0]);
	struct Identity worker = {0};
	if (!identity(worker_pid, &worker, false)) { close(gate[1]); waitpid(worker_pid, NULL, 0); return 1; }
	pid_t watcher = fork();
	if (watcher < 0) { close(gate[1]); waitpid(worker_pid, NULL, 0); return 1; }
	if (!watcher) { close(gate[1]); close(control[1]); close(ack[0]); guardian(adb, &worker, control[0], ack[1], limit_ms); }
	close(control[0]); close(ack[1]);
	char c = 0; struct pollfd p = {ack[0], POLLIN, 0};
	bool armed = poll(&p, 1, 1000) > 0 && read(ack[0], &c, 1) == 1 && c == 'R';
	bool paused = armed && same(adb, true) && !kill(adb->pid, SIGSTOP);
	int64_t pause_limit = now_ms() + 500;
	while (paused && !stopped(adb) && now_ms() < pause_limit) pause_ms(10);
	paused = paused && stopped(adb);
	note(paused ? "adb_paused_guardian_armed" : "adb_pause_refused", adb->pid);
	if (paused && test_kill_guardian) (void)kill(watcher, SIGKILL);
	if (paused) { c = 'G'; if (write(gate[1], &c, 1) != 1) paused = false; }
	close(gate[1]);
	int status = 0; bool finished = false, watcher_reaped = false;
	int64_t deadline = now_ms() + limit_ms;
	while (now_ms() < deadline) {
		pid_t rc = waitpid(worker_pid, &status, WNOHANG);
		if (rc == worker_pid) { finished = true; break; }
		if (rc < 0 && errno != EINTR) break;
		if (waitpid(watcher, NULL, WNOHANG) == watcher) { watcher_reaped = true; break; }
		if (interrupted || !paused) break;
		pause_ms(10);
	}
	if (!finished) { terminate_worker(&worker); while (waitpid(worker_pid, &status, 0) < 0 && errno == EINTR) {} }
	// Do not let an inherited pipe descriptor delay EOF after parent death.
	c = finished && paused ? 'D' : 'F'; (void)write(control[1], &c, 1); close(control[1]);
	p = (struct pollfd){ack[0], POLLIN, 0};
	bool resumed = poll(&p, 1, 1500) > 0 && read(ack[0], &c, 1) == 1 && c == 'Y';
	close(ack[0]);
	if (!resumed) resumed = resume(adb); // Also recover if the guardian died.
	if (!watcher_reaped) while (waitpid(watcher, NULL, 0) < 0 && errno == EINTR) {}
	remove_owned_mode_lock(worker_pid);
	if (!paused || !finished || !resumed || interrupted) return 1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static bool under(const char *path, const char *root) {
	size_t n = strlen(root);
	return !strncmp(path, root, n) && (path[n] == '/' || !path[n]);
}
static void inspect_link(pid_t pid, const char *path, const char *root) {
	char target[1024]; ssize_t n = readlink(path, target, sizeof(target) - 1);
	if (n <= 0 || (size_t)n >= sizeof(target) - 1) return;
	target[n] = 0;
	if (under(target, root)) printf("pid=%ld %s -> %s\n", (long)pid, path, target);
}
static int holders(const char *root) {
	if (!root || root[0] != '/' || strlen(root) < 2) return 2;
	DIR *dir = opendir("/proc"); if (!dir) return 1;
	struct dirent *e;
	while ((e = readdir(dir))) {
		char *end; long pid = strtol(e->d_name, &end, 10);
		if (*end || pid < 2) continue;
		char path[128], line[2048];
		snprintf(path, sizeof(path), "/proc/%ld/cwd", pid); inspect_link(pid, path, root);
		snprintf(path, sizeof(path), "/proc/%ld/exe", pid); inspect_link(pid, path, root);
		snprintf(path, sizeof(path), "/proc/%ld/fd", pid);
		DIR *fds = opendir(path);
		if (fds) {
			struct dirent *fd;
			while ((fd = readdir(fds))) {
				char *tail; (void)strtol(fd->d_name, &tail, 10); if (*tail) continue;
				snprintf(path, sizeof(path), "/proc/%ld/fd/%.32s", pid, fd->d_name);
				inspect_link(pid, path, root);
			}
			closedir(fds);
		}
		snprintf(path, sizeof(path), "/proc/%ld/maps", pid);
		FILE *maps = fopen(path, "r");
		if (maps) { while (fgets(line, sizeof(line), maps)) if (strstr(line, root)) printf("pid=%ld map %s", pid, line); fclose(maps); }
	}
	closedir(dir); return 0;
}
static int selftest(void) {
	// Fake ADB is ONLY our own child, not the real daemon. No USB/sysfs/SD I/O.
	pid_t fake = fork(); if (fake < 0) return 1;
	if (!fake) { signal(SIGTERM, SIG_DFL); for (;;) pause(); }
	struct Identity id = {0};
	if (!identity(fake, &id, true)) { kill(fake, SIGKILL); waitpid(fake, NULL, 0); return 1; }
	char *ok[] = {"/bin/true", NULL}, *fail[] = {"/bin/false", NULL}, *slow[] = {"/bin/sleep", "2", NULL};
	unsigned passed = 0;
	if (!transaction(&id, ok, 500) && !stopped(&id)) { puts("PASS successful child + resume"); ++passed; }
	if (transaction(&id, fail, 500) && !stopped(&id)) { puts("PASS failing child + resume"); ++passed; }
	if (transaction(&id, slow, 150) && !stopped(&id)) { puts("PASS deadline + resume"); ++passed; }
	test_kill_guardian = true;
	if (transaction(&id, slow, 500) && !stopped(&id)) { puts("PASS guardian death + parent recovery"); ++passed; }
	test_kill_guardian = false;
	struct Identity wrong = id; ++wrong.start;
	if (transaction(&wrong, ok, 500) && !stopped(&id)) { puts("PASS PID/start mismatch refused"); ++passed; }
	(void)kill(fake, SIGSTOP);
	int64_t limit = now_ms() + 500; while (!stopped(&id) && now_ms() < limit) pause_ms(5);
	struct Identity held = id; held.state = 'T';
	if (transaction(&held, ok, 500) && stopped(&id)) { puts("PASS external pause preserved"); ++passed; }
	(void)kill(fake, SIGCONT);
	pid_t owner = fork();
	if (!owner) _exit(transaction(&id, slow, 500));
	if (owner > 0) {
		limit = now_ms() + 1000; while (!stopped(&id) && now_ms() < limit) pause_ms(5);
		bool was_stopped = stopped(&id);
		(void)kill(owner, SIGKILL); waitpid(owner, NULL, 0);
		limit = now_ms() + 2000; while (stopped(&id) && now_ms() < limit) pause_ms(10);
		if (was_stopped && !stopped(&id)) { puts("PASS parent death + independent guardian recovery"); ++passed; }
	}
	(void)kill(fake, SIGCONT); (void)kill(fake, SIGTERM); waitpid(fake, NULL, 0);
	printf("selftest_passed=%u expected=7\n", passed);
	return passed == 7 ? 0 : 1;
}
int main(int argc, char **argv) {
	if (argc == 3 && !strcmp(argv[1], "--holders")) return holders(argv[2]);
	signal(SIGHUP, SIG_IGN); signal(SIGPIPE, SIG_IGN);
	if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
	if (argc < 2 || argv[1][0] != '/') return 2;
	struct sigaction sa = {.sa_handler = stop_requested};
	sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
	journal = open(GUARD_LOG, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	int lockfd = open(GUARD_LOCK, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (journal < 0 || lockfd < 0) return 1;
	int64_t deadline = now_ms() + 5000;
	while (flock(lockfd, LOCK_EX | LOCK_NB)) {
		if (errno != EWOULDBLOCK || now_ms() >= deadline || interrupted) return 1;
		pause_ms(20);
	}
	struct Identity adb = {0};
	if (!discover(&adb)) { note("unique_running_stock_adb_not_found", 0); return 1; }
	int rc = transaction(&adb, &argv[1], LIMIT_MS);
	note(rc ? "transaction_failed" : "transaction_complete", adb.pid);
	close(lockfd); close(journal); return rc;
}
