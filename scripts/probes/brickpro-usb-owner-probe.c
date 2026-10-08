/* Temporary ownership probe, NOT a controller launcher.
 * Only suspends the exact stock adbd instance and, optionally, unbinds/rebinds
 * the unchanged gadget. Independent guardian survives worker death/ADB loss.
 * No role trigger, descriptor/link changes, motor, radio or SD writes.
 */
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

enum Fault { NO_FAULT, WORKER_AFTER_STOP, WORKER_AFTER_UNBIND,
	GUARD_AFTER_STOP, GUARD_AFTER_UNBIND };
struct Identity { pid_t pid; unsigned long long start; char state; char exe[512]; };
struct Snapshot {
	struct Identity adb;
	char udc_path[512], udc[128];
	int log_fd;
	char restore_lock_path[512];
	bool unbind;
	unsigned hold_ms, guardian_ms;
	enum Fault fault;
	void *context;
	bool (*before_unbind)(const struct Snapshot *);
	bool (*resume_extra)(const struct Snapshot *);
	bool (*activate)(const struct Snapshot *);
	int (*step)(const struct Snapshot *);
	void (*worker_release)(const struct Snapshot *);
	int (*cleanup)(const struct Snapshot *);
};
static volatile sig_atomic_t interrupted;

static void request_stop(int sig) { (void)sig; interrupted = 1; }
static int64_t now_ms(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void pause_ms(unsigned ms) {
	struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
	(void)nanosleep(&ts, NULL);
}
static void record(const struct Snapshot *s, const char *stage, int detail) {
	char line[256];
	int length = snprintf(line, sizeof(line), "mono_ms=%lld pid=%ld stage=%s detail=%d\n",
		(long long)now_ms(), (long)getpid(), stage, detail);
	if (length > 0 && (size_t)length < sizeof(line)) (void)write(s->log_fd, line, (size_t)length);
}
static int read_value(const char *path, char *out, size_t size) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t n = read(fd, out, size - 1);
	int saved = errno;
	close(fd);
	if (n < 0) { errno = saved; return -1; }
	if ((size_t)n == size - 1) { errno = EOVERFLOW; return -1; }
	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) --n;
	out[n] = 0;
	return 0;
}
static int write_value(const char *path, const char *value) {
	int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0) return -1;
	size_t n = strlen(value);
	ssize_t written = write(fd, value, n);
	int saved = errno;
	close(fd);
	if (written != (ssize_t)n) { errno = written < 0 ? saved : EIO; return -1; }
	return 0;
}
static int identity(pid_t pid, struct Identity *out) {
	char path[80], data[4096];
	if (pid <= 1) return -1;
	snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
	if (read_value(path, data, sizeof(data))) return -1;
	char *end = strrchr(data, ')');
	if (!end || end[1] != ' ') return -1;
	char *context = NULL, *token = strtok_r(end + 2, " ", &context);
	for (unsigned field = 3; token; ++field, token = strtok_r(NULL, " ", &context)) {
		if (field == 3) out->state = token[0];
		if (field == 22) {
			char *tail;
			errno = 0;
			out->start = strtoull(token, &tail, 10);
			if (errno || *tail) return -1;
			break;
		}
	}
	if (!token) return -1;
	snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
	ssize_t n = readlink(path, out->exe, sizeof(out->exe) - 1);
	if (n <= 0 || (size_t)n == sizeof(out->exe) - 1) return -1;
	out->exe[n] = 0;
	out->pid = pid;
	return 0;
}
static bool same_process(const struct Identity *expected) {
	struct Identity current = {0};
	return !identity(expected->pid, &current) && current.start == expected->start &&
		!strcmp(current.exe, expected->exe);
}
static bool all_stopped(const struct Identity *expected) {
	char path[80];
	if (!same_process(expected)) return false;
	snprintf(path, sizeof(path), "/proc/%ld/task", (long)expected->pid);
	DIR *dir = opendir(path);
	if (!dir) return false;
	struct dirent *entry;
	unsigned count = 0;
	bool stopped = true;
	while ((entry = readdir(dir))) {
		char *end;
		long tid = strtol(entry->d_name, &end, 10);
		if (*end || tid <= 1) continue;
		struct Identity task = {0};
		if (identity((pid_t)tid, &task) || (task.state != 'T' && task.state != 't')) stopped = false;
		++count;
	}
	closedir(dir);
	return stopped && count;
}
static bool udc_unbound(const char *value) { return !*value || !strcmp(value, "none"); }
static bool link_matches(const char *path, const char *expected) {
	struct stat info;
	if (lstat(path, &info) || !S_ISLNK(info.st_mode)) return false;
	char *target = realpath(path, NULL), *wanted = realpath(expected, NULL);
	bool same = target && wanted && !strcmp(target, wanted);
	free(target); free(wanted);
	return same;
}

