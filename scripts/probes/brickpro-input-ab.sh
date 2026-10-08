#!/bin/sh
# Temporary, checksum-gated A/B runner for the Brick Pro input daemon.
# It runs the supplied 8 ms daemon from /tmp, invokes the read-only evdev
# probe, and restores the stock daemon on normal exit, signals, or parent loss.

set -u

STOCK_INPUTD=/usr/trimui/bin/trimui_inputd
FAST_INPUTD=/tmp/trimui_inputd-8ms
INPUT_PROBE=/tmp/brickpro-input-probe-aarch64
EXPECTED_STOCK=2746b37c463309f765ed63751187534ac14403a1693ccb87638bfd3462db14a5
EXPECTED_FAST=45e5a0c9f68e8c43c054c43729ba4335c5acaf7a05c770ef8fc2309fc9fd43a7
FAST_PID=
WATCHDOG_PID=
STOCK_WAS_RUNNING=0
RESTORED=0

file_sha256() {
	sha256sum "$1" 2>/dev/null | awk '{print $1}'
}

trimui_event_present() {
	for audit_name in /sys/class/input/event*/device/name; do
		[ -r "$audit_name" ] || continue
		[ "$(cat "$audit_name" 2>/dev/null)" = "TRIMUI Player1" ] && return 0
	done
	return 1
}

wait_trimui_event_gone() {
	for audit_wait in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		trimui_event_present || return 0
		usleep 100000
	done
	return 1
}

start_stock_verified() {
	for audit_attempt in 1 2; do
		start-stop-daemon -S -b -x "$STOCK_INPUTD" || true
		sleep 1
		audit_stock_pid="$(pidof trimui_inputd 2>/dev/null || true)"
		if [ -n "$audit_stock_pid" ] && trimui_event_present; then
			printf 'stock_restore_attempt=%s stock_pid=%s status=stable\n' \
				"$audit_attempt" "$audit_stock_pid"
			return 0
		fi
		for audit_pid in $audit_stock_pid; do
			kill -TERM "$audit_pid" 2>/dev/null || true
		done
		usleep 200000
		for audit_pid in $audit_stock_pid; do
			kill -KILL "$audit_pid" 2>/dev/null || true
		done
		wait_trimui_event_gone || true
		sleep 1
	done
	echo "stock daemon restoration failed" >&2
	return 1
}

restore_stock() {
	[ "$RESTORED" = 1 ] && return
	RESTORED=1
	if [ -n "$WATCHDOG_PID" ]; then
		kill -TERM "$WATCHDOG_PID" 2>/dev/null || true
		wait "$WATCHDOG_PID" 2>/dev/null || true
		WATCHDOG_PID=
	fi
	if [ -n "$FAST_PID" ] && kill -0 "$FAST_PID" 2>/dev/null; then
		kill -TERM "$FAST_PID" 2>/dev/null || true
		for audit_wait in 1 2 3 4 5 6 7 8 9 10; do
			kill -0 "$FAST_PID" 2>/dev/null || break
			usleep 100000
		done
		kill -0 "$FAST_PID" 2>/dev/null && kill -KILL "$FAST_PID" 2>/dev/null || true
	fi
	wait_trimui_event_gone || true
	if [ "$STOCK_WAS_RUNNING" = 1 ] && ! pidof trimui_inputd >/dev/null 2>&1; then
		start_stock_verified || true
	fi
	printf 'restore stock_running=%s\n' "$(pidof trimui_inputd 2>/dev/null || true)"
}

trap restore_stock EXIT HUP INT TERM

[ -x "$STOCK_INPUTD" ] || { echo "stock input daemon missing" >&2; exit 1; }
[ -x "$FAST_INPUTD" ] || { echo "8ms input daemon missing" >&2; exit 1; }
[ -x "$INPUT_PROBE" ] || { echo "input probe missing" >&2; exit 1; }

STOCK_SHA="$(file_sha256 "$STOCK_INPUTD")"
FAST_SHA="$(file_sha256 "$FAST_INPUTD")"
printf 'stock_sha256=%s fast_sha256=%s\n' "$STOCK_SHA" "$FAST_SHA"
[ "$STOCK_SHA" = "$EXPECTED_STOCK" ] || { echo "unexpected stock daemon; refusing A/B" >&2; exit 1; }
[ "$FAST_SHA" = "$EXPECTED_FAST" ] || { echo "unexpected 8ms daemon; refusing A/B" >&2; exit 1; }

STOCK_PIDS="$(pidof trimui_inputd 2>/dev/null || true)"
if [ -n "$STOCK_PIDS" ]; then
	STOCK_WAS_RUNNING=1
	for audit_pid in $STOCK_PIDS; do
		kill -TERM "$audit_pid" 2>/dev/null || true
	done
	for audit_wait in 1 2 3 4 5 6 7 8 9 10; do
		pidof trimui_inputd >/dev/null 2>&1 || break
		usleep 100000
	done
	if pidof trimui_inputd >/dev/null 2>&1; then
		echo "stock daemon did not stop; refusing A/B" >&2
		exit 1
	fi
	wait_trimui_event_gone || {
		echo "stock evdev node did not disappear; refusing A/B" >&2
		exit 1
	}
fi

"$FAST_INPUTD" >/tmp/nx-controller-8ms-inputd.log 2>&1 &
FAST_PID=$!
usleep 500000
kill -0 "$FAST_PID" 2>/dev/null && trimui_event_present || {
	echo "8ms daemon failed to create a stable evdev node" >&2
	exit 1
}

PARENT_PID=$$
(
	while kill -0 "$PARENT_PID" 2>/dev/null && kill -0 "$FAST_PID" 2>/dev/null; do
		sleep 1
	done
	if ! kill -0 "$PARENT_PID" 2>/dev/null; then
		kill -TERM "$FAST_PID" 2>/dev/null || true
		for audit_wait in 1 2 3 4 5 6 7 8 9 10; do
			kill -0 "$FAST_PID" 2>/dev/null || break
			usleep 100000
		done
		kill -0 "$FAST_PID" 2>/dev/null && kill -KILL "$FAST_PID" 2>/dev/null || true
		wait_trimui_event_gone || true
		if ! pidof trimui_inputd >/dev/null 2>&1; then
			start_stock_verified || true
		fi
	fi
) &
WATCHDOG_PID=$!

printf 'profile=experimental-8ms fast_pid=%s probe_seconds=20\n' "$FAST_PID"
sleep 5
"$INPUT_PROBE" --seconds 20
