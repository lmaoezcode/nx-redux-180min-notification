#!/bin/sh
# Independent recovery guard for the destructive part of the H2 FunctionFS
# endpoint test. This script is started detached before the USB gadget changes.

printf '%s\n' 'Disabled: legacy watchdog ignored restore errors and cleared recovery evidence. Do not use it.' >&2
exit 2

set -u
trap '' HUP

TOKEN=/tmp/nx-h2-usb-out.active
LIVE_PID_FILE=/tmp/nx-h2-usb-out-live.pid
PREVIOUS_MODE_FILE=/tmp/nx-h2-usb-out.previous-mode
TEST_BIN=/tmp/usbgamepad-functionfs-out-live
PERSIST_DIR=/mnt/SDCARD/.userdata/tg5040/logs
LOG="$PERSIST_DIR/nx-controller-h2-recovery.txt"
CHECKPOINT_LOG="$PERSIST_DIR/nx-controller-h2-checkpoint.txt"

checkpoint() {
	printf '%s watchdog pid=%s stage=%s udc=%s\n' \
		"$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" "$$" "$1" \
		"$(cat /sys/kernel/config/usb_gadget/g1/UDC 2>/dev/null)" >> "$CHECKPOINT_LOG"
	sync
}

find_coordinator() {
	for recovery_path in \
		/mnt/SDCARD/.system/bin/nx_usb_mode.sh \
		/mnt/SDCARD/.system/tg5040/bin/nx_usb_mode.sh; do
		[ -x "$recovery_path" ] && { printf '%s\n' "$recovery_path"; return 0; }
	done
	return 1
}

run_coordinator_restore() {
	recovery_installed="$(find_coordinator 2>/dev/null)" || return 1
	recovery_runtime="${recovery_installed%/*}"
	if [ -x /tmp/nx_usb_mode_h2.sh ]; then
		NX_USB_RUNTIME_DIR="$recovery_runtime" NX_USB_CHECKPOINT_LOG="$CHECKPOINT_LOG" \
			/tmp/nx_usb_mode_h2.sh restore
	else
		"$recovery_installed" restore
	fi
}

stop_live_process() {
	[ -r "$LIVE_PID_FILE" ] || return 0
	recovery_pid="$(cat "$LIVE_PID_FILE" 2>/dev/null)"
	case "$recovery_pid" in *[!0-9]*|'') return 0 ;; esac
	recovery_exe="$(readlink "/proc/$recovery_pid/exe" 2>/dev/null)"
	[ "$recovery_exe" = "$TEST_BIN" ] || return 0
	kill -TERM "$recovery_pid" 2>/dev/null || true
	for recovery_wait in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		kill -0 "$recovery_pid" 2>/dev/null || return 0
		usleep 100000 2>/dev/null || sleep 1
	done
	kill -KILL "$recovery_pid" 2>/dev/null || true
}

# The normal probe lasts ten seconds. Twenty-five seconds leaves room for USB
# enumeration/retry while still guaranteeing unattended recovery.
mkdir -p "$PERSIST_DIR" || exit 1
checkpoint armed
sleep 25
[ -e "$TOKEN" ] || exit 0

checkpoint recovery_begin
printf '%s watchdog recovery started\n' "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" >> "$LOG"
stop_live_process
checkpoint live_process_stopped
sleep 1
"$TEST_BIN" --restore >> "$LOG" 2>&1 || true
checkpoint binary_restore_finished

previous_mode="$(cat "$PREVIOUS_MODE_FILE" 2>/dev/null)"
if [ "$previous_mode" = mtp ]; then
	run_coordinator_restore >> "$LOG" 2>&1 || true
fi
checkpoint mode_restore_finished
rm -f "$TOKEN" "$LIVE_PID_FILE"
printf '%s watchdog recovery finished previous_mode=%s\n' \
	"$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" "$previous_mode" >> "$LOG"
checkpoint recovery_finished
