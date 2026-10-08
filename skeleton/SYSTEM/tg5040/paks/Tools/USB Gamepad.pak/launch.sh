#!/bin/sh

cd "$(dirname "$0")" || exit 1

# NextUI puts application logs in persistent .userdata only when Developer →
# Debug logging is enabled. Otherwise LOGS_PATH points at the tmpfs log area.
# Keep the same fallback when the pak is launched outside the normal shell.
LOG_DIR="${LOGS_PATH:-/tmp/nx-logs}"
LOG_FILE="$LOG_DIR/usb-gamepad.txt"
LAYOUT_FILE="${USERDATA_PATH:-/mnt/SDCARD/.userdata/tg5040}/usb-gamepad-layout"
ACTIVE_MARKER=/tmp/nx_usb_gamepad_active
GADGET_UDC=/sys/kernel/config/usb_gadget/g1/UDC
USB_ROLE_ROOT=/sys/devices/platform/soc/usbc0
SHOW_PID=""
KEYMON_WAS_RUNNING=0
CLEANED_UP=0
STOCK_INPUTD=/usr/trimui/bin/trimui_inputd
FAST_INPUTD_TMP=/tmp/nx_trimui_inputd_4ms
FAST_INPUTD_PID=""
FAST_INPUTD_WATCHDOG_PID=""
STOCK_INPUTD_WAS_RUNNING=0
INPUTD_PROFILE=stock-16ms

restore_stock_inputd() {
	if [ -n "$FAST_INPUTD_WATCHDOG_PID" ] && kill -0 "$FAST_INPUTD_WATCHDOG_PID" 2>/dev/null; then
		kill -TERM "$FAST_INPUTD_WATCHDOG_PID" 2>/dev/null
	fi
	FAST_INPUTD_WATCHDOG_PID=""
	if [ -n "$FAST_INPUTD_PID" ] && kill -0 "$FAST_INPUTD_PID" 2>/dev/null; then
		kill -TERM "$FAST_INPUTD_PID" 2>/dev/null
		usleep 100000
		kill -9 "$FAST_INPUTD_PID" 2>/dev/null
	fi
	FAST_INPUTD_PID=""
	rm -f "$FAST_INPUTD_TMP"

	if [ "$STOCK_INPUTD_WAS_RUNNING" = "1" ] && ! pidof trimui_inputd >/dev/null 2>&1; then
		"$STOCK_INPUTD" >> "$LOG_FILE" 2>&1 &
		usleep 300000
	fi
	STOCK_INPUTD_WAS_RUNNING=0
}

