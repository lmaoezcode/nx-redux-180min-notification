#!/bin/sh

# Keep the stock UI, NOT its launcher: that launcher repairs the filesystem
# before disconnecting the host. All control and logs below live in RAM.
STOCK_DIR=/usr/trimui/apps/usb_storage
STAGED_LAUNCHER=/tmp/nx-usb-storage-stage.sh
STAGED_HELPER=/tmp/nx-usb-storage-mode.sh
STAGED_DIAG=/tmp/nx-usb-diag.sh
STAGED_SERVICES=/tmp/nx-usb-services.sh
STAGED_GUARD=/tmp/nx-usb-guard.elf
STAGE_LOG=/tmp/nx-usb-storage.txt
METRICS_LOG=/tmp/nx-usb-metrics.txt
STORAGE_MARKER=/tmp/nx_usb_storage_active
SESSION_LOCK=/tmp/nx_usb_storage_session.lock
BLOCKED_MARKER=/tmp/nx_usb_storage_remount_blocked
ORIGINAL_HELPER="${SYSTEM_PATH:-/mnt/SDCARD/.system}/bin/nx_usb_mode.sh"
ORIGINAL_GUARD="${SYSTEM_PATH:-/mnt/SDCARD/.system}/bin/nx_usb_guard.elf"

if [ "${NX_USB_STORAGE_STAGED:-0}" != 1 ]; then
	[ -x "$STOCK_DIR/usb_storage" ] && [ -x "$ORIGINAL_HELPER" ] && [ -x "$ORIGINAL_GUARD" ] || exit 1
	mkdir "$SESSION_LOCK" 2>/dev/null || { echo "USB Storage already running"; exit 1; }
	if ! cp -f "$0" "$STAGED_LAUNCHER" || ! cp -f "$ORIGINAL_HELPER" "$STAGED_HELPER" ||
		! cp -f "${SYSTEM_PATH:-/mnt/SDCARD/.system}/bin/nx_usb_services.sh" "$STAGED_SERVICES" ||
		! cp -f "$ORIGINAL_GUARD" "$STAGED_GUARD"; then
		rmdir "$SESSION_LOCK" 2>/dev/null
		exit 1
	fi
	if [ -r "${SYSTEM_PATH:-/mnt/SDCARD/.system}/bin/nx_usb_diag.sh" ]; then
		cp -f "${SYSTEM_PATH:-/mnt/SDCARD/.system}/bin/nx_usb_diag.sh" "$STAGED_DIAG"
		/bin/sh "$STAGED_DIAG" >/dev/null 2>&1 &
	fi
	sync
	cd /tmp || exit 1
	export NX_USB_STORAGE_STAGED=1
	export NX_USB_GUARD="$STAGED_GUARD"
	exec /bin/sh "$STAGED_LAUNCHER" "$@"
fi

exec >"$STAGE_LOG" 2>&1
echo "NX USB Storage safe-v4: RAM OSD pause + guarded ADB transaction; no automatic repair"
SD_ROOT="${SDCARD_PATH:-/mnt/SDCARD}"
SD_DEVICE="$(awk -v root="$SD_ROOT" '$2 == root { print $1; exit }' /proc/mounts)"
SD_TYPE="$(awk -v root="$SD_ROOT" '$2 == root { print $3; exit }' /proc/mounts)"
SD_OPTIONS="$(awk -v root="$SD_ROOT" '$2 == root { print $4; exit }' /proc/mounts)"
USB_MODE_BEFORE=adb
UI_PID=
DETACHED=0
SERVICES_PREPARED=0
USB_CHANGED=0
STEP_PID=

usb_mode() {
	/bin/sh "$STAGED_HELPER" "$@"
}

sd_mounted() {
	awk -v root="$SD_ROOT" '$2 == root { found = 1 } END { exit !found }' /proc/mounts
}

