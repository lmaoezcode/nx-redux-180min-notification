#!/bin/sh
# Read-only hardware inventory for NX Controller research on TrimUI Brick Pro.
#
# This script deliberately does not bind/unbind USB, start/stop Bluetooth,
# change rfkill, touch the motor, alter the display, or read Bluetooth link
# keys. It is safe to stream over ADB without copying it to the handheld:
#
#   adb shell sh -s < scripts/probes/brickpro-controller-audit.sh

set -u

section() {
	printf '\n=== %s ===\n' "$1"
}

read_node() {
	for audit_path in "$@"; do
		if [ -e "$audit_path" ]; then
			printf '%s: ' "$audit_path"
			cat "$audit_path" 2>&1 || true
		fi
	done
}

hash_file() {
	audit_path=$1
	[ -f "$audit_path" ] || return 0
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$audit_path" 2>&1
	elif command -v md5sum >/dev/null 2>&1; then
		printf 'sha256sum unavailable; md5 only: '
		md5sum "$audit_path" 2>&1
	else
		ls -ln "$audit_path" 2>&1
	fi
}

command_version() {
	audit_cmd=$1
	shift
	if command -v "$audit_cmd" >/dev/null 2>&1; then
		printf '%s: ' "$audit_cmd"
		"$audit_cmd" "$@" 2>&1 | head -n 2 || true
	else
		printf '%s: unavailable\n' "$audit_cmd"
	fi
}

redact_stream() {
	sed \
		-e 's/\([[:xdigit:]][[:xdigit:]]:\)\{5\}[[:xdigit:]][[:xdigit:]]/<redacted-mac>/g' \
		-e 's/androidboot.serialno=[^ ]*/androidboot.serialno=<redacted>/g'
}

printf 'NX Controller Brick Pro hardware audit v2\n'
printf 'read_only=yes link_keys_collected=no\n'
date 2>&1 || true

section "system identity"
uname -a 2>&1 || true
read_node /tmp/device /etc/os-release /proc/device-tree/model
if [ -r /proc/cmdline ]; then
	printf '/proc/cmdline: '
	redact_stream </proc/cmdline
fi
if [ -r /proc/device-tree/compatible ]; then
	printf '/proc/device-tree/compatible: '
	tr '\000' ' ' </proc/device-tree/compatible 2>/dev/null || true
	printf '\n'
fi
if [ -r /usr/trimui/bin/MainUI ]; then
	strings /usr/trimui/bin/MainUI 2>/dev/null | grep '^Trimui' | head -n 5 || true
fi

section "binary identity"
for audit_path in \
	/usr/trimui/bin/trimui_inputd \
	/bin/setusbconfig \
	/usr/bin/bluetoothd \
	/usr/libexec/bluetooth/bluetoothd \
	/usr/bin/hciattach \
	/bin/hciattach \
	/mnt/SDCARD/.system/tg5040/bin/usbgamepad.elf \
	/mnt/SDCARD/.system/tg5040/paks/Tools/USB\ Gamepad.pak/usbgamepad.elf
do
	hash_file "$audit_path"
done
command_version bluetoothd --version
command_version hciattach -h
command_version cc --version
command_version gcc --version

section "relevant kernel configuration"
if [ -r /proc/config.gz ] && command -v zcat >/dev/null 2>&1; then
	zcat /proc/config.gz 2>/dev/null |
		grep -E '^((# )?CONFIG_(USB|HID|INPUT|BT|RFKILL|LEDS|POWER_SUPPLY|I2C).*)' |
		grep -E '(GADGET|CONFIGFS|FUNCTIONFS|F_FS|F_HID|HID|UINPUT|EVDEV|BT_HIDP|BT_RFCOMM|BT_BNEP|BT_HCIUART|XRADIO|RFKILL|LEDS|POWER_SUPPLY|I2C)' || true
else
	printf '/proc/config.gz unavailable\n'
fi

section "loaded modules"
cat /proc/modules 2>&1 || true

