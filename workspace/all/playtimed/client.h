#ifndef NX_DAILY_PLAY_TIME_CLIENT_H
#define NX_DAILY_PLAY_TIME_CLIENT_H

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The supervisor exports this ONLY after the daemon's ready handshake.
// This is opt-in: old launchers retain minarch's original per-session fallback.
static inline pid_t DailyPlayTime_clientPid(void) {
	const char* value = getenv("NX_PLAYTIME_PID");
	char* end;
	if (!value || !*value)
		return 0;
	long pid = strtol(value, &end, 10);
	return !*end && pid > 1 && pid <= 0x7fffffffL ? (pid_t)pid : 0;
}

static inline int DailyPlayTime_clientActive(void) {
	pid_t pid = DailyPlayTime_clientPid();
	return pid > 1 && (kill(pid, 0) == 0 || errno == EPERM);
}

static inline void DailyPlayTime_clientPause(int paused) {
	pid_t pid = DailyPlayTime_clientPid();
	if (pid > 1)
		kill(pid, paused ? SIGUSR1 : SIGUSR2);
}

// ---- in-game delivery -------------------------------------------------------
// While the helper runs, minarch registers its PID in <tmp>/nx-playtime-inline.
// The helper then hands the reminder text over in <tmp>/nx-playtime-due instead
// of raising the system toast; minarch shows it as its own notification.
static inline int DailyPlayTime_clientPath(char* out, size_t size, const char* name) {
	const char* tmp = getenv("NX_PLAYTIME_TMP");
	int n = snprintf(out, size, "%s/%s", tmp && *tmp ? tmp : "/tmp", name);
	return n > 0 && (size_t)n < size;
}

static inline void DailyPlayTime_clientAnnounceInline(void) {
	char path[4096], record[32];
	if (!DailyPlayTime_clientPath(path, sizeof(path), "nx-playtime-inline")) return;
	int n = snprintf(record, sizeof(record), "%ld\n", (long)getpid());
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) return;
	if (n > 0 && write(fd, record, (size_t)n) != (ssize_t)n) { /* helper falls back to the toast */ }
	close(fd);
}

static inline void DailyPlayTime_clientWithdrawInline(void) {
	char path[4096];
	if (DailyPlayTime_clientPath(path, sizeof(path), "nx-playtime-inline")) unlink(path);
}

// Returns 1 and fills out with a reminder if one is waiting (and removes it).
static inline int DailyPlayTime_clientTakeDue(char* out, size_t size) {
	char path[4096];
	if (!out || size < 2 || !DailyPlayTime_clientPath(path, sizeof(path), "nx-playtime-due")) return 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return 0;
	ssize_t n = read(fd, out, size - 1);
	close(fd);
	unlink(path);
	if (n <= 0) return 0;
	out[n] = 0;
	char* end = strpbrk(out, "\r\n");
	if (end) *end = 0;
	return out[0] != 0;
}

// Returns 1 once when the helper asks the game to close because the daily limit
// ran out (the file is removed). The client saves and quits through its normal
// route; the helper takes the process down itself if it does not.
static inline int DailyPlayTime_clientTakeQuit(void) {
	char path[4096];
	if (!DailyPlayTime_clientPath(path, sizeof(path), "nx-playtime-quit")) return 0;
	if (access(path, F_OK) != 0) return 0;
	unlink(path);
	return 1;
}

#endif