static int restore(const struct Snapshot *s) {
	char current[128];
	int result = 0;
	int mutex = -1;
	if (*s->restore_lock_path) {
		mutex = open(s->restore_lock_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
		if (mutex < 0) return 1;
		int64_t deadline = now_ms() + 1000;
		while (flock(mutex, LOCK_EX | LOCK_NB)) {
			if (errno != EWOULDBLOCK || now_ms() >= deadline) {
				record(s, "restore_mutex_busy_or_failed", errno); close(mutex); return 1;
			}
			pause_ms(10);
		}
	}
	record(s, "restore_begin", 0);
	if (s->cleanup) {
		bool safe = same_process(&s->adb) && !kill(s->adb.pid, SIGSTOP);
		int64_t deadline = now_ms() + 1000;
		while (safe && !all_stopped(&s->adb) && now_ms() < deadline) pause_ms(10);
		safe = safe && all_stopped(&s->adb);
		if (safe && !read_value(s->udc_path, current, sizeof(current))) {
			if (!udc_unbound(current) && !strcmp(current, s->udc)) {
				(void)write_value(s->udc_path, "\n");
				deadline = now_ms() + 500;
				do {
					if (read_value(s->udc_path, current, sizeof(current))) { safe = false; break; }
					if (udc_unbound(current)) break;
					pause_ms(10);
				} while (now_ms() < deadline);
			}
			safe = safe && udc_unbound(current);
		} else safe = false;
		if (!safe || s->cleanup(s)) { record(s, "owned_function_cleanup_failed", 0); result = 1; }
	}
	if (s->unbind) {
		if (read_value(s->udc_path, current, sizeof(current))) {
			record(s, "restore_udc_unreadable", errno); result = 1;
		} else if (udc_unbound(current)) {
			int rc = write_value(s->udc_path, s->udc);
			record(s, "restore_bind_write", rc ? errno : 0);
			if (read_value(s->udc_path, current, sizeof(current)) || strcmp(current, s->udc)) result = 1;
		} else if (strcmp(current, s->udc)) {
			record(s, "restore_foreign_owner_refused", 0); result = 1;
		}
	}
	/* Resume even if rebind failed. Never strand the exact original daemon.
	 * Its own reopen path may recover USB; that is not labelled a probe pass. */
	if (!same_process(&s->adb) || kill(s->adb.pid, SIGCONT)) {
		record(s, "restore_adbd_identity_or_signal_failed", errno); result = 1;
	} else {
		int64_t deadline = now_ms() + 1000;
		struct Identity current_process = {0};
		bool resumed = false;
		do {
			if (!identity(s->adb.pid, &current_process) && current_process.start == s->adb.start &&
				current_process.state != 'T' && current_process.state != 't') { resumed = true; break; }
			pause_ms(10);
		} while (now_ms() < deadline);
		if (!resumed) result = 1;
	}
	if (read_value(s->udc_path, current, sizeof(current)) || strcmp(current, s->udc)) result = 1;
	if (s->resume_extra && !s->resume_extra(s)) result = 1;
	record(s, result ? "restore_failed_journal_retained" : "restore_verified_local", result);
	if (mutex >= 0) close(mutex);
	return result;
}

static void guardian(const struct Snapshot *s, int command, int answer,
	const struct Identity *worker) {
	interrupted = 0;
	record(s, "guardian_armed", 0);
	char byte = 'A';
	if (write(answer, &byte, 1) != 1) _exit(2);
	struct pollfd descriptor = {command, POLLIN | POLLHUP, 0};
	int64_t deadline = now_ms() + (s->guardian_ms ? s->guardian_ms : 5000);
	bool normal_request = false;
	while (!interrupted && now_ms() < deadline) {
		int rc = poll(&descriptor, 1, 50);
		if (rc < 0 && errno == EINTR) continue;
		if (rc < 0) break;
		if (rc) {
			ssize_t n = read(command, &byte, 1);
			normal_request = n == 1 && byte == 'R';
			record(s, normal_request ? "worker_restore_requested" : "worker_lost", 0);
			break;
		}
	}
	if (!normal_request && same_process(worker)) {
		/* Prevent a late worker from stopping ADB after the guardian resumes it. */
		(void)kill(worker->pid, SIGKILL);
		record(s, "worker_deadline_or_loss", 0);
	}
	int rc = restore(s);
	byte = rc ? '1' : '0';
	(void)write(answer, &byte, 1);
	close(command); close(answer);
	_exit(rc);
}

static int run_probe(const struct Snapshot *s) {
	int commands[2], answers[2];
	if (pipe2(commands, O_CLOEXEC)) return 2;
	if (pipe2(answers, O_CLOEXEC)) { close(commands[0]); close(commands[1]); return 2; }
	struct Identity worker = {0};
	if (identity(getpid(), &worker)) return 2;
	pid_t guard = fork();
	if (guard < 0) return 2;
	if (!guard) {
		close(commands[1]); close(answers[0]);
		guardian(s, commands[0], answers[1], &worker);
	}
	close(commands[0]); close(answers[1]);
	char byte;
	int result = 1;
	struct pollfd descriptor = {answers[0], POLLIN | POLLHUP, 0};
	if (poll(&descriptor, 1, 1000) <= 0 || read(answers[0], &byte, 1) != 1 || byte != 'A')
		goto finish;
	if (!same_process(&s->adb) || kill(s->adb.pid, SIGSTOP)) goto finish;
	record(s, "adbd_stop_requested", 0);
	int64_t deadline = now_ms() + 1000;
	while (!interrupted && !all_stopped(&s->adb) && now_ms() < deadline) pause_ms(10);
	if (!all_stopped(&s->adb)) { record(s, "adbd_not_fully_stopped", 0); goto finish; }
	record(s, "adbd_all_threads_stopped", 0);
	if (s->before_unbind && !s->before_unbind(s)) {
		record(s, "extra_takeover_refused", 0); goto finish;
	}
	if (s->fault == WORKER_AFTER_STOP) (void)kill(getpid(), SIGKILL);
	if (s->fault == GUARD_AFTER_STOP) { (void)kill(guard, SIGKILL); goto finish; }
	if (s->unbind) {
		int rc = write_value(s->udc_path, "\n");
		record(s, "unbind_write", rc ? errno : 0);
		char value[128];
		deadline = now_ms() + 500;
		bool empty = false;
		do {
			if (read_value(s->udc_path, value, sizeof(value))) break;
			if (udc_unbound(value)) { empty = true; break; }
			pause_ms(10);
		} while (!interrupted && now_ms() < deadline);
		if (!empty) { record(s, "unbind_not_verified", 0); goto finish; }
		record(s, "unbind_verified", 0);
		if (s->fault == WORKER_AFTER_UNBIND) (void)kill(getpid(), SIGKILL);
		if (s->fault == GUARD_AFTER_UNBIND) { (void)kill(guard, SIGKILL); goto finish; }
	}
	if (s->activate && !s->activate(s)) { record(s, "activation_failed", 0); goto finish; }
	deadline = now_ms() + s->hold_ms;
	result = 0;
	unsigned observations = 0;
	while (!interrupted && now_ms() < deadline) {
		if (!all_stopped(&s->adb)) { result = 1; break; }
		if (s->unbind) {
			char value[128];
			if (read_value(s->udc_path, value, sizeof(value)) ||
				(s->activate ? strcmp(value, s->udc) != 0 : !udc_unbound(value))) {
				record(s, "unexpected_rebind_while_adbd_stopped", 0); result = 1; break;
			}
		}
		if (s->step) {
			int rc = s->step(s);
			if (rc) { result = rc == 2 ? 0 : 1; break; }
		}
		++observations;
		pause_ms(10);
	}
	if (interrupted) result = 1;
	record(s, "hold_finished_observations", (int)observations);
finish:
	if (s->worker_release) s->worker_release(s);
	byte = 'R';
	bool restored = false, answer_received = false;
	if (write(commands[1], &byte, 1) == 1 && poll(&descriptor, 1, 3000) > 0 &&
		read(answers[0], &byte, 1) == 1) { answer_received = true; restored = byte == '0'; }
	if (!restored) {
		record(s, answer_received ? "guardian_restore_failed_fallback" : "guardian_unavailable_fallback", 0);
		result = 1;
		(void)restore(s);
	}
	close(commands[1]); close(answers[0]);
	int status;
	while (waitpid(guard, &status, 0) < 0 && errno == EINTR) {}
	record(s, "probe_finished", result);
	return result;
}

static bool fixture_activate(const struct Snapshot *s) {
	return !write_value(s->udc_path, s->udc);
}
static int fixture_step(const struct Snapshot *s) { (void)s; return 2; }
static int fixture_cleanup(const struct Snapshot *s) {
	char value[128];
	if (read_value(s->udc_path, value, sizeof(value)) || !udc_unbound(value)) return 1;
	record(s, "fixture_cleanup_saw_unbound", 0);
	return 0;
}
static int self_test(void) {
	char directory[160];
	snprintf(directory, sizeof(directory), "/tmp/nx-usb-owner-selftest.%ld", (long)getpid());
	if (mkdir(directory, 0700)) return 2;
	struct Snapshot s = {.log_fd = STDOUT_FILENO, .hold_ms = 60};
	snprintf(s.udc_path, sizeof(s.udc_path), "%s/UDC", directory);
	snprintf(s.udc, sizeof(s.udc), "fixture.udc");
	int fd = open(s.udc_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (fd < 0) return 2;
	close(fd);
	pid_t target = fork();
	if (target < 0) return 2;
	if (!target) { for (;;) pause(); }
	if (identity(target, &s.adb)) return 2;
	unsigned failed = 0;
	for (unsigned test = 0; test < 9; ++test) {
		s.unbind = test != 0;
		s.fault = test < 2 || test == 6 ? NO_FAULT :
			(test < 6 ? (enum Fault)(test - 1) : test == 7 ? WORKER_AFTER_UNBIND : GUARD_AFTER_UNBIND);
		s.activate = test < 6 ? NULL : fixture_activate;
		s.step = test < 6 ? NULL : fixture_step;
		s.cleanup = test < 6 ? NULL : fixture_cleanup;
		(void)write_value(s.udc_path, s.udc);
		pid_t worker = fork();
		if (worker < 0) { ++failed; continue; }
		if (!worker) _exit(run_probe(&s));
		int status;
		(void)waitpid(worker, &status, 0);
		/* A killed worker's guardian is now reparented; allow its restore to finish. */
		pause_ms(200);
		char value[128];
		struct Identity after = {0};
		bool safe = !read_value(s.udc_path, value, sizeof(value)) && !strcmp(value, s.udc) &&
			!identity(target, &after) && after.start == s.adb.start && after.state != 'T' && after.state != 't';
		bool expected = s.fault == NO_FAULT ? WIFEXITED(status) && !WEXITSTATUS(status) :
			(s.fault == WORKER_AFTER_STOP || s.fault == WORKER_AFTER_UNBIND ?
				WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL : WIFEXITED(status) && WEXITSTATUS(status) == 1);
		printf("SELFTEST case=%u safe=%d expected_exit=%d\n", test, safe, expected);
		if (!safe || !expected) ++failed;
	}
	struct Identity wrong = s.adb;
	++wrong.start;
	if (same_process(&wrong) || !same_process(&s.adb)) ++failed;
	char link[512];
	snprintf(link, sizeof(link), "%s/link", directory);
	bool absolute = !symlink(s.udc_path, link) && link_matches(link, s.udc_path);
	(void)unlink(link);
	bool relative = !symlink("UDC", link) && link_matches(link, s.udc_path);
	bool foreign = !link_matches(link, directory) && !link_matches(s.udc_path, s.udc_path);
	(void)unlink(link);
	printf("LINKTEST absolute=%d relative=%d foreign_or_regular_refused=%d\n", absolute, relative, foreign);
	if (!absolute || !relative || !foreign) ++failed;
	(void)kill(target, SIGCONT);
	(void)kill(target, SIGKILL);
	(void)waitpid(target, NULL, 0);
	(void)unlink(s.udc_path);
	(void)rmdir(directory);
	printf("SELFTEST failed=%u (fixture files and own child processes only)\n", failed);
	return failed ? 1 : 0;
}

static int discover_adbd(struct Identity *out) {
	DIR *dir = opendir("/proc");
	if (!dir) return -1;
	struct dirent *entry;
	unsigned matches = 0;
	while ((entry = readdir(dir))) {
		char *end;
		long pid = strtol(entry->d_name, &end, 10);
		if (*end || pid <= 1) continue;
		struct Identity candidate = {0};
		if (!identity((pid_t)pid, &candidate) && !strcmp(candidate.exe, "/bin/adbd")) {
			*out = candidate; ++matches;
		}
	}
	closedir(dir);
	return matches == 1 && out->state != 'T' && out->state != 't' ? 0 : -1;
}

int main(int argc, char **argv) {
	(void)signal(SIGHUP, SIG_IGN);
	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGTERM, request_stop);
	(void)signal(SIGINT, request_stop);
	if (argc == 2 && !strcmp(argv[1], "--selftest")) return self_test();
	struct Snapshot s = {.log_fd = -1, .hold_ms = 300};
	if (argc != 3 || (strcmp(argv[1], "--pause-only") && strcmp(argv[1], "--unbind"))) {
		fprintf(stderr, "usage: %s --selftest | (--pause-only|--unbind) /tmp/nx-usb-owner-NAME.log\n", argv[0]);
		return 2;
	}
	s.unbind = !strcmp(argv[1], "--unbind");
	const char *basename = strrchr(argv[2], '/');
	if (strncmp(argv[2], "/tmp/nx-usb-owner-", 18) || !basename || strchr(argv[2] + 18, '/') ||
		strstr(argv[2], "..") || !argv[2][18]) return 2;
	if (geteuid() != 0) return 2;
	int lock = open("/tmp/nx-usb-owner-probe.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) return 2;
	if (!access("/tmp/nx_usb_mode.lock", F_OK) || !access("/tmp/nx-h2-usb-out.active", F_OK)) return 2;
	char role[64];
	if (read_value("/sys/devices/platform/soc/usbc0/otg_role", role, sizeof(role)) ||
		strcmp(role, "usb_device") || discover_adbd(&s.adb)) return 2;
	/* Reject mass storage even if not bound: an open LUN must not be touched. */
	DIR *functions = opendir("/sys/kernel/config/usb_gadget/g1/functions");
	if (!functions) return 2;
	struct dirent *entry;
	bool storage = false;
	while ((entry = readdir(functions))) if (!strncmp(entry->d_name, "mass_storage.", 13)) storage = true;
	closedir(functions);
	if (storage) return 2;
	snprintf(s.udc_path, sizeof(s.udc_path), "/sys/kernel/config/usb_gadget/g1/UDC");
	if (read_value(s.udc_path, s.udc, sizeof(s.udc)) || udc_unbound(s.udc)) return 2;
	s.log_fd = open(argv[2], O_CREAT | O_EXCL | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
	if (s.log_fd < 0) return 2;
	dprintf(s.log_fd, "snapshot_adbd_pid=%ld start=%llu exe=%s original_udc=%s unbind=%d\n",
		(long)s.adb.pid, s.adb.start, s.adb.exe, s.udc, s.unbind);
	record(&s, "preflight_passed", 0);
	int rc = run_probe(&s);
	close(s.log_fd); close(lock);
	return rc;
}
