#ifndef NX_DAILY_PLAY_TIME_H
#define NX_DAILY_PLAY_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DAILY_MINUTE_MS UINT64_C(60000)
#define DAILY_LIMIT_MS UINT64_C(86400000)

typedef struct {
	int date; // Local YYYYMMDD, never an elapsed wall-clock timestamp.
	uint64_t elapsed_ms;
	bool notified;
	uint64_t notified_at_ms;
} DailyPlayTime;

typedef struct {
	bool enabled;
	int first_minutes;
	int repeat_minutes;
	int duration_seconds;
} DailyPlayTimeConfig;

// Elapsed time comes only from CLOCK_MONOTONIC. Civil time selects the day.
// On a day change, only the part AFTER local midnight belongs to the new day.
bool DailyPlayTime_tick(DailyPlayTime* state, int date, uint64_t since_midnight_ms,
						uint64_t delta_ms, bool active);
bool DailyPlayTime_due(const DailyPlayTime* state, const DailyPlayTimeConfig* config);
void DailyPlayTime_notified(DailyPlayTime* state);
void DailyPlayTime_message(const DailyPlayTime* state, char* out, size_t size);
bool DailyPlayTime_parse(DailyPlayTime* state, const char* record);
int DailyPlayTime_format(const DailyPlayTime* state, char* out, size_t size);

// Classifies the canonical TWO shell words nextui queues; never executes them.
// Tools (including USB Storage, Settings and the PortMaster manager) are not games.
bool DailyPlayTime_gameCommand(const char* command, const char* sdcard);

#endif
