// Host test for gamepad_power.c (no Linux or USB access).
#include "../gamepad_power.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static NxGamepadPowerMode parse(const char* text, bool* ok) {
	NxGamepadPowerMode mode = (NxGamepadPowerMode)99;
	*ok = NxGamepadPower_parseMode(text, &mode);
	return mode;
}

int main(void) {
	bool ok;

	assert(parse("pc", &ok) == NXGP_POWER_PC && ok);
	assert(parse("phone", &ok) == NXGP_POWER_PHONE && ok);
	assert(parse("phone\n", &ok) == NXGP_POWER_PHONE && ok);
	assert(parse("  PHONE \r\n", &ok) == NXGP_POWER_PHONE && ok);
	assert(parse("Pc", &ok) == NXGP_POWER_PC && ok);
	assert(parse("\tpc\t", &ok) == NXGP_POWER_PC && ok);

	// Anything else is rejected and must not touch the output, so a caller that
	// pre-sets the default keeps it.
	assert(parse("", &ok) == (NxGamepadPowerMode)99 && !ok);
	assert(parse("   \n", &ok) == (NxGamepadPowerMode)99 && !ok);
	assert(parse("phones", &ok) == (NxGamepadPowerMode)99 && !ok);
	assert(parse("ph", &ok) == (NxGamepadPowerMode)99 && !ok);
	assert(parse("pc phone", &ok) == (NxGamepadPowerMode)99 && !ok);
	assert(parse("phone=1", &ok) == (NxGamepadPowerMode)99 && !ok);
	assert(parse("android", &ok) == (NxGamepadPowerMode)99 && !ok);

	NxGamepadPowerMode mode = NXGP_POWER_PC;
	assert(!NxGamepadPower_parseMode(NULL, &mode) && mode == NXGP_POWER_PC);
	assert(!NxGamepadPower_parseMode("phone", NULL));

	assert(strcmp(NxGamepadPower_modeName(NXGP_POWER_PC), "pc") == 0);
	assert(strcmp(NxGamepadPower_modeName(NXGP_POWER_PHONE), "phone") == 0);

	puts("usbgamepad power-mode tests passed");
	return 0;
}