enable_fast_inputd() {
	# This is a guarded, temporary patch of the exact Brick Pro daemon tested
	# below. Never overwrite /usr: copy it to tmpfs, replace only the ARM64
	# `mov w0, #16000` before usleep(), and verify the complete patched image.
	[ -x "$STOCK_INPUTD" ] || {
		printf '%s\n' "usbgamepad: stock input daemon is unavailable; keeping stock polling" >> "$LOG_FILE"
		return 1
	}

	STOCK_SUM="$(md5sum "$STOCK_INPUTD" 2>/dev/null | awk '{print $1}')"
	if [ "$STOCK_SUM" != "bc0dc91dc32d1a5b949901d46fa790e2" ]; then
		printf '%s\n' "usbgamepad: unknown trimui_inputd md5=$STOCK_SUM; refusing polling patch" >> "$LOG_FILE"
		return 1
	fi

	cp "$STOCK_INPUTD" "$FAST_INPUTD_TMP" || return 1
	# File offset 0x2108: 00 d0 87 52 = mov w0,#16000.
	# Replacement:          00 f4 81 52 = mov w0,#4000.
	printf '\000\364\201\122' | dd of="$FAST_INPUTD_TMP" bs=1 seek=8456 conv=notrunc 2>/dev/null || {
		rm -f "$FAST_INPUTD_TMP"
		return 1
	}
	chmod 755 "$FAST_INPUTD_TMP"
	FAST_SUM="$(md5sum "$FAST_INPUTD_TMP" 2>/dev/null | awk '{print $1}')"
	if [ "$FAST_SUM" != "b47501ad15189563e8034cce58f7e693" ]; then
		printf '%s\n' "usbgamepad: patched input daemon verification failed md5=$FAST_SUM" >> "$LOG_FILE"
		rm -f "$FAST_INPUTD_TMP"
		return 1
	fi

	STOCK_PIDS="$(pidof trimui_inputd 2>/dev/null)"
	if [ -n "$STOCK_PIDS" ]; then
		STOCK_INPUTD_WAS_RUNNING=1
		kill -TERM $STOCK_PIDS 2>/dev/null
		WAIT_COUNT=0
		while pidof trimui_inputd >/dev/null 2>&1 && [ "$WAIT_COUNT" -lt 10 ]; do
			usleep 50000
			WAIT_COUNT=$((WAIT_COUNT + 1))
		done
		REMAINING_PIDS="$(pidof trimui_inputd 2>/dev/null)"
		[ -n "$REMAINING_PIDS" ] && kill -9 $REMAINING_PIDS 2>/dev/null
		usleep 100000
	fi

	"$FAST_INPUTD_TMP" >> "$LOG_FILE" 2>&1 &
	FAST_INPUTD_PID=$!
	usleep 300000
	if ! kill -0 "$FAST_INPUTD_PID" 2>/dev/null; then
		printf '%s\n' "usbgamepad: 4ms input daemon failed; restoring stock daemon" >> "$LOG_FILE"
		restore_stock_inputd
		return 1
	fi

	INPUTD_PROFILE=experimental-4ms
	LAUNCHER_PID=$$
	(
		while kill -0 "$LAUNCHER_PID" 2>/dev/null && kill -0 "$FAST_INPUTD_PID" 2>/dev/null; do
			sleep 1
		done
		# Traps cover normal exits. This independent watcher also repairs input
		# if the pak launcher itself is killed abruptly.
		if ! kill -0 "$LAUNCHER_PID" 2>/dev/null; then
			kill -TERM "$FAST_INPUTD_PID" 2>/dev/null
			usleep 100000
			kill -9 "$FAST_INPUTD_PID" 2>/dev/null
			rm -f "$FAST_INPUTD_TMP"
			if ! pidof trimui_inputd >/dev/null 2>&1; then
				"$STOCK_INPUTD" >> "$LOG_FILE" 2>&1 &
			fi
		fi
	) &
	FAST_INPUTD_WATCHDOG_PID=$!
	printf '%s\n' "usbgamepad: input daemon profile=$INPUTD_PROFILE pid=$FAST_INPUTD_PID" >> "$LOG_FILE"
	return 0
}

cleanup() {
	[ "$CLEANED_UP" = "1" ] && return
	CLEANED_UP=1

	if [ -n "$SHOW_PID" ] && kill -0 "$SHOW_PID" 2>/dev/null; then
		kill -INT "$SHOW_PID" 2>/dev/null
		usleep 100000
		kill -9 "$SHOW_PID" 2>/dev/null
	fi

	# Normally usbgamepad.elf restores ADB itself. This path catches a crash,
	# SIGKILL, or a failed early setup and keeps the data port recoverable.
	if [ -e "$ACTIVE_MARKER" ]; then
		if ./usbgamepad.elf --restore >> "$LOG_FILE" 2>&1; then
			rm -f "$ACTIVE_MARKER"
		else
			[ -e "$GADGET_UDC" ] && printf '\n' > "$GADGET_UDC" 2>/dev/null
			[ -r "$USB_ROLE_ROOT/usb_null" ] && cat "$USB_ROLE_ROOT/usb_null" >/dev/null 2>&1
			sleep 1
			[ -r "$USB_ROLE_ROOT/usb_device" ] && cat "$USB_ROLE_ROOT/usb_device" >/dev/null 2>&1
			sleep 1
			if /bin/setusbconfig adb >> "$LOG_FILE" 2>&1; then
				rm -f "$ACTIVE_MARKER"
			fi
		fi
	fi

	rm -f /tmp/stay_awake
	touch /tmp/hide_osdd
	restore_stock_inputd
	if [ "$KEYMON_WAS_RUNNING" = "1" ] && ! pidof keymon.elf >/dev/null 2>&1; then
		keymon.elf >/dev/null 2>&1 &
	fi
	nx_usb_mode.sh restore >> "$LOG_FILE" 2>&1 || true
}

trap cleanup EXIT HUP INT TERM

mkdir -p "$(dirname "$LOG_FILE")"
mkdir -p "$(dirname "$LAYOUT_FILE")"
touch /tmp/stay_awake /tmp/hide_osdd

