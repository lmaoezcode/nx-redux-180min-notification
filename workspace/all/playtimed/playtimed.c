#define _GNU_SOURCE
#include "daily.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// How often the ledger is checkpointed to the SD card while a game runs. Each
// checkpoint is a write+fsync+rename on exFAT, so keep it sparse: a crash or
// sudden power loss costs at most this much counted play time (a clean exit,
// shutdown, new day or reminder always saves immediately).
#define CHECKPOINT_INTERVAL_MS 120000

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t paused;
static char pid_path[4096], toast_path[4096], suspend_path[4096], osd_menu_path[4096];

static void on_signal(int sig) {
	if (sig == SIGUSR1) paused = 1;
	else if (sig == SIGUSR2) paused = 0;
	else stopping = 1;
}

static bool join(char* out, size_t size, const char* directory, const char* name) {
	int n = snprintf(out, size, "%s/%s", directory, name);
	return n >= 0 && (size_t)n < size;
}

static uint64_t clock_ms(clockid_t clock) {
	struct timespec t;
	if (clock_gettime(clock, &t) != 0) return 0;
	return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int civil_day(uint64_t* since_midnight_ms) {
	struct timespec t;
	struct tm local;
	if (clock_gettime(CLOCK_REALTIME, &t) || !localtime_r(&t.tv_sec, &local)) return 0;
	*since_midnight_ms = ((uint64_t)local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec)
		* 1000 + t.tv_nsec / 1000000;
	return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

static bool write_all(int fd, const char* data, size_t size) {
	while (size) {
		ssize_t n = write(fd, data, size);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return false;
		data += n;
		size -= (size_t)n;
	}
	return true;
}

// Every checkpoint is a small write+fsync+rename, with no SD descriptor kept open.
static bool atomic_write(const char* path, const char* data, bool no_replace) {
	char temporary[4096];
	int n = snprintf(temporary, sizeof(temporary), "%s.new.XXXXXX", path);
	if (n < 0 || (size_t)n >= sizeof(temporary)) return false;
	int fd = mkstemp(temporary);
	if (fd < 0) return false;
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	bool ok = write_all(fd, data, strlen(data)) && fsync(fd) == 0;
	if (close(fd) != 0) ok = false;
	if (ok) {
		// Toast IPC is on tmpfs. link() publishes a complete JSON without ever
		// replacing a pending system/volume toast written by another process.
		ok = no_replace ? link(temporary, path) == 0 : rename(temporary, path) == 0;
	}
	unlink(temporary);
	return ok;
}

static bool save(const char* path, const DailyPlayTime* state) {
	char record[160];
	int n = DailyPlayTime_format(state, record, sizeof(record));
	if (n < 0 || (size_t)n >= sizeof(record) || !atomic_write(path, record, false)) {
		fprintf(stderr, "playtimed: checkpoint failed (%s)\n", strerror(errno));
		return false;
	}
	// Flush the rename too, where the SD filesystem supports directory fsync.
	char directory[4096];
	if (strlen(path) < sizeof(directory)) {
		strcpy(directory, path);
		char* slash = strrchr(directory, '/');
		if (slash) {
			*slash = 0;
			int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			if (fd >= 0) { fsync(fd); close(fd); }
		}
	}
	return true;
}

static bool load(const char* path, DailyPlayTime* state) {
	FILE* file = fopen(path, "r");
	if (!file) return errno == ENOENT;
	char record[160];
	size_t n = fread(record, 1, sizeof(record) - 1, file);
	bool ok = !ferror(file) && feof(file);
	record[n] = 0;
	fclose(file);
	// Fail open for the GAME, but never overwrite an invalid/unknown ledger.
	return ok && DailyPlayTime_parse(state, record);
}

static int clamp(int value, int low, int high) {
	return value < low ? low : value > high ? high : value;
}

static DailyPlayTimeConfig read_config(const char* path) {
	DailyPlayTimeConfig config = {false, 60, 60, 5};
	FILE* file = fopen(path, "r");
	if (!file) return config;
	char line[512];
	int value;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "playTimeReminderEnabled=%d", &value) == 1)
			config.enabled = value != 0;
		else if (sscanf(line, "playTimeFirstMinutes=%d", &value) == 1)
			config.first_minutes = clamp(value, 1, 1440);
		else if (sscanf(line, "playTimeRepeatMinutes=%d", &value) == 1)
			config.repeat_minutes = clamp(value, 0, 1440);
		else if (sscanf(line, "playTimeNotificationDuration=%d", &value) == 1)
			config.duration_seconds = clamp(value, 1, 30);
	}
	fclose(file);
	return config;
}

