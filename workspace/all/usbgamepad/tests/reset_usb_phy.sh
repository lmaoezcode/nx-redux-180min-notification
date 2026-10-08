#!/bin/sh

ROLE_ROOT=/sys/devices/platform/soc/usbc0

# These vendor sysfs nodes perform an action when read.  usb_null drops the
# physical role; usb_device brings the OTG PHY back in peripheral mode.
cat "$ROLE_ROOT/usb_null" >/dev/null 2>&1
sleep 1
cat "$ROLE_ROOT/usb_device" >/dev/null 2>&1
sleep 1
/bin/setusbconfig adb >/tmp/reset-usb-phy.log 2>&1
