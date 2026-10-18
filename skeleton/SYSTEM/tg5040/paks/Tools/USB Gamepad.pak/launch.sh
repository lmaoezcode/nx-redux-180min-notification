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

# --- USB mode safety begin ---------------------------------------------------
# Host-tested by workspace/all/usbgamepad/tests/test_launch_usb_restore.sh,
# which extracts exactly this block. Fail-closed (AGENT.md 11.1): nx_usb_mode.sh
# is never run when nx_usb_guard.elf is missing or cannot start (for example a
# Docker-built static binary that aborts with "FATAL: kernel too old"). The
# only fallbacks are: read `status`, log the mismatch, tell the user, and leave
# USB in the state it is in (ADB after usbgamepad.elf --restore).
USB_MODE_SH="$(command -v nx_usb_mode.sh 2>/dev/null)"
[ -n "$USB_MODE_SH" ] || USB_MODE_SH="${SYSTEM_PATH:-/mnt/SDCARD/.system}/bin/nx_usb_mode.sh"
# Same resolution order as nx_usb_mode.sh itself.
USB_GUARD="${NX_USB_GUARD:-${NX_USB_RUNTIME_DIR:-$(dirname "$USB_MODE_SH")}/nx_usb_guard.elf}"
USB_PRE_MODE=""
USB_GUARD_OK=0
USB_SESSION_STARTED=0
USB_FAIL_MARKER=/tmp/nx_usb_gamepad_restore_failed

usb_log() {
	printf 'usbgamepad: %s\n' "$*" >> "$LOG_FILE"
}

usb_mode_status() {
	"$USB_MODE_SH" status 2>/dev/null
}

# $1 on-screen text, $2 hint. Best effort, never blocks for more than 6 s.
usb_notice() {
	command -v show2.elf >/dev/null 2>&1 || return 0
	show2.elf \
		--mode=progress \
		--image="${SYSTEM_PATH:-/mnt/SDCARD/.system}/res/logo.png" \
		--bgcolor=0x000000 \
		--fontcolor=0xFF6666 \
		--text="$1" \
		--hint="$2" \
		--timeout=6 \
		--texty=78 \
		--progressy=95 >> "$LOG_FILE" 2>&1
}

# The guard is exec'd by every nx_usb_mode.sh action except `status`. --holders
# only scans /proc: it proves the binary starts without any USB/SD I/O.
usb_guard_usable() {
	if [ ! -x "$USB_GUARD" ]; then
		usb_log "USB guard missing or not executable: $USB_GUARD"
		return 1
	fi
	"$USB_GUARD" --holders /nx-usb-guard-preflight >/dev/null 2>> "$LOG_FILE"
	guard_rc=$?
	if [ "$guard_rc" -ne 0 ]; then
		usb_log "USB guard did not start rc=$guard_rc (134 = abort, e.g. FATAL: kernel too old from a Docker-built static binary; check readelf -n shows ABI <= 3.7.0): $USB_GUARD"
		return 1
	fi
	return 0
}

# Before ANY change: guard usable and USB mode known. Returns 1 to refuse; in
# that case nothing was touched, so MTP/ADB keeps running as before.
usb_preflight() {
	if ! usb_guard_usable; then
		usb_notice "USB helper cannot start" "USB left unchanged. Log: $LOG_FILE"
		return 1
	fi
	USB_GUARD_OK=1
	USB_PRE_MODE="$(usb_mode_status)"
	if [ "$USB_PRE_MODE" = link ]; then
		# A USB Cable netplay link left behind: its daemon outlives the wizard, and
		# a crash or power-quit skips the teardown. One guarded link-stop gives the
		# previous mode back; only then is the gadget free for the gamepad.
		usb_log "USB link marker found; running link-stop first"
		"$USB_MODE_SH" link-stop >> "$LOG_FILE" 2>&1 || usb_log "nx_usb_mode.sh link-stop failed"
		USB_PRE_MODE="$(usb_mode_status)"
	fi
	case "$USB_PRE_MODE" in
		mtp|adb) usb_log "USB mode before session: $USB_PRE_MODE" ;;
		*)
			usb_log "USB mode before session is '$USB_PRE_MODE'; refusing to start"
			usb_notice "USB is busy ($USB_PRE_MODE)" "Close USB Storage or Netplay first. Log: $LOG_FILE"
			return 1
			;;
	esac
	return 0
}

# The bottom port has one device controller, so MTP and the gamepad cannot own
# it simultaneously. Marks the session started so cleanup verifies the mode.
usb_enter_gamepad_mode() {
	USB_SESSION_STARTED=1
	"$USB_MODE_SH" stop >> "$LOG_FILE" 2>&1
	stop_rc=$?
	if [ "$stop_rc" -ne 0 ]; then
		usb_log "nx_usb_mode.sh stop failed rc=$stop_rc; not starting the gamepad"
		usb_notice "USB mode change failed" "Log: $LOG_FILE"
		return 1
	fi
	return 0
}

# Run once from cleanup(): bring USB back to the mode seen before the session.
# At most ONE guarded `restore` call, never retried, never run without a guard
# that passed usb_preflight(). Always verified with the guard-free `status`.
restore_usb_mode() {
	[ "$USB_SESSION_STARTED" = "1" ] || return 0
	usb_now="$(usb_mode_status)"
	if [ "$usb_now" = "$USB_PRE_MODE" ]; then
		usb_log "USB mode is $usb_now as before the session"
		rm -f "$USB_FAIL_MARKER"
		return 0
	fi
	restore_rc=""
	if [ "$USB_PRE_MODE" = "mtp" ] && [ "$USB_GUARD_OK" = "1" ]; then
		"$USB_MODE_SH" restore >> "$LOG_FILE" 2>&1
		restore_rc=$?
		usb_log "nx_usb_mode.sh restore rc=$restore_rc"
		usb_now="$(usb_mode_status)"
		if [ "$usb_now" = "$USB_PRE_MODE" ]; then
			usb_log "USB mode restored to $usb_now"
			rm -f "$USB_FAIL_MARKER"
			return 0
		fi
	fi
	usb_log "USB_MODE_NOT_RESTORED expected=${USB_PRE_MODE:-unknown} actual=${usb_now:-unknown} restore_rc=${restore_rc:-not-run}; leaving USB as is"
	printf 'expected=%s actual=%s restore_rc=%s\n' "${USB_PRE_MODE:-unknown}" "${usb_now:-unknown}" "${restore_rc:-not-run}" > "$USB_FAIL_MARKER" 2>/dev/null
	usb_notice "USB not restored (now: ${usb_now:-unknown})" "Reboot to get MTP back. Log: $LOG_FILE"
	return 1
}
# --- USB mode safety end -----------------------------------------------------

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
	restore_usb_mode || true
}

trap cleanup EXIT HUP INT TERM

mkdir -p "$(dirname "$LOG_FILE")"
mkdir -p "$(dirname "$LAYOUT_FILE")"

# Refuse before touching anything when the USB helper chain is not usable.
# Skipping cleanup here is intentional: nothing has been changed yet.
if ! usb_preflight; then
	trap - EXIT HUP INT TERM
	exit 1
fi

touch /tmp/stay_awake /tmp/hide_osdd

# Cleanup returns USB to the mode recorded by usb_preflight (MTP or ADB).
usb_enter_gamepad_mode || exit 1

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
