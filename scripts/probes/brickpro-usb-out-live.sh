#!/bin/sh
# Timed H2 live-bind test. It snapshots the current NX USB data mode, stops
# MTP through the coordinator, runs a lab-only HID IN+OUT profile, then restores
# exactly the previous adb/mtp mode. A separate detached recovery guard handles
# parent death and lost ADB.

printf '%s\n' 'Disabled: legacy H2 recovery failed. Read the audit; use an independent log/recovery channel first.' >&2
exit 2

set -u
trap '' HUP

TEST_BIN=/tmp/usbgamepad-functionfs-out-live
WATCHDOG=/tmp/brickpro-usb-out-recovery.sh
EXPECTED_SHA256=0703b8b90505afd1ee2369d07fdc1466f48a95481fc2c0297486d803bf9cf719
TOKEN=/tmp/nx-h2-usb-out.active
MAIN_PID_FILE=/tmp/nx-h2-usb-out-main.pid
LIVE_PID_FILE=/tmp/nx-h2-usb-out-live.pid
WATCHDOG_PID_FILE=/tmp/nx-h2-usb-out-watchdog.pid
PREVIOUS_MODE_FILE=/tmp/nx-h2-usb-out.previous-mode
PERSIST_DIR=/mnt/SDCARD/.userdata/tg5040/logs
LOG="$PERSIST_DIR/nx-controller-h2-live.txt"
CHECKPOINT_LOG="$PERSIST_DIR/nx-controller-h2-checkpoint.txt"
RECOVERY_LOG="$PERSIST_DIR/nx-controller-h2-recovery.txt"

checkpoint() {
	printf '%s main pid=%s stage=%s udc=%s\n' \
		"$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" "$$" "$1" \
		"$(cat /sys/kernel/config/usb_gadget/g1/UDC 2>/dev/null)" >> "$CHECKPOINT_LOG"
	sync
}

find_coordinator() {
	for live_path in \
		/mnt/SDCARD/.system/bin/nx_usb_mode.sh \
		/mnt/SDCARD/.system/tg5040/bin/nx_usb_mode.sh; do
		[ -x "$live_path" ] && { printf '%s\n' "$live_path"; return 0; }
	done
	return 1
}

run_coordinator() {
	NX_USB_RUNTIME_DIR="$coordinator_runtime" NX_USB_CHECKPOINT_LOG="$CHECKPOINT_LOG" \
		"$coordinator" "$@"
}

restore_previous_mode() {
	checkpoint restore_binary_begin
	"$TEST_BIN" --restore >> "$LOG" 2>&1 || true
	checkpoint restore_binary_end
	if [ "$previous_mode" = mtp ]; then
		checkpoint restore_mtp_begin
		run_coordinator restore >> "$LOG" 2>&1 || return 1
		checkpoint restore_mtp_end
	fi
	return 0
}

mkdir -p "$PERSIST_DIR" || exit 1
: > "$LOG"
: > "$CHECKPOINT_LOG"
: > "$RECOVERY_LOG"
checkpoint preflight_begin

sha256="$(sha256sum "$TEST_BIN" 2>/dev/null | awk '{print $1}')"
[ "$sha256" = "$EXPECTED_SHA256" ] || {
	printf 'refusing H2 live test: unexpected binary checksum %s\n' "$sha256" > "$LOG"
	exit 1
}
[ -x "$WATCHDOG" ] || { echo 'refusing H2 live test: recovery guard missing' > "$LOG"; exit 1; }
[ ! -e "$TOKEN" ] || { echo 'refusing H2 live test: another session is active' > "$LOG"; exit 1; }

installed_coordinator="$(find_coordinator 2>/dev/null)" || {
	echo 'refusing H2 live test: NX USB coordinator missing' > "$LOG"
	exit 1
}
coordinator_runtime="${installed_coordinator%/*}"
if [ -x /tmp/nx_usb_mode_h2.sh ]; then
	coordinator=/tmp/nx_usb_mode_h2.sh
else
	coordinator="$installed_coordinator"
fi
previous_mode="$(run_coordinator status 2>/dev/null)"
case "$previous_mode" in
	mtp|adb) ;;
	storage) echo 'refusing H2 live test: USB Storage owns the gadget' > "$LOG"; exit 1 ;;
	*) echo "refusing H2 live test: unknown USB mode '$previous_mode'" > "$LOG"; exit 1 ;;
esac

printf '%s H2 start previous_mode=%s sha256=%s\n' \
	"$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" "$previous_mode" "$sha256" >> "$LOG"
printf '%s\n' "$previous_mode" > "$PREVIOUS_MODE_FILE"
printf '%s\n' "$$" > "$MAIN_PID_FILE"
: > "$TOKEN"
checkpoint session_marked

start-stop-daemon -S -b -m -p "$WATCHDOG_PID_FILE" -x "$WATCHDOG" || {
	echo 'could not start detached recovery guard' >> "$LOG"
	rm -f "$TOKEN" "$MAIN_PID_FILE"
	exit 1
}
checkpoint watchdog_started

checkpoint coordinator_stop_begin
if ! run_coordinator stop >> "$LOG" 2>&1; then
	checkpoint coordinator_stop_failed
	echo 'NX USB coordinator could not stop MTP safely' >> "$LOG"
	restore_previous_mode || true
	rm -f "$TOKEN" "$MAIN_PID_FILE"
	exit 1
fi
checkpoint coordinator_stop_end

checkpoint live_binary_begin
"$TEST_BIN" --profile out-probe --test-seconds 10 >> "$LOG" 2>&1 &
live_pid=$!
printf '%s\n' "$live_pid" > "$LIVE_PID_FILE"
wait "$live_pid"
test_result=$?
checkpoint live_binary_end
rm -f "$LIVE_PID_FILE"

# The binary restores ADB itself. The coordinator is still called for MTP so
# the final state matches the snapshot taken before the test.
restore_result=0
checkpoint normal_restore_begin
restore_previous_mode || restore_result=$?
checkpoint normal_restore_end
printf '%s H2 finished test_rc=%s restore_rc=%s previous_mode=%s\n' \
	"$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" "$test_result" "$restore_result" \
	"$previous_mode" >> "$LOG"

rm -f "$TOKEN" "$MAIN_PID_FILE"
if [ -r "$WATCHDOG_PID_FILE" ]; then
	watchdog_pid="$(cat "$WATCHDOG_PID_FILE" 2>/dev/null)"
	case "$watchdog_pid" in *[!0-9]*|'') ;; *) kill -TERM "$watchdog_pid" 2>/dev/null || true ;; esac
fi
rm -f "$WATCHDOG_PID_FILE"
checkpoint session_finished
exit $((test_result != 0 || restore_result != 0))