# The bottom port has one device controller, so MTP and the gamepad cannot own
# it simultaneously. Cleanup restores the normal ADB + MTP data mode.
nx_usb_mode.sh stop >> "$LOG_FILE" 2>&1 || true

INITIAL_LAYOUT=xbox
if [ -r "$LAYOUT_FILE" ]; then
	read -r SAVED_LAYOUT < "$LAYOUT_FILE"
	case "$SAVED_LAYOUT" in
		xbox|nintendo) INITIAL_LAYOUT="$SAVED_LAYOUT" ;;
	esac
fi
printf '%s\n' "usbgamepad pak build: polling-test-v13-20261002 profile=pc persistent_layout=$INITIAL_LAYOUT" >> "$LOG_FILE"

# The binary talks directly to /dev/disp so raw 0 really switches the panel
# backlight off. Give it the current UI brightness for exact restoration.
BRIGHTNESS_LEVEL="$(osdctl.elf get brightness 2>/dev/null)"
case "$BRIGHTNESS_LEVEL" in
	0) NXGP_SCREEN_BRIGHTNESS_RAW=1 ;;
	1) NXGP_SCREEN_BRIGHTNESS_RAW=8 ;;
	2) NXGP_SCREEN_BRIGHTNESS_RAW=16 ;;
	3) NXGP_SCREEN_BRIGHTNESS_RAW=32 ;;
	4) NXGP_SCREEN_BRIGHTNESS_RAW=48 ;;
	5) NXGP_SCREEN_BRIGHTNESS_RAW=72 ;;
	6) NXGP_SCREEN_BRIGHTNESS_RAW=96 ;;
	7) NXGP_SCREEN_BRIGHTNESS_RAW=128 ;;
	8) NXGP_SCREEN_BRIGHTNESS_RAW=160 ;;
	9) NXGP_SCREEN_BRIGHTNESS_RAW=192 ;;
	10) NXGP_SCREEN_BRIGHTNESS_RAW=255 ;;
	*) NXGP_SCREEN_BRIGHTNESS_RAW=16 ;;
esac
# The emulated wired HORIPAD 0f0d:00c1 is an input-only controller: it has no
# host-controlled rumble protocol. Keep the local motor disabled in this stable
# compatibility profile; an output-capable protocol belongs in a separate
# experimental profile so it cannot break enumeration or Steam/SDL mappings.
NXGP_RUMBLE_ENABLED=0
NXGP_RUMBLE_STRENGTH=0
NXGP_LAYOUT_STATE_FILE="$LAYOUT_FILE"
export NXGP_SCREEN_BRIGHTNESS_RAW NXGP_RUMBLE_ENABLED NXGP_RUMBLE_STRENGTH NXGP_LAYOUT_STATE_FILE

KEYMON_PID="$(pidof keymon.elf 2>/dev/null)"
if [ -n "$KEYMON_PID" ]; then
	KEYMON_WAS_RUNNING=1
	kill $KEYMON_PID 2>/dev/null
	usleep 100000
fi

# Try the verified 4ms input scan while this tool is active. Unsupported
# firmware revisions safely continue with the original 16ms daemon.
enable_fast_inputd || true

show2.elf \
	--mode=daemon \
	--image="$SYSTEM_PATH/res/logo.png" \
	--bgcolor=0x000000 \
	--fontcolor=0xFFFFFF \
	--text="Preparing USB gamepad ($INPUTD_PROFILE)..." \
	--hint="FN1 layout - FN2 screen - MENU + SELECT exit" \
	--texty=78 \
	--fontsize=28 >> "$LOG_FILE" 2>&1 &
SHOW_PID=$!

./usbgamepad.elf --profile pc --layout "$INITIAL_LAYOUT" >> "$LOG_FILE" 2>&1
RESULT=$?

cleanup
trap - EXIT HUP INT TERM

if [ "$RESULT" -ne 0 ]; then
	show2.elf \
		--mode=progress \
		--image="$SYSTEM_PATH/res/logo.png" \
		--bgcolor=0x000000 \
		--fontcolor=0xFF6666 \
		--text="USB gamepad could not start" \
		--hint="Log: $LOG_FILE" \
		--timeout=5 \
		--texty=78 \
		--progressy=95 >> "$LOG_FILE" 2>&1
fi

exit "$RESULT"
