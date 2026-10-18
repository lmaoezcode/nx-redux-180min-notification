#ifndef NX_USB_GAMEPAD_POWER_H
#define NX_USB_GAMEPAD_POWER_H

#include <stdbool.h>

// How the gamepad treats the USB-C port while it is a controller.
//   pc    (default) change nothing: the port keeps its stock charging and role
//         behaviour, exactly as before this option existed.
//   phone while the controller runs, stop charging from the host, cap the
//         input current at its minimum and hold the port as a USB device
//         (usblink_power.c does the register work); always put everything back
//         on exit. For hosts that supply little current or fight over the role.
typedef enum {
	NXGP_POWER_PC = 0,
	NXGP_POWER_PHONE = 1,
} NxGamepadPowerMode;

// Parses "pc" or "phone" (case-insensitive, surrounding whitespace and a
// trailing newline ignored). Returns false and leaves *mode untouched for
// anything else, including NULL and the empty string.
bool NxGamepadPower_parseMode(const char* text, NxGamepadPowerMode* mode);

const char* NxGamepadPower_modeName(NxGamepadPowerMode mode);

#endif
