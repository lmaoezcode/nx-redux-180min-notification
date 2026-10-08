#include "gamepad_mapper.h"

#include <string.h>

#define BUTTON_SOUTH NXGP_BUTTON_SOUTH
#define BUTTON_EAST NXGP_BUTTON_EAST
#define BUTTON_WEST NXGP_BUTTON_WEST
#define BUTTON_NORTH NXGP_BUTTON_NORTH
#define BUTTON_L1 NXGP_BUTTON_L1
#define BUTTON_R1 NXGP_BUTTON_R1
#define BUTTON_L2 NXGP_BUTTON_L2
#define BUTTON_R2 NXGP_BUTTON_R2
#define BUTTON_SELECT NXGP_BUTTON_SELECT
#define BUTTON_START NXGP_BUTTON_START
#define BUTTON_L3 NXGP_BUTTON_L3
#define BUTTON_R3 NXGP_BUTTON_R3
#define BUTTON_DPAD_UP NXGP_BUTTON_DPAD_UP
#define BUTTON_DPAD_DOWN NXGP_BUTTON_DPAD_DOWN
#define BUTTON_DPAD_LEFT NXGP_BUTTON_DPAD_LEFT
#define BUTTON_DPAD_RIGHT NXGP_BUTTON_DPAD_RIGHT
#define BUTTON_HOME NXGP_BUTTON_HOME

static bool setButton(NxGamepadState* state, unsigned int index, bool pressed) {
	uint32_t mask = 1u << index;
	uint32_t before = state->buttons;
	if (pressed)
		state->buttons |= mask;
	else
		state->buttons &= ~mask;
	return before != state->buttons;
}

void NxGamepad_init(NxGamepadState* state) {
	memset(state, 0, sizeof(*state));
	state->layout = NXGP_LAYOUT_XBOX;
}

bool NxGamepad_setLayout(NxGamepadState* state, NxGamepadLayout layout) {
	if (layout != NXGP_LAYOUT_XBOX && layout != NXGP_LAYOUT_NINTENDO)
		return false;
	bool changed = state->layout != layout;
	state->layout = layout;
	return changed;
}

NxGamepadLayout NxGamepad_toggleLayout(NxGamepadState* state) {
	state->layout = NxGamepad_otherLayout(state->layout);
	return state->layout;
}

NxGamepadLayout NxGamepad_otherLayout(NxGamepadLayout layout) {
	return layout == NXGP_LAYOUT_NINTENDO ? NXGP_LAYOUT_XBOX : NXGP_LAYOUT_NINTENDO;
}

bool NxGamepad_faceButtonsHeld(const NxGamepadState* state) {
	const uint32_t faceMask = (1u << BUTTON_SOUTH) | (1u << BUTTON_EAST) |
		(1u << BUTTON_WEST) | (1u << BUTTON_NORTH);
	return (state->buttons & faceMask) != 0;
}

void NxGamepad_clearFaceButtons(NxGamepadState* state) {
	const uint32_t faceMask = (1u << BUTTON_SOUTH) | (1u << BUTTON_EAST) |
		(1u << BUTTON_WEST) | (1u << BUTTON_NORTH);
	state->buttons &= ~faceMask;
}

