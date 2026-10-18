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


// ---- Hard daily limit (a Simple Mode setting) -------------------------------
// Pure logic; playtimed.c does the I/O. `enabled` already means "Simple Mode is
// on, it has a PIN and the limit is above 0".
typedef struct {
	bool enabled;
	int limit_minutes;	 // 1..1440
	int warn_minutes;	 // first warning before the limit; a last one at 1 minute
	int grace_seconds;	 // play time allowed after the limit before the game is closed
	int extra_minutes;	 // minutes added by one PIN unlock
	int extra_max;		 // unlocks per day; 0 = unlimited
} DailyLimitConfig;

// PIN unlocks granted on one civil day. A grant from another day counts for nothing.
typedef struct {
	int date;
	int count;
} DailyExtra;

typedef enum {
	DAILY_LIMIT_OK,
	DAILY_LIMIT_WARN,	  // inside warn_minutes
	DAILY_LIMIT_LAST,	  // inside the last minute
	DAILY_LIMIT_REACHED,  // over the allowance, still inside the grace period
	DAILY_LIMIT_EXPIRED,  // grace used up: close the game
} DailyLimitPhase;

// Allowance for `today` in ms: limit plus unlocks granted today.
uint64_t DailyLimit_allowanceMs(const DailyLimitConfig* config, const DailyExtra* extra, int today);
// Time left (negative once over). `state` from another day counts as 0 played.
int64_t DailyLimit_remainingMs(const DailyPlayTime* state, const DailyLimitConfig* config,
							   const DailyExtra* extra, int today);
DailyLimitPhase DailyLimit_phase(const DailyPlayTime* state, const DailyLimitConfig* config,
								 const DailyExtra* extra, int today);
bool DailyLimit_canGrant(const DailyLimitConfig* config, const DailyExtra* extra, int today);
void DailyLimit_grant(DailyExtra* extra, int today);
// "NN min left today." / "Time's up. Closing in N min." style text for a phase.
void DailyLimit_message(DailyLimitPhase phase, int64_t remaining_ms, int grace_seconds,
						char* out, size_t size);
bool DailyExtra_parse(DailyExtra* extra, const char* record);
int DailyExtra_format(const DailyExtra* extra, char* out, size_t size);

#endif
