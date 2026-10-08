#ifndef NX_DAILY_PLAY_TIME_CLIENT_H
#define NX_DAILY_PLAY_TIME_CLIENT_H

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
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

#endif
