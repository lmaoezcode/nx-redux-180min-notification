#include "../play_time.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void add_active_minutes(PlayTimeTracker* tracker, uint32_t* now, int minutes,
							   int repeat_minutes, char* message, size_t message_size,
							   int* reminder_count) {
	for (int second = 0; second < minutes * 60; second++) {
		*now += 1000;
		if (PlayTimeTracker_update(tracker, *now, true, repeat_minutes, message, message_size))
			(*reminder_count)++;
	}
}

int main(void) {
	PlayTimeTracker tracker;
	uint32_t now = 100;
	char message[64] = {0};
	int reminders = 0;

	PlayTimeTracker_init(&tracker, now, 60);
	add_active_minutes(&tracker, &now, 59, 30, message, sizeof(message), &reminders);
	assert(reminders == 0);
	add_active_minutes(&tracker, &now, 1, 30, message, sizeof(message), &reminders);
	assert(reminders == 1);
	assert(strcmp(message, "Play time: 1h. Take a short break.") == 0);
	add_active_minutes(&tracker, &now, 30, 30, message, sizeof(message), &reminders);
	assert(reminders == 2);
	assert(strcmp(message, "Play time: 1h 30m. Take a short break.") == 0);

	// A long sleep/menu gap is discarded.
	PlayTimeTracker_init(&tracker, now, 15);
	now += 10 * 60 * 1000;
	assert(!PlayTimeTracker_update(&tracker, now, true, 15, message, sizeof(message)));
	assert(tracker.elapsed_ms == 0);

	// Disabling repeat stops after the first reminder.
	reminders = 0;
	PlayTimeTracker_init(&tracker, now, 15);
	add_active_minutes(&tracker, &now, 30, 0, message, sizeof(message), &reminders);
	assert(reminders == 1);

	puts("play-time tracker tests passed");
	return 0;
}
