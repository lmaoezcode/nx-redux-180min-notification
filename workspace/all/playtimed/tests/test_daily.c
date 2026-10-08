#include "../daily.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void play(DailyPlayTime* state, unsigned minutes) {
	for (unsigned i = 0; i < minutes * 60; ++i)
		DailyPlayTime_tick(state, 20261004, 36000000, 1000, true);
}

static void record(DailyPlayTime* state, DailyPlayTime* restored) {
	char text[160];
	assert(DailyPlayTime_format(state, text, sizeof(text)) > 0);
	assert(DailyPlayTime_parse(restored, text));
	assert(restored->date == state->date && restored->elapsed_ms == state->elapsed_ms);
	assert(restored->notified == state->notified && restored->notified_at_ms == state->notified_at_ms);
}

int main(void) {
	DailyPlayTime state = {0}, next = {0};
	DailyPlayTimeConfig config = {true, 180, 30, 5};
	assert(DailyPlayTime_tick(&state, 20261004, 36000000, 0, false));
	play(&state, 60); // Emulator 1, then a port, then emulator 2; same ledger.
	record(&state, &next);
	play(&next, 119);
	assert(!DailyPlayTime_due(&next, &config));
	play(&next, 1);
	assert(next.elapsed_ms == 180 * DAILY_MINUTE_MS && DailyPlayTime_due(&next, &config));
	DailyPlayTime_notified(&next);
	record(&next, &state); // Restart does not re-notify the same milestone.
	assert(!DailyPlayTime_due(&state, &config));
	play(&state, 29);
	assert(!DailyPlayTime_due(&state, &config));
	play(&state, 1);
	assert(DailyPlayTime_due(&state, &config));
	DailyPlayTime_notified(&state);
	config.repeat_minutes = 0;
	play(&state, 30);
	assert(!DailyPlayTime_due(&state, &config));
	config.enabled = false;
	assert(!DailyPlayTime_due(&state, &config));
	// Disabling alerts doesn't erase or stop the daily tally.
	uint64_t before = state.elapsed_ms;
	play(&state, 1);
	assert(state.elapsed_ms == before + DAILY_MINUTE_MS);
	config.enabled = true;
	config.repeat_minutes = 30;
	config.first_minutes = 300;
	assert(!DailyPlayTime_due(&state, &config));

	before = state.elapsed_ms;
	for (int i = 0; i < 120; ++i)
		DailyPlayTime_tick(&state, 20261004, 36000000, 1000, false);
	DailyPlayTime_tick(&state, 20261004, 36000000, 3600000, true);
	assert(state.elapsed_ms == before); // Menu, sleep and blocked gaps excluded.
	assert(DailyPlayTime_tick(&state, 20261005, 250, 1000, true));
	assert(state.elapsed_ms == 250 && !state.notified && state.notified_at_ms == 0);
	config.first_minutes = 1;
	config.repeat_minutes = 0;
	DailyPlayTime_tick(&state, 20261005, 36000000, 5000, true);
	for (int i = 0; i < 55; ++i)
		DailyPlayTime_tick(&state, 20261005, 36000000, 1000, true);
	assert(DailyPlayTime_due(&state, &config)); // Off = once EACH day.
	DailyPlayTime_notified(&state);
	assert(!DailyPlayTime_due(&state, &config));
	// Civil-clock jumps never turn into hours of elapsed game time.
	assert(DailyPlayTime_tick(&state, 20261006, 36000000, 1000, true));
	assert(state.elapsed_ms == 1000);
	assert(DailyPlayTime_tick(&state, 20261004, 36000000, 1000, true));
	assert(state.elapsed_ms == 1000);
	state.elapsed_ms = DAILY_LIMIT_MS - 1;
	DailyPlayTime_tick(&state, state.date, 36000000, 1000, true);
	assert(state.elapsed_ms == DAILY_LIMIT_MS);

	assert(!DailyPlayTime_parse(&next, "garbage"));
	assert(!DailyPlayTime_parse(&next, "NX_DAILY_PLAYTIME 2 20261004 100 0 0\n"));
	assert(!DailyPlayTime_parse(&next, "NX_DAILY_PLAYTIME 1 20260229 100 0 0\n"));
	assert(!DailyPlayTime_parse(&next, "NX_DAILY_PLAYTIME 1 20261004 -1 0 0\n"));
	assert(!DailyPlayTime_parse(&next, "NX_DAILY_PLAYTIME 1 20261004 100 1 101\n"));
	assert(!DailyPlayTime_parse(&next, "NX_DAILY_PLAYTIME 1 20261004 100 0 1\n"));
	assert(!DailyPlayTime_parse(&next, "NX_DAILY_PLAYTIME 1 20261004 100 0 0\nEXTRA"));
	assert(DailyPlayTime_parse(&next, "NX_DAILY_PLAYTIME 1 20240229 100 0 0\n"));
	char message[128];
	DailyPlayTime_message(&state, message, sizeof(message));
	assert(strstr(message, "Today: 24h 0m"));

	const char* sd = "/mnt/SDCARD";
	assert(DailyPlayTime_gameCommand("'/mnt/SDCARD/Emus/GB.pak/launch.sh' '/mnt/SDCARD/Roms/Game Boy (GB)/Link'\\''s Awakening.gb'", sd));
	assert(DailyPlayTime_gameCommand("'/mnt/SDCARD/Emus/tg5040/DC.pak/launch.sh' '/mnt/SDCARD/Roms/DC/Game.chd'", sd));
	assert(DailyPlayTime_gameCommand("'/mnt/SDCARD/.system/paks/Emus/GBA.pak/launch.sh' '/mnt/SDCARD/Roms/GBA/Game.gba'", sd));
	assert(DailyPlayTime_gameCommand("'/mnt/SDCARD/Emus/PORTS.pak/launch.sh' '/mnt/SDCARD/Roms/Ports (PORTS)/Balatro.sh'", sd));
	assert(!DailyPlayTime_gameCommand("'/mnt/SDCARD/Tools/PortMaster.pak/launch.sh'", sd));
	assert(!DailyPlayTime_gameCommand("'/mnt/SDCARD/.system/paks/Tools/USB Storage.pak/launch.sh'", sd));
	assert(!DailyPlayTime_gameCommand("'/mnt/SDCARD/.system/paks/Tools/USB Gamepad.pak/launch.sh'", sd));
	assert(!DailyPlayTime_gameCommand("'/mnt/SDCARD/Tools/Test.pak/launch.sh' '/mnt/SDCARD/Roms/GBA/Game.gba'", sd));
	assert(!DailyPlayTime_gameCommand("'/mnt/SDCARD/Emus/GBA.pak/launch.sh' '/mnt/SDCARD/RomsExtra/a.gba'", sd));
	assert(!DailyPlayTime_gameCommand("'/mnt/SDCARD/Emus/GBA.pak/launch.sh' '/mnt/SDCARD/Roms/../Tools/a.sh'", sd));
	assert(!DailyPlayTime_gameCommand("'/mnt/SDCARD/Emus/GBA.pak/launch.sh' '/mnt/SDCARD/Roms/GBA/Game.gba' ''", sd));
	assert(!DailyPlayTime_gameCommand("'/mnt/SDCARD/Emus/GBA.pak/launch.sh' '/mnt/SDCARD/Roms/a.gba'; reboot", sd));
	assert(!DailyPlayTime_gameCommand("'/mnt/SDCARD/Emus/GBA.pak/launch.sh' '/mnt/SDCARD/Roms/a.gba", sd));
	assert(!DailyPlayTime_gameCommand(NULL, sd));
	puts("daily play time: all unit checks passed");
	return 0;
}
