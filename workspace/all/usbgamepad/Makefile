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
SOURCE = usbgamepad.c gamepad_mapper.c ds4_codec.c
PRODUCT = build/$(PLATFORM)/$(TARGET).elf
CC = $(CROSS_COMPILE)gcc
CFLAGS += -O2 -Wall -Wextra -std=gnu99 -D_GNU_SOURCE

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
	sh tests/test_launch_usb_restore.sh
	sh tests/test_static_abi_check.sh

clean:
	rm -rf build
