#include "../gamepad_mapper.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void testNeutralReport(void) {
	NxGamepadState state;
	uint8_t report[NXGP_BROWSER_REPORT_SIZE];
	NxGamepad_init(&state);
	NxGamepad_encodeBrowserReport(&state, report);
	uint8_t expected[NXGP_BROWSER_REPORT_SIZE] = {0};
	assert(memcmp(report, expected, sizeof(expected)) == 0);
	(void)expected;
}

static void testFaceAndStandardButtons(void) {
	NxGamepadState state;
	uint8_t report[NXGP_BROWSER_REPORT_SIZE];
	NxGamepad_init(&state);
	assert(NxGamepad_setKey(&state, NXGP_BTN_A, true));
	assert(NxGamepad_setKey(&state, NXGP_BTN_B, true));
	assert(NxGamepad_setKey(&state, NXGP_BTN_THUMBR, true));
	assert(NxGamepad_setKey(&state, NXGP_KEY_HOMEPAGE, true));
	NxGamepad_encodeBrowserReport(&state, report);
	assert(report[8] == 0x03);
	assert(report[9] == 0x08);
	assert(report[10] == 0x01);
	assert(NxGamepad_setKey(&state, NXGP_KEY_F2, true));
	assert(NxGamepad_setKey(&state, NXGP_KEY_F2, false));
	assert(!NxGamepad_setKey(&state, NXGP_KEY_VOLUMEUP, true));
}

static void testAxesAndDigitalTriggers(void) {
	NxGamepadState state;
	uint8_t report[NXGP_BROWSER_REPORT_SIZE];
	NxGamepad_init(&state);
	NxGamepad_setAbs(&state, NXGP_ABS_X, -32767);
	NxGamepad_setAbs(&state, NXGP_ABS_Y, 32767);
	NxGamepad_setAbs(&state, NXGP_ABS_RX, 0x1234);
	NxGamepad_setAbs(&state, NXGP_ABS_RY, -2);
	assert(!NxGamepad_setAbs(&state, NXGP_ABS_Z, 127));
	assert(!NxGamepad_setAbs(&state, NXGP_ABS_RZ, 127));
	assert(NxGamepad_setAbs(&state, NXGP_ABS_Z, 128));
	assert(NxGamepad_setAbs(&state, NXGP_ABS_RZ, 255));
	NxGamepad_encodeBrowserReport(&state, report);
	assert(report[0] == 0x01 && report[1] == 0x80);
	assert(report[2] == 0xff && report[3] == 0x7f);
	assert(report[4] == 0x34 && report[5] == 0x12);
	assert(report[6] == 0xfe && report[7] == 0xff);
	assert((report[8] & (1u << 6)) != 0);
	assert((report[8] & (1u << 7)) != 0);
	assert(NxGamepad_setAbs(&state, NXGP_ABS_Z, 0));
	assert(NxGamepad_setAbs(&state, NXGP_ABS_RZ, 0));
}

static void testDpadButtons(void) {
	NxGamepadState state;
	uint8_t report[NXGP_BROWSER_REPORT_SIZE];
	NxGamepad_init(&state);
	NxGamepad_setAbs(&state, NXGP_ABS_HAT0X, -1);
	NxGamepad_setAbs(&state, NXGP_ABS_HAT0Y, -1);
	NxGamepad_encodeBrowserReport(&state, report);
	assert(report[9] == 0x50); // buttons 12 (up) and 14 (left)
	NxGamepad_setAbs(&state, NXGP_ABS_HAT0X, 1);
	NxGamepad_setAbs(&state, NXGP_ABS_HAT0Y, 1);
	NxGamepad_encodeBrowserReport(&state, report);
	assert(report[9] == 0xa0); // buttons 13 (down) and 15 (right)
	NxGamepad_setAbs(&state, NXGP_ABS_HAT0X, 0);
	NxGamepad_setAbs(&state, NXGP_ABS_HAT0Y, 0);
	NxGamepad_encodeBrowserReport(&state, report);
	assert(report[9] == 0);
}

