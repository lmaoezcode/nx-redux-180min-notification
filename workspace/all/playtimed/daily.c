#include "daily.h"

#include <stdio.h>
#include <string.h>

static bool valid_date(int date) {
	int year = date / 10000, month = date / 100 % 100, day = date % 100;
	static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1)
		return false;
	int leap = month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
	return day <= days[month] + leap;
}

bool DailyPlayTime_tick(DailyPlayTime* state, int date, uint64_t since_midnight_ms,
						uint64_t delta_ms, bool active) {
	if (!state || !valid_date(date))
		return false;
	bool new_day = state->date != date;
	if (new_day) {
		*state = (DailyPlayTime){.date = date};
		if (delta_ms > since_midnight_ms)
			delta_ms = since_midnight_ms;
	}
	// Do not charge a long scheduler/I/O stall or a suspend gap as play time.
	if (active && delta_ms <= 5000) {
		if (delta_ms > DAILY_LIMIT_MS - state->elapsed_ms)
			state->elapsed_ms = DAILY_LIMIT_MS;
		else
			state->elapsed_ms += delta_ms;
	}
	return new_day;
}

bool DailyPlayTime_due(const DailyPlayTime* state, const DailyPlayTimeConfig* config) {
	if (!state || !config || !config->enabled || config->first_minutes <= 0)
		return false;
	uint64_t first = (uint64_t)config->first_minutes * DAILY_MINUTE_MS;
	if (state->elapsed_ms < first)
		return false;
	if (!state->notified)
		return true;
	if (config->repeat_minutes <= 0)
		return false;
	return state->elapsed_ms - state->notified_at_ms >=
		(uint64_t)config->repeat_minutes * DAILY_MINUTE_MS;
}

void DailyPlayTime_notified(DailyPlayTime* state) {
	state->notified = true;
	state->notified_at_ms = state->elapsed_ms;
}

void DailyPlayTime_message(const DailyPlayTime* state, char* out, size_t size) {
	uint64_t minutes = state->elapsed_ms / DAILY_MINUTE_MS;
	if (minutes >= 60)
		snprintf(out, size, "Today: %lluh %llum. Take a break.",
			(unsigned long long)(minutes / 60), (unsigned long long)(minutes % 60));
	else
		snprintf(out, size, "Today: %llum. Take a break.", (unsigned long long)minutes);
}

bool DailyPlayTime_parse(DailyPlayTime* state, const char* record) {
	int version, date, notified, used = 0;
	unsigned long long elapsed, at;
	if (!state || !record ||
		sscanf(record, "NX_DAILY_PLAYTIME %d %d %llu %d %llu %n",
			&version, &date, &elapsed, &notified, &at, &used) != 5 || !used || record[used] ||
		version != 1 || !valid_date(date) || elapsed > DAILY_LIMIT_MS || at > elapsed ||
		(notified != 0 && notified != 1) || (!notified && at != 0))
		return false;
	*state = (DailyPlayTime){date, elapsed, notified != 0, at};
	return true;
}

int DailyPlayTime_format(const DailyPlayTime* state, char* out, size_t size) {
	return snprintf(out, size, "NX_DAILY_PLAYTIME 1 %d %llu %d %llu\n", state->date,
		(unsigned long long)state->elapsed_ms, state->notified ? 1 : 0,
		(unsigned long long)state->notified_at_ms);
}

// nextui single-quotes both paths and escapes apostrophes as '\''.
// Reject truncation, unbalanced quotes, operators and extra shell words.
static bool two_words(const char* command, char words[2][4096]) {
	int word = 0, quoted = 0, started = 0;
	size_t length = 0;
	if (!command)
		return false;
	for (const char* p = command; *p; ++p) {
		char c = *p;
		if (quoted) {
			if (c == '\'') { quoted = 0; continue; }
		} else if (c == '\'') {
			if (word >= 2)
				return false;
			quoted = 1;
			started = 1;
			continue;
		} else if (c == '\\' && p[1]) {
			c = *++p;
		} else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			if (started) {
				words[word++][length] = 0;
				length = 0;
				started = 0;
			}
			continue;
		} else if (strchr(";|&<>$`\"#(){}*?[]\\", c)) {
			return false;
		}
		if (word >= 2 || length + 1 >= sizeof(words[0]))
			return false;
		started = 1;
		words[word][length++] = c;
	}
	if (quoted)
		return false;
	if (started)
		words[word++][length] = 0;
	return word == 2;
}

static bool under(const char* path, const char* directory) {
	size_t n = strlen(directory);
	return strncmp(path, directory, n) == 0 && path[n] == '/' && path[n + 1];
}

