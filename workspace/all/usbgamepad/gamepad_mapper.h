#ifndef NX_USB_GAMEPAD_MAPPER_H
#define NX_USB_GAMEPAD_MAPPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NXGP_BROWSER_REPORT_SIZE 11
#define NXGP_PC_REPORT_SIZE 8
#define NXGP_MAX_REPORT_SIZE 11

// Linux input-event values used by TrimUI's virtual "TRIMUI Player1" pad.
// They live here instead of pulling linux/input.h into the mapper so the
// translation logic can be unit-tested on non-Linux development hosts.
enum {
	NXGP_KEY_F1 = 59,
	NXGP_KEY_F2 = 60,
	NXGP_KEY_VOLUMEDOWN = 114,
	NXGP_KEY_VOLUMEUP = 115,
	NXGP_KEY_HOMEPAGE = 172,
	NXGP_BTN_A = 0x130,
	NXGP_BTN_B = 0x131,
	NXGP_BTN_X = 0x133,
	NXGP_BTN_Y = 0x134,
	NXGP_BTN_TL = 0x136,
	NXGP_BTN_TR = 0x137,
	NXGP_BTN_TL2 = 0x138,
	NXGP_BTN_TR2 = 0x139,
	NXGP_BTN_SELECT = 0x13a,
	NXGP_BTN_START = 0x13b,
	NXGP_BTN_MODE = 0x13c,
	NXGP_BTN_THUMBL = 0x13d,
	NXGP_BTN_THUMBR = 0x13e,
};

enum {
	NXGP_ABS_X = 0,
	NXGP_ABS_Y = 1,
	NXGP_ABS_Z = 2,
	NXGP_ABS_RX = 3,
	NXGP_ABS_RY = 4,
	NXGP_ABS_RZ = 5,
	NXGP_ABS_HAT0X = 16,
	NXGP_ABS_HAT0Y = 17,
};

typedef enum {
	// Xbox/position-first: the bottom face button is host A.
	NXGP_LAYOUT_XBOX = 0,
	// Nintendo/label-first: the cap marked A is host A.
	NXGP_LAYOUT_NINTENDO = 1,
} NxGamepadLayout;

/* Canonical physical/positional buttons. These are deliberately independent
 * of browser B-numbers and protocol report bits. */
typedef enum {
	NXGP_BUTTON_SOUTH = 0,
	NXGP_BUTTON_EAST,
	NXGP_BUTTON_WEST,
	NXGP_BUTTON_NORTH,
	NXGP_BUTTON_L1,
	NXGP_BUTTON_R1,
	NXGP_BUTTON_L2,
	NXGP_BUTTON_R2,
	NXGP_BUTTON_SELECT,
	NXGP_BUTTON_START,
	NXGP_BUTTON_L3,
	NXGP_BUTTON_R3,
	NXGP_BUTTON_DPAD_UP,
	NXGP_BUTTON_DPAD_DOWN,
	NXGP_BUTTON_DPAD_LEFT,
	NXGP_BUTTON_DPAD_RIGHT,
	NXGP_BUTTON_HOME,
} NxGamepadButton;

typedef struct {
	int16_t left_x;
	int16_t left_y;
	int16_t right_x;
	int16_t right_y;
	uint32_t buttons;
	bool menu_pressed;
	bool fn1_pressed;
	bool fn2_pressed;
	NxGamepadLayout layout;
} NxGamepadState;

void NxGamepad_init(NxGamepadState* state);
bool NxGamepad_setLayout(NxGamepadState* state, NxGamepadLayout layout);
NxGamepadLayout NxGamepad_toggleLayout(NxGamepadState* state);
NxGamepadLayout NxGamepad_otherLayout(NxGamepadLayout layout);
bool NxGamepad_faceButtonsHeld(const NxGamepadState* state);
void NxGamepad_clearFaceButtons(NxGamepadState* state);
bool NxGamepad_setKey(NxGamepadState* state, uint16_t code, bool pressed);
bool NxGamepad_setAbs(NxGamepadState* state, uint16_t code, int32_t value);
bool NxGamepad_exitChordActive(const NxGamepadState* state);
void NxGamepad_encodeBrowserReport(
	const NxGamepadState* state, uint8_t report[NXGP_BROWSER_REPORT_SIZE]);
void NxGamepad_encodePcReport(const NxGamepadState* state, uint8_t report[NXGP_PC_REPORT_SIZE]);

#endif
