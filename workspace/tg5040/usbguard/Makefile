TARGET = nx_usb_guard.elf
CC = $(CROSS_COMPILE)gcc
CFLAGS = -std=c11 -Os -Wall -Wextra -Werror

all: $(TARGET)

$(TARGET): usbguard.c
	$(CC) $(CFLAGS) -static -o $@ $<

clean:
	rm -f $(TARGET)
