#include "play_time.h"

#include <stdio.h>

#define PLAY_TIME_MINUTE_MS (60ULL * 1000ULL)
#define PLAY_TIME_MAX_ACTIVE_GAP_MS 5000U

void PlayTimeTracker_init(PlayTimeTracker* tracker, uint32_t now, int first_minutes) {
	if (!tracker)
		return;
	tracker->elapsed_ms = 0;
	tracker->next_reminder_ms = first_minutes > 0
								? (uint64_t)first_minutes * PLAY_TIME_MINUTE_MS
								: 0;
	tracker->last_tick = now;
}

void PlayTimeTracker_resume(PlayTimeTracker* tracker, uint32_t now) {
	if (tracker)
		tracker->last_tick = now;
}

bool PlayTimeTracker_update(PlayTimeTracker* tracker, uint32_t now, bool enabled,
							int repeat_minutes, char* message, size_t message_size) {
	if (!tracker)
		return false;

	uint32_t delta = now - tracker->last_tick;
	tracker->last_tick = now;
	if (!enabled || tracker->next_reminder_ms == 0)
		return false;

	if (delta <= PLAY_TIME_MAX_ACTIVE_GAP_MS)
		tracker->elapsed_ms += delta;
	if (tracker->elapsed_ms < tracker->next_reminder_ms)
		return false;

	uint64_t total_minutes = tracker->elapsed_ms / PLAY_TIME_MINUTE_MS;
	if (message && message_size > 0) {
		if (total_minutes >= 60 && total_minutes % 60 != 0) {
			snprintf(message, message_size, "Play time: %lluh %llum. Take a short break.",
					 (unsigned long long)(total_minutes / 60),
					 (unsigned long long)(total_minutes % 60));
		} else if (total_minutes >= 60) {
			snprintf(message, message_size, "Play time: %lluh. Take a short break.",
					 (unsigned long long)(total_minutes / 60));
		} else {
			snprintf(message, message_size, "Play time: %llum. Take a short break.",
					 (unsigned long long)total_minutes);
		}
	}

	if (repeat_minutes <= 0) {
		tracker->next_reminder_ms = 0;
	} else {
		uint64_t repeat_ms = (uint64_t)repeat_minutes * PLAY_TIME_MINUTE_MS;
		do {
			tracker->next_reminder_ms += repeat_ms;
		} while (tracker->next_reminder_ms <= tracker->elapsed_ms);
	}
	return true;
}
