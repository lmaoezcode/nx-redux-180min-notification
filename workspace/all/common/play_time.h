#ifndef __PLAY_TIME_H__
#define __PLAY_TIME_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint64_t elapsed_ms;
	uint64_t next_reminder_ms;
	uint32_t last_tick;
} PlayTimeTracker;

void PlayTimeTracker_init(PlayTimeTracker* tracker, uint32_t now, int first_minutes);
void PlayTimeTracker_resume(PlayTimeTracker* tracker, uint32_t now);

// Adds active wall-clock time and formats a reminder when a milestone is met.
// Gaps over five seconds are treated as menu/suspend time and are not counted.
bool PlayTimeTracker_update(PlayTimeTracker* tracker, uint32_t now, bool enabled,
							int repeat_minutes, char* message, size_t message_size);

#endif // __PLAY_TIME_H__