section "USB device controller"
if [ -d /sys/class/udc ]; then
	ls -la /sys/class/udc 2>&1 || true
	for audit_udc in /sys/class/udc/*; do
		[ -e "$audit_udc" ] || continue
		printf '\n-- %s --\n' "$audit_udc"
		readlink -f "$audit_udc" 2>&1 || true
		read_node \
			"$audit_udc/state" \
			"$audit_udc/current_speed" \
			"$audit_udc/maximum_speed" \
			"$audit_udc/supported_speeds" \
			"$audit_udc/uevent"
	done
else
	printf '/sys/class/udc unavailable\n'
fi

section "USB gadget configuration"
if [ -d /sys/kernel/config/usb_gadget ]; then
	for audit_gadget in /sys/kernel/config/usb_gadget/*; do
		[ -d "$audit_gadget" ] || continue
		printf '\n-- %s --\n' "$audit_gadget"
		read_node \
			"$audit_gadget/UDC" \
			"$audit_gadget/idVendor" \
			"$audit_gadget/idProduct" \
			"$audit_gadget/bcdUSB" \
			"$audit_gadget/bcdDevice" \
			"$audit_gadget/bDeviceClass" \
			"$audit_gadget/bDeviceSubClass" \
			"$audit_gadget/bDeviceProtocol"
		printf '%s\n' '-- functions --'
		ls -la "$audit_gadget/functions" 2>&1 || true
		printf '%s\n' '-- configuration links --'
		for audit_config in "$audit_gadget"/configs/*; do
			[ -d "$audit_config" ] || continue
			ls -la "$audit_config" 2>&1 || true
		done
	done
else
	printf '/sys/kernel/config/usb_gadget unavailable\n'
fi
grep -E 'functionfs|configfs|ffs' /proc/mounts 2>/dev/null || true

section "USB and gadget kernel log"
dmesg 2>/dev/null | redact_stream | grep -Ei 'usb|udc|gadget|functionfs|ffs|musb|sunxi' | tail -n 240 || true
grep -Ei 'usb|udc|musb|sunxi|xradio|xr829|hci|input' /proc/interrupts 2>/dev/null || true

section "input inventory"
cat /proc/bus/input/devices 2>&1 || true
for audit_event in /sys/class/input/event*; do
	[ -e "$audit_event" ] || continue
	printf '\n-- %s --\n' "$audit_event"
	read_node \
		"$audit_event/device/name" \
		"$audit_event/device/phys" \
		"$audit_event/device/uniq" \
		"$audit_event/device/id/bustype" \
		"$audit_event/device/id/vendor" \
		"$audit_event/device/id/product" \
		"$audit_event/device/id/version" \
		"$audit_event/device/capabilities/ev" \
		"$audit_event/device/capabilities/key" \
		"$audit_event/device/capabilities/abs" \
		"$audit_event/device/capabilities/sw"
done

section "I2C input hardware"
ls -la /dev/i2c-* 2>&1 || true
for audit_i2c in /sys/class/i2c-dev/i2c-* /sys/bus/i2c/devices/3-0028 /sys/bus/i2c/devices/3-0029; do
	[ -e "$audit_i2c" ] || continue
	printf '\n-- %s --\n' "$audit_i2c"
	readlink -f "$audit_i2c" 2>&1 || true
	read_node "$audit_i2c/name" "$audit_i2c/modalias" "$audit_i2c/uevent"
done

section "Bluetooth controller and ownership"
ps 2>&1 | grep -E 'bluetoothd|bluealsa|hciattach|bluetoothctl|wpa_supplicant' | grep -v grep || true
if command -v hciconfig >/dev/null 2>&1; then
	# The public adapter address is intentionally redacted; it is not needed to
	# identify the chip/driver and makes shared diagnostic reports safer.
	hciconfig -a 2>&1 | sed 's/BD Address: [^ ]*/BD Address: REDACTED/' || true
else
	printf 'hciconfig unavailable\n'
fi
for audit_hci in /sys/class/bluetooth/hci*; do
	[ -e "$audit_hci" ] || continue
	printf '\n-- %s --\n' "$audit_hci"
	read_node \
		"$audit_hci/name" \
		"$audit_hci/uevent" \
		"$audit_hci/device/uevent" \
		"$audit_hci/device/modalias" \
		"$audit_hci/device/driver/module/version"
done
for audit_rfkill in /sys/class/rfkill/rfkill*; do
	[ -e "$audit_rfkill" ] || continue
	printf '\n-- %s --\n' "$audit_rfkill"
	read_node \
		"$audit_rfkill/name" \
		"$audit_rfkill/type" \
		"$audit_rfkill/state" \
		"$audit_rfkill/soft" \
		"$audit_rfkill/hard"
done
dmesg 2>/dev/null | redact_stream | grep -Ei 'bluetooth|xradio|xr829|hci|rfkill|ttyS1' | tail -n 240 || true

section "motor, screen, LED and battery nodes"
read_node \
	/sys/class/motor/voltage \
	/sys/class/gpio/gpio227/value \
	/sys/class/backlight/*/brightness \
	/sys/class/backlight/*/max_brightness \
	/sys/class/power_supply/*/type \
	/sys/class/power_supply/*/status \
	/sys/class/power_supply/*/capacity \
	/sys/class/power_supply/*/voltage_now \
	/sys/class/power_supply/*/current_now
for audit_led in /sys/class/leds/*; do
	[ -d "$audit_led" ] || continue
	printf '\n-- %s --\n' "$audit_led"
	read_node "$audit_led/brightness" "$audit_led/max_brightness" "$audit_led/trigger"
done

section "controller-related process state"
ps 2>&1 | grep -E 'trimui_inputd|keymon|osdd|nextui|usbgamepad|umtpr|usb_storage' | grep -v grep || true
read_node /tmp/stay_awake /tmp/device

section "mounts"
cat /proc/mounts 2>&1 || true

section "audit end"
printf 'No settings or hardware state were changed by this script.\n'
