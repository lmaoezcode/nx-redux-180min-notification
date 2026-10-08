#!/bin/sh

# This helper is pushed to /tmp during development.  The timed mode restores
# the stock ADB gadget by itself even though launching it temporarily removes
# the ADB transport used to start the test.
SECONDS_TO_RUN="${1:-20}"
exec /tmp/usbgamepad-functionfs-test --test-seconds "$SECONDS_TO_RUN" \
	</dev/null >/tmp/usbgamepad-live-test.txt 2>&1
