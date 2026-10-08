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