bool NxGamepad_setKey(NxGamepadState* state, uint16_t code, bool pressed) {
	unsigned int button;
	switch (code) {
	case NXGP_BTN_A: button = BUTTON_SOUTH; break; // physical B, bottom
	case NXGP_BTN_B: button = BUTTON_EAST; break;  // physical A, right
	case NXGP_BTN_X: button = BUTTON_WEST; break;  // physical Y, left
	case NXGP_BTN_Y: button = BUTTON_NORTH; break; // physical X, top
	case NXGP_BTN_TL: button = BUTTON_L1; break;
	case NXGP_BTN_TR: button = BUTTON_R1; break;
	case NXGP_BTN_TL2: button = BUTTON_L2; break;
	case NXGP_BTN_TR2: button = BUTTON_R2; break;
	case NXGP_BTN_SELECT: button = BUTTON_SELECT; break;
	case NXGP_BTN_START: button = BUTTON_START; break;
	case NXGP_BTN_THUMBL: button = BUTTON_L3; break;
	case NXGP_BTN_THUMBR: button = BUTTON_R3; break;
	case NXGP_KEY_HOMEPAGE: button = BUTTON_HOME; break;
	case NXGP_KEY_F1: {
		bool changed = state->fn1_pressed != pressed;
		state->fn1_pressed = pressed;
		return changed;
	}
	case NXGP_KEY_F2: {
		bool changed = state->fn2_pressed != pressed;
		state->fn2_pressed = pressed;
		return changed;
	}
	case NXGP_BTN_MODE: {
		bool changed = state->menu_pressed != pressed;
		state->menu_pressed = pressed;
		return changed;
	}
	default: return false;
	}
	return setButton(state, button, pressed);
}

bool NxGamepad_setAbs(NxGamepadState* state, uint16_t code, int32_t value) {
	switch (code) {
	case NXGP_ABS_X:
		if (state->left_x == value)
			return false;
		state->left_x = (int16_t)value;
		return true;
	case NXGP_ABS_Y:
		if (state->left_y == value)
			return false;
		state->left_y = (int16_t)value;
		return true;
	case NXGP_ABS_RX:
		if (state->right_x == value)
			return false;
		state->right_x = (int16_t)value;
		return true;
	case NXGP_ABS_RY:
		if (state->right_y == value)
			return false;
		state->right_y = (int16_t)value;
		return true;
	case NXGP_ABS_Z: return setButton(state, BUTTON_L2, value >= 128);
	case NXGP_ABS_RZ: return setButton(state, BUTTON_R2, value >= 128);
	case NXGP_ABS_HAT0X: {
		bool changed = setButton(state, BUTTON_DPAD_LEFT, value < 0);
		return setButton(state, BUTTON_DPAD_RIGHT, value > 0) || changed;
	}
	case NXGP_ABS_HAT0Y: {
		bool changed = setButton(state, BUTTON_DPAD_UP, value < 0);
		return setButton(state, BUTTON_DPAD_DOWN, value > 0) || changed;
	}
	default: return false;
	}
}

bool NxGamepad_exitChordActive(const NxGamepadState* state) {
	return state->menu_pressed && (state->buttons & (1u << BUTTON_SELECT)) != 0;
}

static void putS16LE(uint8_t* dst, int16_t value) {
	uint16_t raw = (uint16_t)value;
	dst[0] = (uint8_t)(raw & 0xff);
	dst[1] = (uint8_t)(raw >> 8);
}

static uint8_t signedAxisToU8(int16_t value) {
	// Preserve both endpoints and put the signed centre exactly at 128.
	return (uint8_t)(((int32_t)value + 32768) >> 8);
}

void NxGamepad_encodeBrowserReport(
	const NxGamepadState* state, uint8_t report[NXGP_BROWSER_REPORT_SIZE]) {
	putS16LE(&report[0], state->left_x);
	putS16LE(&report[2], state->left_y);
	putS16LE(&report[4], state->right_x);
	putS16LE(&report[6], state->right_y);
	uint32_t buttons = state->buttons;
	if (state->layout == NXGP_LAYOUT_NINTENDO) {
		buttons &= ~0x0fu;
		if ((state->buttons & (1u << BUTTON_SOUTH)) != 0)
			buttons |= 1u << BUTTON_EAST;
		if ((state->buttons & (1u << BUTTON_EAST)) != 0)
			buttons |= 1u << BUTTON_SOUTH;
		if ((state->buttons & (1u << BUTTON_WEST)) != 0)
			buttons |= 1u << BUTTON_NORTH;
		if ((state->buttons & (1u << BUTTON_NORTH)) != 0)
			buttons |= 1u << BUTTON_WEST;
	}
	report[8] = (uint8_t)(buttons & 0xff);
	report[9] = (uint8_t)(buttons >> 8);
	report[10] = (uint8_t)(buttons >> 16); // upper seven bits are descriptor padding
}