static void testPcCompatibilityReport(void) {
	NxGamepadState state;
	uint8_t report[NXGP_PC_REPORT_SIZE];
	NxGamepad_init(&state);
	NxGamepad_encodePcReport(&state, report);
	uint8_t neutral[NXGP_PC_REPORT_SIZE] = {0, 0, 8, 128, 128, 128, 128, 0};
	assert(memcmp(report, neutral, sizeof(neutral)) == 0);

	NxGamepad_setKey(&state, NXGP_BTN_A, true); // physical bottom -> raw b1
	NxGamepad_setKey(&state, NXGP_BTN_X, true); // physical left -> raw b0
	NxGamepad_setKey(&state, NXGP_BTN_TL2, true);
	NxGamepad_setKey(&state, NXGP_BTN_THUMBR, true);
	NxGamepad_setKey(&state, NXGP_KEY_HOMEPAGE, true);
	NxGamepad_setAbs(&state, NXGP_ABS_HAT0X, 1);
	NxGamepad_setAbs(&state, NXGP_ABS_HAT0Y, -1);
	NxGamepad_encodePcReport(&state, report);
	assert(report[0] == 0x43); // raw b0, b1 and b6
	assert(report[1] == 0x18); // raw b11 and HOME at raw b12
	assert(report[2] == 1);    // up-right POV
	assert(report[3] == 128 && report[4] == 128);
	assert(report[5] == 128 && report[6] == 128);
	assert(report[7] == 0);

	NxGamepad_setAbs(&state, NXGP_ABS_HAT0X, -1);
	NxGamepad_setAbs(&state, NXGP_ABS_HAT0Y, 1);
	NxGamepad_encodePcReport(&state, report);
	assert(report[2] == 5); // down-left

	NxGamepad_setAbs(&state, NXGP_ABS_X, -32768);
	NxGamepad_setAbs(&state, NXGP_ABS_Y, 32767);
	NxGamepad_setAbs(&state, NXGP_ABS_RX, -1);
	NxGamepad_setAbs(&state, NXGP_ABS_RY, 0);
	NxGamepad_encodePcReport(&state, report);
	assert(report[3] == 0);
	assert(report[4] == 255);
	assert(report[5] == 127);
	assert(report[6] == 128);
}

static void testLayoutSwitch(void) {
	NxGamepadState state;
	uint8_t report[NXGP_PC_REPORT_SIZE];
	NxGamepad_init(&state);
	assert(state.layout == NXGP_LAYOUT_XBOX);

	// Physical A is the right button (BTN_B from TRIMUI Player1). In Xbox
	// mode it is host B/raw b2; Nintendo mode follows the cap and makes it
	// host A/raw b1.
	NxGamepad_setKey(&state, NXGP_BTN_B, true);
	NxGamepad_encodePcReport(&state, report);
	assert(report[0] == 0x04);
	assert(NxGamepad_toggleLayout(&state) == NXGP_LAYOUT_NINTENDO);
	NxGamepad_encodePcReport(&state, report);
	assert(report[0] == 0x02);
	assert(NxGamepad_setLayout(&state, NXGP_LAYOUT_XBOX));
	assert(!NxGamepad_setLayout(&state, NXGP_LAYOUT_XBOX));

	// FN keys are local controls and never enter the host report.
	assert(NxGamepad_setKey(&state, NXGP_KEY_F1, true));
	assert(!NxGamepad_setKey(&state, NXGP_KEY_F1, true));
	assert(NxGamepad_setKey(&state, NXGP_KEY_F2, true));
	NxGamepad_encodePcReport(&state, report);
	assert(report[0] == 0x04);
}

static void testSafeLayoutTransitionHelpers(void) {
	NxGamepadState state;
	uint8_t report[NXGP_PC_REPORT_SIZE];
	NxGamepad_init(&state);
	assert(NxGamepad_otherLayout(state.layout) == NXGP_LAYOUT_NINTENDO);
	assert(!NxGamepad_faceButtonsHeld(&state));

	NxGamepad_setKey(&state, NXGP_BTN_A, true);
	NxGamepad_setKey(&state, NXGP_BTN_B, true);
	NxGamepad_setKey(&state, NXGP_BTN_TL, true);
	assert(NxGamepad_faceButtonsHeld(&state));
	NxGamepad_clearFaceButtons(&state);
	assert(!NxGamepad_faceButtonsHeld(&state));
	NxGamepad_encodePcReport(&state, report);
	assert(report[0] == 0x10); // shoulder remains held; face buttons are neutral
}

static void testExitChord(void) {
	NxGamepadState state;
	NxGamepad_init(&state);
	NxGamepad_setKey(&state, NXGP_BTN_MODE, true);
	assert(!NxGamepad_exitChordActive(&state));
	NxGamepad_setKey(&state, NXGP_BTN_SELECT, true);
	assert(NxGamepad_exitChordActive(&state));
	NxGamepad_setKey(&state, NXGP_BTN_MODE, false);
	assert(!NxGamepad_exitChordActive(&state));
}

int main(void) {
	testNeutralReport();
	testFaceAndStandardButtons();
	testAxesAndDigitalTriggers();
	testDpadButtons();
	testPcCompatibilityReport();
	testLayoutSwitch();
	testSafeLayoutTransitionHelpers();
	testExitChord();
	puts("usbgamepad mapper tests passed");
	return 0;
}
