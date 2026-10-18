#include "gamepad_power.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

bool NxGamepadPower_parseMode(const char* text, NxGamepadPowerMode* mode) {
	if (!text || !mode)
		return false;
	while (*text && isspace((unsigned char)*text))
		text++;
	size_t length = strlen(text);
	while (length > 0 && isspace((unsigned char)text[length - 1]))
		length--;
	if (length == 2 && strncasecmp(text, "pc", 2) == 0) {
		*mode = NXGP_POWER_PC;
		return true;
	}
	if (length == 5 && strncasecmp(text, "phone", 5) == 0) {
		*mode = NXGP_POWER_PHONE;
		return true;
	}
	return false;
}

const char* NxGamepadPower_modeName(NxGamepadPowerMode mode) {
	return mode == NXGP_POWER_PHONE ? "phone" : "pc";
}
