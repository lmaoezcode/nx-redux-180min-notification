###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq ($(filter test clean,$(MAKECMDGOALS)),)
ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=tg5040 make)
endif
ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif
endif

###########################################################

TARGET = usbgamepad
# usblink_power.c is shared with usblink (charger / USB-C role of the PMIC); it is
# compiled from its own directory, not copied.
SOURCE = usbgamepad.c gamepad_mapper.c gamepad_power.c ds4_codec.c ../usblink/usblink_power.c
PRODUCT = build/$(PLATFORM)/$(TARGET).elf
CC = $(CROSS_COMPILE)gcc
CFLAGS += -O2 -Wall -Wextra -std=gnu99 -D_GNU_SOURCE -I../usblink

all:
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)

HOST_CC ?= cc
test:
	mkdir -p build/tests
	$(HOST_CC) tests/test_mapper.c gamepad_mapper.c -o build/tests/test_mapper -O2 -Wall -Wextra -std=c99
	./build/tests/test_mapper
	$(HOST_CC) tests/test_ds4_codec.c gamepad_mapper.c ds4_codec.c -o build/tests/test_ds4_codec -O2 -Wall -Wextra -std=c99
	./build/tests/test_ds4_codec
	$(HOST_CC) tests/test_power_mode.c gamepad_power.c -o build/tests/test_power_mode -O2 -Wall -Wextra -std=gnu99 -D_GNU_SOURCE
	./build/tests/test_power_mode
	sh tests/test_launch_usb_restore.sh
	sh tests/test_launch_power.sh
	sh tests/test_static_abi_check.sh

clean:
	rm -rf build
