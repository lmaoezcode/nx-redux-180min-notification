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

	// ---- hard daily limit ----
	{
		const int today = 20261004;
		DailyLimitConfig lim = {true, 60, 5, 180, 15, 3};
		DailyExtra none = {0, 0};
		DailyPlayTime t = {0};
		char msg[128];
		DailyPlayTime_tick(&t, today, 36000000, 0, false);
		assert(DailyLimit_phase(&t, &lim, &none, today) == DAILY_LIMIT_OK);
		play(&t, 54);
		assert(DailyLimit_phase(&t, &lim, &none, today) == DAILY_LIMIT_OK);
		play(&t, 1); // 55 min played, 5 left
		assert(DailyLimit_phase(&t, &lim, &none, today) == DAILY_LIMIT_WARN);
		DailyLimit_message(DAILY_LIMIT_WARN, DailyLimit_remainingMs(&t, &lim, &none, today), 180, msg, sizeof(msg));
		assert(strstr(msg, "5 min"));
		play(&t, 4); // 59 min played
		assert(DailyLimit_phase(&t, &lim, &none, today) == DAILY_LIMIT_LAST);
		play(&t, 1); // 60 min: reached, grace starts
		assert(DailyLimit_phase(&t, &lim, &none, today) == DAILY_LIMIT_REACHED);
		DailyLimit_message(DAILY_LIMIT_REACHED, 0, 180, msg, sizeof(msg));
		assert(strstr(msg, "3 min"));
		play(&t, 2);
		assert(DailyLimit_phase(&t, &lim, &none, today) == DAILY_LIMIT_REACHED);
		play(&t, 1); // 63 min: grace (3 min) used up
		assert(DailyLimit_phase(&t, &lim, &none, today) == DAILY_LIMIT_EXPIRED);
		// A PIN unlock adds minutes for TODAY only and can be repeated up to the cap.
		DailyExtra extra = {0, 0};
		assert(DailyLimit_canGrant(&lim, &extra, today));
		DailyLimit_grant(&extra, today);
		assert(DailyLimit_allowanceMs(&lim, &extra, today) == 75 * DAILY_MINUTE_MS);
		assert(DailyLimit_phase(&t, &lim, &extra, today) == DAILY_LIMIT_OK);
		DailyLimit_grant(&extra, today);
		DailyLimit_grant(&extra, today);
		assert(!DailyLimit_canGrant(&lim, &extra, today)); // cap of 3
		lim.extra_max = 0; // 0 = unlimited
		assert(DailyLimit_canGrant(&lim, &extra, today));
		lim.extra_max = 3;
		assert(DailyLimit_allowanceMs(&lim, &extra, today + 1) == 60 * DAILY_MINUTE_MS); // old grants do not carry over
		// Another day's ledger counts as nothing played.
		assert(DailyLimit_phase(&t, &lim, &none, today + 1) == DAILY_LIMIT_OK);
		// Disabled, or an allowance of a whole day, never triggers.
		lim.enabled = false;
		assert(DailyLimit_phase(&t, &lim, &none, today) == DAILY_LIMIT_OK);
		lim.enabled = true;
		lim.limit_minutes = 1440;
		assert(DailyLimit_phase(&t, &lim, &none, today) == DAILY_LIMIT_OK);
		// warn_minutes of 1 leaves only the last-minute warning.
		lim.limit_minutes = 60;
		lim.warn_minutes = 1;
		DailyPlayTime w = {0};
		DailyPlayTime_tick(&w, today, 36000000, 0, false);
		play(&w, 56);
		assert(DailyLimit_phase(&w, &lim, &none, today) == DAILY_LIMIT_OK);
		// The unlock record survives a restart and rejects garbage.
		char rec[64];
		DailyExtra back = {0, 0};
		assert(DailyExtra_format(&extra, rec, sizeof(rec)) > 0 && DailyExtra_parse(&back, rec));
		assert(back.date == today && back.count == 3);
		assert(!DailyExtra_parse(&back, "NX_DAILY_EXTRA 1 20261340 1\n"));
		assert(!DailyExtra_parse(&back, "NX_DAILY_EXTRA 2 20261004 1\n"));
		assert(!DailyExtra_parse(&back, "NX_DAILY_EXTRA 1 20261004 -1\n"));
		assert(!DailyExtra_parse(&back, "NX_DAILY_EXTRA 1 20261004 1 junk\n"));
	}
	puts("daily play time: all unit checks passed");
	return 0;
}