bool DailyPlayTime_gameCommand(const char* command, const char* sdcard) {
	char words[2][4096], directory[4096];
	if (!sdcard || !sdcard[0] || !two_words(command, words))
		return false;
	// Don't allow a Tool name, or ../ escaping the designated ROM/emulator roots.
	if (strstr(words[0], "/../") || strstr(words[1], "/../"))
		return false;
	size_t n = strlen(words[0]);
	const char* suffix = ".pak/launch.sh";
	size_t suffix_n = strlen(suffix);
	if (n < suffix_n || strcmp(words[0] + n - suffix_n, suffix))
		return false;
	int len = snprintf(directory, sizeof(directory), "%s/Roms", sdcard);
	if (len < 0 || (size_t)len >= sizeof(directory) || !under(words[1], directory))
		return false;
	len = snprintf(directory, sizeof(directory), "%s/Emus", sdcard);
	if (len < 0 || (size_t)len >= sizeof(directory))
		return false;
	if (under(words[0], directory))
		return true;
	len = snprintf(directory, sizeof(directory), "%s/.system/paks/Emus", sdcard);
	return len >= 0 && (size_t)len < sizeof(directory) && under(words[0], directory);
}

// ---- Hard daily limit -----------------------------------------------------

static int extra_count(const DailyExtra* extra, int today) {
	return extra && extra->date == today && extra->count > 0 ? extra->count : 0;
}

uint64_t DailyLimit_allowanceMs(const DailyLimitConfig* config, const DailyExtra* extra, int today) {
	if (!config || config->limit_minutes <= 0)
		return 0;
	uint64_t minutes = (uint64_t)config->limit_minutes;
	if (config->extra_minutes > 0)
		minutes += (uint64_t)config->extra_minutes * (uint64_t)extra_count(extra, today);
	uint64_t ms = minutes * DAILY_MINUTE_MS;
	return ms > DAILY_LIMIT_MS ? DAILY_LIMIT_MS : ms;
}

int64_t DailyLimit_remainingMs(const DailyPlayTime* state, const DailyLimitConfig* config,
							   const DailyExtra* extra, int today) {
	uint64_t played = state && state->date == today ? state->elapsed_ms : 0;
	return (int64_t)DailyLimit_allowanceMs(config, extra, today) - (int64_t)played;
}

DailyLimitPhase DailyLimit_phase(const DailyPlayTime* state, const DailyLimitConfig* config,
								 const DailyExtra* extra, int today) {
	if (!config || !config->enabled || config->limit_minutes <= 0)
		return DAILY_LIMIT_OK;
	// The ledger stops at 24 h, so an allowance of a full day can never be reached.
	if (DailyLimit_allowanceMs(config, extra, today) >= DAILY_LIMIT_MS)
		return DAILY_LIMIT_OK;
	int64_t left = DailyLimit_remainingMs(state, config, extra, today);
	int64_t grace = (int64_t)(config->grace_seconds > 0 ? config->grace_seconds : 0) * 1000;
	if (left <= -grace)
		return DAILY_LIMIT_EXPIRED;
	if (left <= 0)
		return DAILY_LIMIT_REACHED;
	if (left <= (int64_t)DAILY_MINUTE_MS)
		return DAILY_LIMIT_LAST;
	if (config->warn_minutes > 1 && left <= (int64_t)config->warn_minutes * (int64_t)DAILY_MINUTE_MS)
		return DAILY_LIMIT_WARN;
	return DAILY_LIMIT_OK;
}

bool DailyLimit_canGrant(const DailyLimitConfig* config, const DailyExtra* extra, int today) {
	if (!config || !config->enabled || config->extra_minutes <= 0)
		return false;
	return config->extra_max <= 0 || extra_count(extra, today) < config->extra_max;
}

void DailyLimit_grant(DailyExtra* extra, int today) {
	if (!extra)
		return;
	int count = extra_count(extra, today);
	extra->date = today;
	extra->count = count < 1000 ? count + 1 : count;
}

void DailyLimit_message(DailyLimitPhase phase, int64_t remaining_ms, int grace_seconds,
						char* out, size_t size) {
	if (phase == DAILY_LIMIT_WARN) {
		long long minutes = (remaining_ms + (int64_t)DAILY_MINUTE_MS - 1) / (int64_t)DAILY_MINUTE_MS;
		snprintf(out, size, "%lld min of play time left today.", minutes);
	} else if (phase == DAILY_LIMIT_LAST) {
		snprintf(out, size, "1 minute of play time left today.");
	} else if (phase == DAILY_LIMIT_REACHED) {
		long long left = grace_seconds + remaining_ms / 1000; // remaining_ms is <= 0 here
		if (left < 0)
			left = 0;
		if (left >= 90)
			snprintf(out, size, "Time's up. Closing in %lld min. Save now.", (left + 59) / 60);
		else
			snprintf(out, size, "Time's up. Closing in %lld s. Save now.", left);
	} else {
		snprintf(out, size, "Time's up for today.");
	}
}

bool DailyExtra_parse(DailyExtra* extra, const char* record) {
	int version, date, count, used = 0;
	if (!extra || !record ||
		sscanf(record, "NX_DAILY_EXTRA %d %d %d %n", &version, &date, &count, &used) != 3 || !used ||
		record[used] || version != 1 || !valid_date(date) || count < 0 || count > 1000)
		return false;
	*extra = (DailyExtra){date, count};
	return true;
}

int DailyExtra_format(const DailyExtra* extra, char* out, size_t size) {
	return snprintf(out, size, "NX_DAILY_EXTRA 1 %d %d\n", extra->date, extra->count);
}