static bool osd_running(void) {
#ifdef PLAYTIMED_TEST
	return true; // Integration tests use an isolated fake toast consumer.
#else
	DIR* directory = opendir("/proc");
	if (!directory) return false;
	struct dirent* entry;
	bool found = false;
	while ((entry = readdir(directory))) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
		char path[512], name[32];
		snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
		FILE* file = fopen(path, "r");
		if (!file) continue;
		found = fgets(name, sizeof(name), file) && !strncmp(name, "trimui_osdd", 11);
		fclose(file);
		if (found) break;
	}
	closedir(directory);
	return found;
#endif
}

static bool notify(const DailyPlayTime* state, const DailyPlayTimeConfig* config) {
	if (access(toast_path, F_OK) == 0 || !osd_running()) return false;
	int width = 1024, height = 768;
	const char* device = getenv("DEVICE");
	if (device && !strcmp(device, "smartpro")) { width = 1280; height = 720; }
	FILE* fb = fopen("/sys/class/graphics/fb0/virtual_size", "r");
	if (fb) {
		int fb_width;
		if (fscanf(fb, "%d", &fb_width) == 1 && fb_width >= 640 && fb_width <= 1920)
			width = fb_width;
		fclose(fb);
	}
	char message[128], json[768];
	DailyPlayTime_message(state, message, sizeof(message));
	int n = snprintf(json, sizeof(json),
		"{\"type\":\"default\",\"id\":\"com.nxredux.osd.playtime\","
		"\"duration\":%d,\"size\":2,\"x\":%d,\"y\":%d,\"w\":600,\"h\":80,"
		"\"message\":\"%s\",\"font\":\"\",\"bg\":\"\",\"icon\":\"\","
		"\"fontsize\":24,\"fontcolor\":\"FFFFFFFF\"}\n",
		config->duration_seconds * 1000, (width - 600) / 2, height - 218, message);
	return n >= 0 && (size_t)n < sizeof(json) && atomic_write(toast_path, json, true);
}

static bool active_now(void) {
	return !paused && access(suspend_path, F_OK) != 0 && access(osd_menu_path, F_OK) != 0;
}

static int classify(const char* file_path) {
	FILE* file = fopen(file_path, "r");
	if (!file) return 1;
	char command[32768];
	size_t n = fread(command, 1, sizeof(command) - 1, file);
	bool complete = !ferror(file) && feof(file);
	fclose(file);
	command[n] = 0;
	const char* sdcard = getenv("SDCARD_PATH");
	return complete && DailyPlayTime_gameCommand(command, sdcard ? sdcard : "/mnt/SDCARD") ? 0 : 1;
}