save_logs() {
	sd_mounted || return 1
	LOG_DIR="$SD_ROOT/.userdata/${PLATFORM:-tg5040}/logs"
	mkdir -p "$LOG_DIR" || return 1
	cp -f "$STAGE_LOG" "$LOG_DIR/usb-storage.txt" || true
	[ ! -f "$METRICS_LOG" ] || cp -f "$METRICS_LOG" "$LOG_DIR/usb-thermal.txt"
	[ ! -f /tmp/nx-mtp.log ] || cp -f /tmp/nx-mtp.log "$LOG_DIR/usb-mode.txt"
	[ ! -f /tmp/nx-usb-restored-services.txt ] || cp -f /tmp/nx-usb-restored-services.txt "$LOG_DIR/usb-services.txt"
	[ ! -f /tmp/nx-usb-guard.txt ] || cp -f /tmp/nx-usb-guard.txt "$LOG_DIR/usb-guard.txt"
	sync
}

log_card_holders() {
	echo "SD holder snapshot (diagnosis only; no process is killed):"
	# Native readlink/maps inspection avoids hundreds of shell subprocesses.
	"$STAGED_GUARD" --holders "$SD_ROOT" || true
	cat /proc/mounts
	cat /proc/swaps
}

cleanup() {
	trap - EXIT HUP INT TERM QUIT
	cleanup_result=0
	[ -z "$STEP_PID" ] || { kill -TERM "$STEP_PID" 2>/dev/null; wait "$STEP_PID" 2>/dev/null; STEP_PID=; }
	[ -z "$UI_PID" ] || { kill "$UI_PID" 2>/dev/null; wait "$UI_PID" 2>/dev/null; }
	cd /tmp || return 1
	# No filesystem mount or log-copy is allowed after a failed USB detach.
	cleanup_attempt=0
	while [ "$cleanup_attempt" -lt 3 ]; do
		if usb_mode storage-detach; then DETACHED=1; break; fi
		cleanup_attempt=$((cleanup_attempt + 1))
		sleep 1
	done
	if [ "$DETACHED" != 1 ]; then
		echo "CRITICAL: USB still owns the card; remount and MTP restore blocked"
		touch "$BLOCKED_MARKER"
		return 1
	fi
	sync
	if ! sd_mounted; then
		if [ -z "$SD_DEVICE" ] || [ -z "$SD_TYPE" ] ||
			! mount -t "$SD_TYPE" -o "$SD_OPTIONS" "$SD_DEVICE" "$SD_ROOT"; then
			echo "CRITICAL: card remount failed; leaving logs in RAM"
			touch "$BLOCKED_MARKER"
			return 1
		fi
	fi
	rm -f "$STORAGE_MARKER" "$BLOCKED_MARKER"
	if [ "$SERVICES_PREPARED" = 1 ] && ! /bin/sh "$STAGED_SERVICES" restore; then
		echo "Service restore failed; recovery state kept in RAM"
		cleanup_result=1
	fi
	# Let the responder's old endpoints settle. Retry a failed restore, never
	# mask it with a success message or restore before the card is mounted.
	cleanup_mode_action=stop
	[ "$USB_MODE_BEFORE" != mtp ] || cleanup_mode_action=restore
	cleanup_mode_attempt=0
	while [ "$USB_CHANGED" = 1 ] && ! /bin/sh "$ORIGINAL_HELPER" "$cleanup_mode_action"; do
		cleanup_mode_attempt=$((cleanup_mode_attempt + 1))
		[ "$cleanup_mode_attempt" -lt 3 ] || { echo "USB mode restore failed; see usb-mode.txt"; cleanup_result=1; break; }
		sleep 1
	done
	echo "USB Storage finished: USB detached before SD remount; original mode=$USB_MODE_BEFORE"
	save_logs || true
	rm -f /tmp/stay_awake "$STAGED_LAUNCHER" "$STAGED_HELPER" "$STAGED_SERVICES" "$STAGED_GUARD"
	rmdir "$SESSION_LOCK" 2>/dev/null || true
	return "$cleanup_result"
}
on_exit() {
	exit_status=$?
	cleanup || exit_status=1
	exit "$exit_status"
}
trap on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 131' QUIT

refuse_export() {
	echo "USB Storage blocked: $*"
	# Only the rootfs UI was started, never a launcher with fsck/mount actions.
	[ -z "$UI_PID" ] || { kill -TERM "$UI_PID" 2>/dev/null; wait "$UI_PID" 2>/dev/null; UI_PID=; }
	log_card_holders
	if sd_mounted && command -v show2.elf >/dev/null 2>&1; then
		refusal_text="USB setup failed. See USB log."
		case "$*" in
			*"card is still in use"*) refusal_text="Card is busy. Use MTP or a card reader." ;;
			*"MTP did not stop"*) refusal_text="MTP did not stop. USB Storage cancelled." ;;
			*"NX service"*) refusal_text="Background service is busy. USB Storage cancelled." ;;
			*"USB link is active"*) refusal_text="USB link is active. Leave Netplay first." ;;
		esac
		show2.elf --mode=progress --image=/dev/null --text="$refusal_text" --timeout=5
	fi
	exit 1
}

