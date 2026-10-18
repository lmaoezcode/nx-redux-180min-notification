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
static char inline_path[4096], due_path[4096];
static char simple_path[4096], extra_path[4096], quit_path[4096];

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

// ---- Hard daily limit (a Simple Mode setting) ---------------------------------
// Active only when Simple Mode is on WITH a 4-digit PIN (the same file the
// launcher checks) and playTimeLimitMinutes is above 0. Anything unreadable
// leaves the limit off: a broken helper must never lock the device.
static bool simple_mode_has_pin(const char* path) {
	FILE* file = fopen(path, "r");
	if (!file) return false;
	char pin[16] = {0};
	size_t n = fread(pin, 1, sizeof(pin) - 1, file);
	fclose(file);
	while (n > 0 && (pin[n - 1] == '\n' || pin[n - 1] == '\r' || pin[n - 1] == ' ' || pin[n - 1] == '\t'))
		pin[--n] = 0;
	if (n != 4) return false;
	for (size_t i = 0; i < 4; i++)
		if (pin[i] < '0' || pin[i] > '9') return false;
	return true;
}

static DailyLimitConfig read_limit_config(const char* config_file, const char* simple_file) {
	DailyLimitConfig limit = {false, 0, 5, 180, 15, 3};
	FILE* file = fopen(config_file, "r");
	if (!file) return limit;
	char line[512];
	int value;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "playTimeLimitMinutes=%d", &value) == 1)
			limit.limit_minutes = clamp(value, 0, 1440);
		else if (sscanf(line, "playTimeLimitWarnMinutes=%d", &value) == 1)
			limit.warn_minutes = clamp(value, 1, 60);
		else if (sscanf(line, "playTimeLimitGraceSeconds=%d", &value) == 1)
			limit.grace_seconds = clamp(value, 0, 900);
		else if (sscanf(line, "playTimeLimitExtraMinutes=%d", &value) == 1)
			limit.extra_minutes = clamp(value, 0, 480);
		else if (sscanf(line, "playTimeLimitExtraMax=%d", &value) == 1)
			limit.extra_max = clamp(value, 0, 99);
	}
	fclose(file);
	limit.enabled = limit.limit_minutes > 0 && simple_mode_has_pin(simple_file);
	return limit;
}

// A missing or unreadable record means no unlocks today (stricter, never looser).
static DailyExtra load_extra(const char* path) {
	DailyExtra extra = {0, 0};
	FILE* file = fopen(path, "r");
	if (!file) return extra;
	char record[96];
	size_t n = fread(record, 1, sizeof(record) - 1, file);
	record[n] = 0;
	fclose(file);
	if (!DailyExtra_parse(&extra, record)) extra = (DailyExtra){0, 0};
	return extra;
}

static bool shared_paths(char* state, char* config, char* simple, char* extra) {
	const char* shared = getenv("SHARED_USERDATA_PATH");
	if (!shared || !shared[0]) shared = "/mnt/SDCARD/.userdata/shared";
	return join(state, 4096, shared, "daily-playtime.txt") &&
		join(config, 4096, shared, "minuisettings.txt") &&
		join(simple, 4096, shared, "enable-simple-mode") &&
		join(extra, 4096, shared, "daily-playtime-extra.txt");
}

// Everything below the supervisor that was started AFTER this helper began is
// "the game": snapshot the supervisor's children now, so the keymon-style
// background jobs it started earlier are never touched.
#define MAX_PIDS 4096
static pid_t survivors[256];
static int survivor_count;

static int read_ppid(pid_t pid) {
	char path[64], buffer[1024];
	snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
	FILE* file = fopen(path, "r");
	if (!file) return -1;
	size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
	fclose(file);
	buffer[n] = 0;
	char* close_paren = strrchr(buffer, ')'); // the command name may hold spaces
	int ppid = -1;
	char state;
	if (!close_paren || sscanf(close_paren + 1, " %c %d", &state, &ppid) != 2) return -1;
	return ppid;
}

static int list_processes(pid_t* pids, int* ppids, int max) {
	DIR* directory = opendir("/proc");
	if (!directory) return 0;
	int count = 0;
	struct dirent* entry;
	while (count < max && (entry = readdir(directory))) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
		pid_t pid = (pid_t)atol(entry->d_name);
		int ppid = read_ppid(pid);
		if (ppid < 0) continue;
		pids[count] = pid;
		ppids[count++] = ppid;
	}
	closedir(directory);
	return count;
}

static void snapshot_survivors(pid_t parent) {
	static pid_t pids[MAX_PIDS];
	static int ppids[MAX_PIDS];
	int count = list_processes(pids, ppids, MAX_PIDS);
	survivor_count = 0;
	for (int i = 0; i < count && survivor_count < 256; i++)
		if (ppids[i] == (int)parent) survivors[survivor_count++] = pids[i];
}

static bool is_survivor(pid_t pid) {
	for (int i = 0; i < survivor_count; i++)
		if (survivors[i] == pid) return true;
	return false;
}

