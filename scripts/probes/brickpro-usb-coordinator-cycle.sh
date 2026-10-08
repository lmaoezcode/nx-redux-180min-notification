#!/bin/sh
# Isolate the NX USB coordinator from the FunctionFS gamepad. The process is
# launched through brickpro-detached-launch and uses the same persistent
# checkpoints/recovery guard as H2.

printf '%s\n' 'Disabled: this cycle forces MTP and its recovery did not restore USB. Preserve the actual snapshot.' >&2
exit 2

set -u
trap '' HUP

PERSIST_DIR=/mnt/SDCARD/.userdata/tg5040/logs
LOG="$PERSIST_DIR/nx-controller-usb-cycle.txt"
CHECKPOINT_LOG="$PERSIST_DIR/nx-controller-h2-checkpoint.txt"
TOKEN=/tmp/nx-h2-usb-out.active
PREVIOUS_MODE_FILE=/tmp/nx-h2-usb-out.previous-mode
WATCHDOG=/tmp/brickpro-usb-out-recovery.sh
WATCHDOG_PID_FILE=/tmp/nx-h2-usb-out-watchdog.pid

checkpoint() {
	printf '%s cycle pid=%s stage=%s udc=%s\n' \
		"$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" "$$" "$1" \
		"$(cat /sys/kernel/config/usb_gadget/g1/UDC 2>/dev/null)" >> "$CHECKPOINT_LOG"
	sync
}

find_coordinator() {
	for cycle_path in \
		/mnt/SDCARD/.system/bin/nx_usb_mode.sh \
		/mnt/SDCARD/.system/tg5040/bin/nx_usb_mode.sh; do
		[ -x "$cycle_path" ] && { printf '%s\n' "$cycle_path"; return 0; }
	done
	return 1
}

mkdir -p "$PERSIST_DIR" || exit 1
: > "$LOG"
: > "$CHECKPOINT_LOG"
installed_coordinator="$(find_coordinator 2>/dev/null)" || exit 1
coordinator_runtime="${installed_coordinator%/*}"
coordinator=/tmp/nx_usb_mode_h2.sh
[ -x "$coordinator" ] || coordinator="$installed_coordinator"
run_coordinator() {
	NX_USB_RUNTIME_DIR="$coordinator_runtime" NX_USB_CHECKPOINT_LOG="$CHECKPOINT_LOG" \
		"$coordinator" "$@"
}

initial_mode="$(run_coordinator status 2>/dev/null)"
case "$initial_mode" in mtp|adb) ;; *) exit 1 ;; esac
# The interrupted H2 run can leave the post-reboot baseline at ADB-only even
# though the captured pre-test state was MTP. Repair that expected normal state
# first, then prove the complete MTP -> ADB -> MTP cycle.
previous_mode=mtp
printf '%s\n' "$previous_mode" > "$PREVIOUS_MODE_FILE"
: > "$TOKEN"
checkpoint cycle_begin

start-stop-daemon -S -b -m -p "$WATCHDOG_PID_FILE" -x "$WATCHDOG" || exit 1
checkpoint watchdog_started
if [ "$initial_mode" = adb ]; then
	checkpoint baseline_mtp_restore_begin
	if ! run_coordinator restore >> "$LOG" 2>&1; then
		checkpoint baseline_mtp_restore_failed
		exit 1
	fi
	checkpoint baseline_mtp_restore_end
fi
if ! run_coordinator stop >> "$LOG" 2>&1; then
	checkpoint stop_failed
	exit 1
fi
checkpoint stop_finished
sleep 2

if [ "$previous_mode" = mtp ]; then
	if ! run_coordinator restore >> "$LOG" 2>&1; then
		checkpoint restore_failed
		exit 1
	fi
fi
checkpoint restore_finished
final_mode="$(run_coordinator status 2>/dev/null)"
printf 'initial=%s expected=%s final=%s\n' "$initial_mode" "$previous_mode" "$final_mode" >> "$LOG"
[ "$final_mode" = "$previous_mode" ] || { checkpoint verify_failed; exit 1; }

rm -f "$TOKEN"
if [ -r "$WATCHDOG_PID_FILE" ]; then
	watchdog_pid="$(cat "$WATCHDOG_PID_FILE" 2>/dev/null)"
	case "$watchdog_pid" in *[!0-9]*|'') ;; *) kill -TERM "$watchdog_pid" 2>/dev/null || true ;; esac
fi
rm -f "$WATCHDOG_PID_FILE"
checkpoint cycle_finished
