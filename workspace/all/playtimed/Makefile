PLATFORM ?= $(UNION_PLATFORM)
ifeq (,$(PLATFORM))
$(error please specify PLATFORM)
endif
CC = $(CROSS_COMPILE)gcc
PRODUCT = build/$(PLATFORM)/playtimed.elf

.PHONY: all clean test
all:
	mkdir -p build/$(PLATFORM)
	$(CC) daily.c playtimed.c -std=gnu99 -O2 -Wall -Wextra -Werror -o $(PRODUCT)

test:
	mkdir -p build/tests
	$(CC) daily.c tests/test_daily.c -std=gnu99 -Wall -Wextra -Werror -o build/tests/test_daily
	./build/tests/test_daily

clean:
	rm -f $(PRODUCT)