int main(int argc, char** argv) {
	if (argc == 3 && !strcmp(argv[1], "--is-game")) return classify(argv[2]);
	if (argc != 3 || strcmp(argv[1], "--run")) return 2;
	char* end;
	long parent = strtol(argv[2], &end, 10);
	if (*end || parent <= 1 || parent > 0x7fffffffL || getppid() != (pid_t)parent) return 2;
	const char* shared = getenv("SHARED_USERDATA_PATH");
	if (!shared || !shared[0]) shared = "/mnt/SDCARD/.userdata/shared";
	const char* temporary = "/tmp";
#ifdef PLAYTIMED_TEST
	temporary = getenv("NX_PLAYTIME_TEST_TMP");
	if (!temporary || !temporary[0]) return 2; // Never touch production /tmp in tests.
#endif
	char state_path[4096], config_path[4096], lock_path[4096], poweroff_path[4096], reboot_path[4096];
	if (!join(state_path, sizeof(state_path), shared, "daily-playtime.txt") ||
		!join(config_path, sizeof(config_path), shared, "minuisettings.txt") ||
		!join(pid_path, sizeof(pid_path), temporary, "nx-playtime.pid") ||
		!join(lock_path, sizeof(lock_path), temporary, "nx-playtime.lock") ||
		!join(toast_path, sizeof(toast_path), temporary, "trimui_osd/osd_toast_msg") ||
		!join(suspend_path, sizeof(suspend_path), temporary, "system_suspend") ||
		!join(poweroff_path, sizeof(poweroff_path), temporary, "poweroff") ||
		!join(reboot_path, sizeof(reboot_path), temporary, "reboot") ||
		!join(osd_menu_path, sizeof(osd_menu_path), temporary, "trimui_osd/osdd_show_up")) return 2;
	int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
		if (lock >= 0) close(lock);
		return 1;
	}
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = on_signal;
	sigemptyset(&action.sa_mask);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	sigaction(SIGUSR1, &action, NULL);
	sigaction(SIGUSR2, &action, NULL);
	DailyPlayTime state = {0};
	uint64_t after_midnight = 0;
	if (!load(state_path, &state)) {
		fprintf(stderr, "playtimed: invalid/unreadable ledger; left untouched\n");
		close(lock);
		return 1;
	}
	int initial_date = civil_day(&after_midnight);
	DailyPlayTime_tick(&state, initial_date, after_midnight, 0, false);
	if (!state.date || !save(state_path, &state)) { close(lock); return 1; }
	char pid_record[32];
	snprintf(pid_record, sizeof(pid_record), "%ld\n", (long)getpid());
	if (!atomic_write(pid_path, pid_record, false)) { close(lock); return 1; }
	DailyPlayTimeConfig config = read_config(config_path);
	uint64_t previous = clock_ms(CLOCK_MONOTONIC), previous_boot = clock_ms(CLOCK_BOOTTIME);
	uint64_t last_save = previous, last_config = previous;
	bool was_active = active_now();
	for (;;) {
		struct timespec delay = {1, 0};
		nanosleep(&delay, NULL); // Pause/stop signals interrupt this promptly.
		uint64_t now = clock_ms(CLOCK_MONOTONIC), boot = clock_ms(CLOCK_BOOTTIME);
		uint64_t delta = now >= previous ? now - previous : 0;
		bool awake = !(boot >= previous_boot && boot - previous_boot > delta + 250);
		bool active = active_now();
		int date = civil_day(&after_midnight);
		bool new_day = DailyPlayTime_tick(&state, date, after_midnight, delta,
			was_active && active && awake);
		previous = now;
		previous_boot = boot;
		was_active = active;
		bool exiting = stopping || getppid() != (pid_t)parent ||
			access(poweroff_path, F_OK) == 0 || access(reboot_path, F_OK) == 0;
		if (now - last_config >= 15000) { config = read_config(config_path); last_config = now; }
		bool notified = !exiting && active && awake && DailyPlayTime_due(&state, &config) &&
			notify(&state, &config);
		if (notified) DailyPlayTime_notified(&state);
		if (new_day || notified || exiting || now - last_save >= CHECKPOINT_INTERVAL_MS) {
			save(state_path, &state);
			last_save = now; // Bound retries/logging even if the card became read-only.
		}
		if (exiting) break;
	}
	unlink(pid_path); // Runtime lock ensures only this daemon can own the marker.
	close(lock);
	return 0;
}