# Diagnostics only (no behaviour change): timestamps and the child's kernel state
# letter (R/S running or sleeping, Z = finished but not yet reaped) so the log
# shows whether a step is really still running when this wrapper waits on it.
nx_trace() {
	echo "[t=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)] $*"
}
nx_pid_state() {
	awk '{ print $3 }' "/proc/$1/stat" 2>/dev/null || echo "?"
}

run_step() {
	nx_trace "step start: $*"
	"$@" &
	STEP_PID=$!
	step_polls=0
	while kill -0 "$STEP_PID" 2>/dev/null; do
		if ! kill -0 "$UI_PID" 2>/dev/null; then
			nx_trace "UI gone while step pid=$STEP_PID state=$(nx_pid_state "$STEP_PID") was still waited on"
			echo "B/UI exit requested during preparation; cancelling before further USB work"
			kill -TERM "$STEP_PID" 2>/dev/null || true
			wait "$STEP_PID" 2>/dev/null
			STEP_PID=
			return 125
		fi
		step_polls=$((step_polls + 1))
		# About every 2 s at 0.1 s polls (every 20 s if only sleep 1 exists).
		[ $((step_polls % 20)) -ne 0 ] || nx_trace "step pid=$STEP_PID still polled after $step_polls polls, state=$(nx_pid_state "$STEP_PID")"
		usleep 100000 2>/dev/null || sleep 1
	done
	wait "$STEP_PID"
	step_result=$?
	nx_trace "step finished rc=$step_result after $step_polls polls"
	STEP_PID=
	# A very fast step and B may complete together. Do not advance afterwards.
	[ -n "$UI_PID" ] && kill -0 "$UI_PID" 2>/dev/null || return 125
	return "$step_result"
}

prepare_or_cancel() {
	run_step "$@"
	prepare_result=$?
	[ "$prepare_result" != 125 ] || exit 0
	return "$prepare_result"
}

case "$SD_DEVICE" in
	/dev/mmcblk1p[0-9]*) ;;
	*) refuse_export "mounted SD partition cannot be identified" ;;
esac
USB_MODE_BEFORE="$(usb_mode status)" || refuse_export "USB mode status unavailable"
# A USB Cable netplay link owns the gadget (nx_usb_mode.sh link-start); exporting
# the card while its function is linked is refused, and nothing has been changed yet.
[ "$USB_MODE_BEFORE" != link ] || refuse_export "USB link is active"
[ "$USB_MODE_BEFORE" = mtp ] || USB_MODE_BEFORE=adb
touch "$STORAGE_MARKER" /tmp/stay_awake
# Show the rootfs-only USB UI immediately: B works during preparation too.
(cd "$STOCK_DIR" && LD_LIBRARY_PATH="/usr/trimui/lib:/lib:/usr/lib:$STOCK_DIR" HOME=/tmp exec "$STOCK_DIR/usb_storage") &
UI_PID=$!
SERVICES_PREPARED=1
prepare_or_cancel /bin/sh "$STAGED_SERVICES" prepare || refuse_export "a known NX service did not stop cleanly"
# Do not tear down working MTP if service preparation is refused/cancelled.
USB_CHANGED=1
prepare_or_cancel /bin/sh "$STAGED_HELPER" stop || refuse_export "MTP did not stop cleanly"
sync
cd /tmp || exit 1
umount "$SD_ROOT" || refuse_export "card is still in use; block device NOT exported"

# Change only the bottom controller's role, not the stock script's top GPIO.
# storage-start checks all whole-card mount aliases and swap before exporting.
prepare_or_cancel /bin/sh "$STAGED_HELPER" storage-start "$SD_DEVICE" || refuse_export "UMS configuration or card ownership check failed"
echo "SD unmounted, backing device verified; stock UI ready"
wait "$UI_PID"
RESULT=$?
UI_PID=
exit "$RESULT"