static uint8_t encodeHat(uint32_t buttons) {
	bool up = (buttons & (1u << BUTTON_DPAD_UP)) != 0;
	bool down = (buttons & (1u << BUTTON_DPAD_DOWN)) != 0;
	bool left = (buttons & (1u << BUTTON_DPAD_LEFT)) != 0;
	bool right = (buttons & (1u << BUTTON_DPAD_RIGHT)) != 0;
	int x = right != left ? (right ? 1 : -1) : 0;
	int y = down != up ? (down ? 1 : -1) : 0;
	if (x == 0 && y < 0)
		return 0;
	if (x > 0 && y < 0)
		return 1;
	if (x > 0 && y == 0)
		return 2;
	if (x > 0 && y > 0)
		return 3;
	if (x == 0 && y > 0)
		return 4;
	if (x < 0 && y > 0)
		return 5;
	if (x < 0 && y == 0)
		return 6;
	if (x < 0 && y < 0)
		return 7;
	return 8;
}

static void setPcButton(uint32_t* output, uint32_t buttons, unsigned int source, unsigned int target) {
	if ((buttons & (1u << source)) != 0)
		*output |= 1u << target;
}

void NxGamepad_encodePcReport(const NxGamepadState* state, uint8_t report[NXGP_PC_REPORT_SIZE]) {
	// Match the wired HORIPAD input-only packet used by SDL for 0f0d:00c1:
	// two button bytes, a POV byte, four unsigned stick bytes and one pad byte.
	// Its established host mapping translates raw HOME b12 to standard B16.
	uint32_t buttons = 0;
	if (state->layout == NXGP_LAYOUT_NINTENDO) {
		// Follow the letters printed on the Brick Pro caps: physical A/B/X/Y
		// become host A/B/X/Y even though their positions differ from Xbox.
		setPcButton(&buttons, state->buttons, BUTTON_EAST, 1);
		setPcButton(&buttons, state->buttons, BUTTON_SOUTH, 2);
		setPcButton(&buttons, state->buttons, BUTTON_NORTH, 0);
		setPcButton(&buttons, state->buttons, BUTTON_WEST, 3);
	} else {
		// Follow Xbox positions: bottom/right/left/top become host A/B/X/Y.
		setPcButton(&buttons, state->buttons, BUTTON_SOUTH, 1);
		setPcButton(&buttons, state->buttons, BUTTON_EAST, 2);
		setPcButton(&buttons, state->buttons, BUTTON_WEST, 0);
		setPcButton(&buttons, state->buttons, BUTTON_NORTH, 3);
	}
	setPcButton(&buttons, state->buttons, BUTTON_L1, 4);
	setPcButton(&buttons, state->buttons, BUTTON_R1, 5);
	setPcButton(&buttons, state->buttons, BUTTON_L2, 6);
	setPcButton(&buttons, state->buttons, BUTTON_R2, 7);
	setPcButton(&buttons, state->buttons, BUTTON_SELECT, 8);
	setPcButton(&buttons, state->buttons, BUTTON_START, 9);
	setPcButton(&buttons, state->buttons, BUTTON_L3, 10);
	setPcButton(&buttons, state->buttons, BUTTON_R3, 11);
	setPcButton(&buttons, state->buttons, BUTTON_HOME, 12);

	report[0] = (uint8_t)(buttons & 0xff);
	report[1] = (uint8_t)((buttons >> 8) & 0x3f); // b13 (Capture) is intentionally unused
	report[2] = encodeHat(state->buttons);
	report[3] = signedAxisToU8(state->left_x);
	report[4] = signedAxisToU8(state->left_y);
	report[5] = signedAxisToU8(state->right_x);
	report[6] = signedAxisToU8(state->right_y);
	report[7] = 0;
}