// Sends `sig` to the game's whole process tree. Returns how many were signalled.
static int signal_game(pid_t parent, int sig) {
	static pid_t pids[MAX_PIDS];
	static int ppids[MAX_PIDS];
	static bool marked[MAX_PIDS];
	int count = list_processes(pids, ppids, MAX_PIDS);
	for (int i = 0; i < count; i++)
		marked[i] = ppids[i] == (int)parent && pids[i] != getpid() && !is_survivor(pids[i]);
	for (bool changed = true; changed;) { // descendants of a marked process
		changed = false;
		for (int i = 0; i < count; i++) {
			if (marked[i] || pids[i] == getpid()) continue;
			for (int j = 0; j < count; j++)
				if (marked[j] && pids[j] == (pid_t)ppids[i]) { marked[i] = true; changed = true; break; }
		}
	}
	int signalled = 0;
	for (int i = 0; i < count; i++)
		if (marked[i] && kill(pids[i], sig) == 0) signalled++;
	return signalled;
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

// minarch registers its PID here while it runs under this helper, so a libretro
// game can show the reminder in NX Redux's own in-game notification style. Ports
// and anything else keep the system toast. A dead or missing client falls back.
static bool inline_client_alive(void) {
	FILE* file = fopen(inline_path, "r");
	if (!file) return false;
	long pid = 0;
	bool read = fscanf(file, "%ld", &pid) == 1;
	fclose(file);
	return read && pid > 1 && pid <= 0x7fffffffL && (kill((pid_t)pid, 0) == 0 || errno == EPERM);
}

static bool notify_text(const char* text, int duration_seconds) {
	if (inline_client_alive()) {
		if (access(due_path, F_OK) == 0) return false; // previous reminder not consumed yet
		char line[160];
		int length = snprintf(line, sizeof(line), "%s\n", text);
		return length > 0 && (size_t)length < sizeof(line) && atomic_write(due_path, line, true);
	}
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
	char json[768];
	const char* message = text;
	int n = snprintf(json, sizeof(json),
		"{\"type\":\"default\",\"id\":\"com.nxredux.osd.playtime\","
		"\"duration\":%d,\"size\":2,\"x\":%d,\"y\":%d,\"w\":600,\"h\":80,"
		"\"message\":\"%s\",\"font\":\"\",\"bg\":\"\",\"icon\":\"\","
		"\"fontsize\":24,\"fontcolor\":\"FFFFFFFF\"}\n",
		duration_seconds * 1000, (width - 600) / 2, height - 218, message);
	return n >= 0 && (size_t)n < sizeof(json) && atomic_write(toast_path, json, true);
}

static bool notify(const DailyPlayTime* state, const DailyPlayTimeConfig* config) {
	char text[128];
	DailyPlayTime_message(state, text, sizeof(text));
	return notify_text(text, config->duration_seconds);
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

// ---- Command-line modes used by the supervisor and nextui ----------------------

// What the limit looks like right now. ok=false means "treat the limit as off".
typedef struct {
	bool ok;
	DailyLimitConfig limit;
	DailyExtra extra;
	int today;
	DailyPlayTime state; // already moved to `today`: another day counts as nothing played
} LimitView;

static LimitView limit_view(void) {
	LimitView view = {0};
	char state_file[4096], config_file[4096], simple_file[4096], extra_file[4096];
	if (!shared_paths(state_file, config_file, simple_file, extra_file)) return view;
	view.limit = read_limit_config(config_file, simple_file);
	uint64_t after_midnight = 0;
	view.today = civil_day(&after_midnight);
	if (!view.today) return view;
	if (!load(state_file, &view.state)) return view; // unreadable ledger: fail open
	if (view.state.date != view.today) view.state = (DailyPlayTime){.date = view.today};
	view.extra = load_extra(extra_file);
	view.ok = true;
	return view;
}

// Exit 0 = start the game, 3 = blocked. Anything that is not a game (tools,
// Settings, USB), a disabled limit or any error all mean 0.
static int check_mode(const char* next_file) {
	if (classify(next_file) != 0) return 0;
	LimitView view = limit_view();
	if (!view.ok || !view.limit.enabled) return 0;
	return DailyLimit_remainingMs(&view.state, &view.limit, &view.extra, view.today) <= 0 ? 3 : 0;
}

// One line for nextui: "limit=120 used=125 left=-5 grants=1 max=3 extra=15 can_grant=1" (minutes).
static int status_mode(void) {
	LimitView view = limit_view();
	if (!view.ok || !view.limit.enabled) { puts("enabled=0"); return 0; }
	long long left = DailyLimit_remainingMs(&view.state, &view.limit, &view.extra, view.today);
	left = left >= 0 ? (left + 59999) / 60000 : -((-left) / 60000); // round the remaining time up
	printf("enabled=1 limit=%d used=%llu left=%lld grants=%d max=%d extra=%d can_grant=%d\n",
		view.limit.limit_minutes, (unsigned long long)(view.state.elapsed_ms / DAILY_MINUTE_MS), left,
		view.extra.date == view.today ? view.extra.count : 0, view.limit.extra_max,
		view.limit.extra_minutes, DailyLimit_canGrant(&view.limit, &view.extra, view.today) ? 1 : 0);
	return 0;
}

// nextui calls this AFTER it verified the PIN. 0 = granted, 4 = daily cap reached,
// 5 = no limit in force, 1 = could not record it.
static int grant_mode(void) {
	LimitView view = limit_view();
	if (!view.ok || !view.limit.enabled) return 5;
	if (!DailyLimit_canGrant(&view.limit, &view.extra, view.today)) return 4;
	char state_file[4096], config_file[4096], simple_file[4096], extra_file[4096], record[96];
	if (!shared_paths(state_file, config_file, simple_file, extra_file)) return 1;
	DailyLimit_grant(&view.extra, view.today);
	if (DailyExtra_format(&view.extra, record, sizeof(record)) < 0 || !atomic_write(extra_file, record, false)) return 1;
	printf("granted=%d minutes=%d\n", view.extra.count, view.limit.extra_minutes);
	return 0;
}

int main(int argc, char** argv) {
	if (argc == 3 && !strcmp(argv[1], "--is-game")) return classify(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "--check")) return check_mode(argv[2]);
	if (argc == 2 && !strcmp(argv[1], "--status")) return status_mode();
	if (argc == 2 && !strcmp(argv[1], "--grant")) return grant_mode();
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
		!join(osd_menu_path, sizeof(osd_menu_path), temporary, "trimui_osd/osdd_show_up") ||
		!join(inline_path, sizeof(inline_path), temporary, "nx-playtime-inline") ||
		!join(due_path, sizeof(due_path), temporary, "nx-playtime-due") ||
		!join(quit_path, sizeof(quit_path), temporary, "nx-playtime-quit") ||
		!join(simple_path, sizeof(simple_path), shared, "enable-simple-mode") ||
		!join(extra_path, sizeof(extra_path), shared, "daily-playtime-extra.txt")) return 2;
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
	unlink(due_path); // a reminder left over from a previous session must not replay
	unlink(quit_path); // and neither must a leftover quit request
	snapshot_survivors((pid_t)parent);
	char pid_record[32];
	snprintf(pid_record, sizeof(pid_record), "%ld\n", (long)getpid());
	if (!atomic_write(pid_path, pid_record, false)) { close(lock); return 1; }
	DailyPlayTimeConfig config = read_config(config_path);
	DailyLimitConfig limit = read_limit_config(config_path, simple_path);
	DailyExtra extra = load_extra(extra_path);
	DailyLimitPhase shown_phase = DAILY_LIMIT_OK; // highest limit message delivered so far
	bool closing = false, closing_inline = false, killed_hard = false;
	uint64_t closing_at = 0;
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
		if (now - last_config >= 15000) {
			config = read_config(config_path);
			limit = read_limit_config(config_path, simple_path);
			extra = load_extra(extra_path);
			last_config = now;
		}
		bool notified = !exiting && active && awake && DailyPlayTime_due(&state, &config) &&
			notify(&state, &config);
		if (notified) DailyPlayTime_notified(&state);

		// Hard limit. Elapsed time only grows while the game is really being played,
		// so the grace period is play time too: a quick menu or sleep pauses it.
		if (!exiting && limit.enabled) {
			DailyLimitPhase phase = DailyLimit_phase(&state, &limit, &extra, date);
			if (phase < shown_phase) shown_phase = phase; // more time was granted: warn again later
			if (phase == DAILY_LIMIT_OK) {
				closing = false;
				unlink(quit_path);
			} else if (active && awake) {
				int64_t left = DailyLimit_remainingMs(&state, &limit, &extra, date);
				if (phase > shown_phase && phase != DAILY_LIMIT_EXPIRED) {
					char text[128];
					DailyLimit_message(phase, left, limit.grace_seconds, text, sizeof(text));
					if (notify_text(text, config.duration_seconds < 8 ? 8 : config.duration_seconds))
						shown_phase = phase;
				}
				if (phase == DAILY_LIMIT_EXPIRED && !closing) {
					closing = true;
					closing_at = now;
					closing_inline = inline_client_alive();
					if (closing_inline) {
						// minarch saves its auto-resume state and quits cleanly.
						atomic_write(quit_path, "QUIT\n", false);
					} else {
						signal_game((pid_t)parent, SIGTERM);
					}
				} else if (closing && !killed_hard && now - closing_at >= (closing_inline ? 20000u : 15000u)) {
					// Still running: ports ignore SIGTERM sometimes; a hung minarch is taken down
					// with SIGTERM (its power-off path saves first), never SIGKILL.
					killed_hard = true;
					signal_game((pid_t)parent, closing_inline ? SIGTERM : SIGKILL);
				}
			}
		}
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
