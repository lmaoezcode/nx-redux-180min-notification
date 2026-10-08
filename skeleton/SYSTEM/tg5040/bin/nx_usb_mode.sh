#!/bin/sh

# NX Redux USB data-mode coordinator for tg5040. The stock mtp.gs0 function
# advertises a vendor-specific interface which Windows cannot match to its
# inbox MTP driver. uMTP-Responder supplies a standards-compliant 06/01/01
# FunctionFS interface while the stock ADB function remains available.

ACTION="${1:-restore}"
STORAGE_DEVICE="${2:-}"
LOCK_DIR=/tmp/nx_usb_mode.lock
LOG=/tmp/nx-mtp.log
FFS_DIR=/dev/ffs-umtp
GADGET=/sys/kernel/config/usb_gadget/g1
UDC_CLASS=/sys/class/udc
USB_ROLE_ROOT=/sys/devices/platform/soc/usbc0
USB_ROLE_DEVICE="$USB_ROLE_ROOT/usb_device"
USB_ROLE_STATE="$USB_ROLE_ROOT/otg_role"
STORAGE_MARKER=/tmp/nx_usb_storage_active
SCRIPT_DIR="${NX_USB_RUNTIME_DIR:-$(CDPATH= cd "$(dirname "$0")" 2>/dev/null && pwd)}"
UMTP_BIN="$SCRIPT_DIR/umtprd"
UMTP_CONFIG="$SCRIPT_DIR/../etc/umtprd.conf"
USB_GUARD="${NX_USB_GUARD:-$SCRIPT_DIR/nx_usb_guard.elf}"

# Native guard owns a separate mutex and arms a detached guardian before
# pausing the exact stock ADB instance. Never suspend ADB from bare shell:
# loss of that shell/session must not leave ADB stopped indefinitely.
if [ "$ACTION" != status ] && [ "${NX_USB_GUARDED:-0}" != 1 ]; then
	[ -x "$USB_GUARD" ] || { echo "Missing USB transaction guard: $USB_GUARD" >&2; exit 1; }
	export NX_USB_GUARDED=1
	exec "$USB_GUARD" /bin/sh "$0" "$@"
fi

log() {
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" "$*" >> "$LOG"
}

checkpoint() {
	[ -n "${NX_USB_CHECKPOINT_LOG:-}" ] || return 0
	printf '%s coordinator pid=%s action=%s stage=%s udc=%s\n' \
		"$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" "$$" "$ACTION" "$1" \
		"$(cat "$GADGET/UDC" 2>/dev/null)" >> "$NX_USB_CHECKPOINT_LOG"
	sync
}

mounted_at() {
	awk -v root="$1" '$2 == root { found = 1 } END { exit !found }' /proc/mounts
}

gadget_bound() {
	bound_udc="$(cat "$GADGET/UDC" 2>/dev/null)" || return 1
	[ -n "$bound_udc" ] && [ "$bound_udc" != none ]
}

pause_short() {
	usleep 100000 2>/dev/null || sleep 1
}

udc_present() {
	for usb_udc in "$UDC_CLASS"/*; do
		[ -e "$usb_udc" ] && return 0
	done
	return 1
}

usb_role_is() {
	[ -r "$USB_ROLE_STATE" ] && [ "$(cat "$USB_ROLE_STATE" 2>/dev/null)" = "$1" ]
}

attach_usb_phy() {
	checkpoint phy_device_begin
	# The Brick Pro port is OTG with VBUS/ID detection.  If the cable already
	# supplies VBUS, reading usb_device again needlessly unregisters/re-registers
	# the UDC and can strand ADB.  Only use the vendor action node as recovery
	# when the port is not already in device role.
	if usb_role_is usb_device && udc_present; then
		checkpoint phy_device_already_ready
		return 0
	fi
	[ -r "$USB_ROLE_DEVICE" ] || { log "USB device-role trigger missing"; return 1; }
	cat "$USB_ROLE_DEVICE" >/dev/null 2>> "$LOG" || {
		log "could not switch USB PHY to device role"
		return 1
	}
	phy_wait=0
	while [ "$phy_wait" -lt 50 ]; do
		if usb_role_is usb_device && udc_present; then
			checkpoint phy_device_end
			return 0
		fi
		phy_wait=$((phy_wait + 1))
		pause_short
	done
	log "USB role/controller did not become ready after device-role request"
	checkpoint phy_device_failed
	return 1
}

set_adb_mode() {
	checkpoint adb_mode_begin
	attach_usb_phy || return 1
	/bin/setusbconfig adb >> "$LOG" 2>&1 || return 1
	checkpoint adb_mode_end
}

storage_linked() {
	for storage_link in "$GADGET"/configs/*/*; do
		[ -L "$storage_link" ] || continue
		case "$(readlink "$storage_link")" in
			*/mass_storage.*) return 0 ;;
		esac
	done
	return 1
}

storage_open() {
	storage_linked && return 0
	for storage_file in "$GADGET"/functions/mass_storage.*/lun.*/file; do
		[ -e "$storage_file" ] || continue
		storage_value="$(cat "$storage_file" 2>/dev/null)" || return 0
		[ -n "$storage_value" ] && return 0
	done
	return 1
}

detach_gadget() {
	[ -d "$GADGET" ] || return 0
	checkpoint gadget_detach_begin
	# A verified ConfigFS unbind is a prerequisite, not an exclusive lock:
	# stock adbd writes UDC again when it reopens FunctionFS endpoints.
	# Do not use the vendor usb_null role action as a transaction lock; its
	# earlier live probe lost USB. Callers still need ownership/restore logic.
	printf '\n' > "$GADGET/UDC" 2>> "$LOG" ||
		log "UDC unbind write reported an error; verifying actual state"
	detach_wait=0
	while [ "$detach_wait" -lt 20 ]; do
		udc_value="$(cat "$GADGET/UDC" 2>/dev/null)"
		udc_read_status=$?
		if [ "$udc_read_status" -eq 0 ] && { [ -z "$udc_value" ] || [ "$udc_value" = none ]; }; then
			checkpoint gadget_detach_end
			return 0
		fi
		detach_wait=$((detach_wait + 1))
		# Stock adbd may rebind the UDC right after our unbind. Repeat the same
		# unbind a few times inside the unchanged ~2 s window instead of only
		# polling; a persistent rebind still fails below, so callers stay blocked.
		if [ "$((detach_wait % 5))" -eq 0 ] && [ "$udc_read_status" -eq 0 ]; then
			log "UDC rebound after unbind (retry $((detach_wait / 5))); repeating unbind"
			printf '\n' > "$GADGET/UDC" 2>> "$LOG" || true
		fi
		pause_short
	done
	if [ "$udc_read_status" -eq 0 ]; then
		log "UDC remained bound after disconnect request: $udc_value"
	else
		log "could not read UDC state after disconnect request"
	fi
	checkpoint gadget_detach_failed
	return 1
}

storage_detach() {
	# Unbind first, unlink the function, then close every backing LUN. Never
	# allow a filesystem check/mount until all three operations are verified.
	[ -d "$GADGET" ] || return 0
	# A rejected entry never exported a LUN. An unrelated bound ADB/MTP
	# interface must not turn that harmless refusal into a frozen remount guard.
	if ! storage_open; then
		log "No USB storage links/backing LUNs to detach"
		return 0
	fi
	detach_gadget || { log "cannot unbind USB storage gadget"; return 1; }
	for storage_link in "$GADGET"/configs/*/*; do
		[ -L "$storage_link" ] || continue
		case "$(readlink "$storage_link")" in
			*/mass_storage.*) rm -f "$storage_link" || return 1 ;;
		esac
	done
	for storage_file in "$GADGET"/functions/mass_storage.*/lun.*/file; do
		[ -e "$storage_file" ] || continue
		printf '\n' > "$storage_file" 2>> "$LOG" || {
			log "cannot close storage backing file: $storage_file"
			return 1
		}
	done
	storage_open && { log "storage still linked or backing LUN still open"; return 1; }
	log "USB storage detached; backing LUNs closed"
	return 0
}

card_in_use() {
	# The stock gadget exports the WHOLE mmc device, not just partition 1.
	# Resolve aliases and check all of that card's partitions and swap too.
	while read -r card_source card_rest; do
		card_resolved="$(readlink -f "$card_source" 2>/dev/null)"
		case "$card_resolved" in
			"$STORAGE_DEVICE"|"$STORAGE_DEVICE"p[0-9]*) return 0 ;;
		esac
	done < /proc/mounts
	while read -r card_source card_rest; do
		card_resolved="$(readlink -f "$card_source" 2>/dev/null)"
		case "$card_resolved" in
			"$STORAGE_DEVICE"|"$STORAGE_DEVICE"p[0-9]*) return 0 ;;
			/mnt/SDCARD/*) return 0 ;;
		esac
	done < /proc/swaps
	return 1
}

storage_start() {
	[ -r /proc/mounts ] && [ -r /proc/swaps ] || {
		log "cannot inspect mounts/swap; refusing UMS"
		return 1
	}
	case "$STORAGE_DEVICE" in
		/dev/mmcblk[0-9]*)
			STORAGE_DEVICE="$(readlink -f "$STORAGE_DEVICE")"
			STORAGE_DEVICE="${STORAGE_DEVICE%p*}"
			;;
		*) log "invalid storage device"; return 1 ;;
	esac
	# setusbconfig on tg5040 hardcodes /dev/mmcblk1; never export another disk.
	[ "$STORAGE_DEVICE" = /dev/mmcblk1 ] && [ -b "$STORAGE_DEVICE" ] || {
		log "stock storage backend does not match the selected card"
		return 1
	}
	card_in_use && { log "card is mounted or in use as swap; refusing UMS"; return 1; }
	if pidof umtprd MtpDaemon >/dev/null 2>&1 || mounted_at "$FFS_DIR"; then
		log "MTP is still running; refusing UMS"
		return 1
	fi
	storage_detach || return 1
	attach_usb_phy || return 1
	# Do not trust the stock script's exit status alone. Verify the actual
	# function, backing block device, ADB interface and UDC after it runs.
	/bin/setusbconfig mass_storage,adb >> "$LOG" 2>&1 || log "stock setter reported an error; checking state"
	storage_backing="$(cat "$GADGET/functions/mass_storage.usb0/lun.0/file" 2>/dev/null)"
	if storage_linked && [ "$(readlink -f "$storage_backing")" = "$STORAGE_DEVICE" ] &&
		[ -L "$GADGET/configs/c.1/ffs.adb" ] && gadget_bound &&
		! card_in_use; then
		log "USB storage active: $STORAGE_DEVICE (ADB retained)"
		return 0
	fi
	log "USB storage configuration verification failed"
	storage_detach || true
	return 1
}

lock() {
	tries=0
	while ! mkdir "$LOCK_DIR" 2>/dev/null; do
		tries=$((tries + 1))
		[ "$tries" -ge 50 ] && return 1
		usleep 100000 2>/dev/null || sleep 1
	done
	printf '%s\n' "$$" > "$LOCK_DIR/pid" || return 1
	trap 'rm -f "$LOCK_DIR/pid"; rmdir "$LOCK_DIR" 2>/dev/null' EXIT
	trap 'exit 129' HUP
	trap 'exit 130' INT
	trap 'exit 143' TERM
}

wait_for_adb_gadget() {
	i=0
	while [ ! -d "$GADGET/configs/c.1" ] && [ "$i" -lt 50 ]; do
		i=$((i + 1))
		usleep 100000 2>/dev/null || sleep 1
	done
	[ -d "$GADGET/configs/c.1" ]
}

remove_umtp_function() {
	checkpoint mtp_remove_begin
	# uMTP-Responder deliberately loops on FunctionFS disconnect.  Stop it and
	# wait for every endpoint fd to close before unbinding ConfigFS; otherwise
	# it can make ffs.mtp ready again during the unbind and reconnect the old
	# composite gadget before its links can be changed.
	killall umtprd 2>/dev/null || true
	killall MtpDaemon 2>/dev/null || true
	i=0
	while pidof umtprd MtpDaemon >/dev/null 2>&1 && [ "$i" -lt 20 ]; do
		i=$((i + 1))
		pause_short
	done
	if pidof umtprd MtpDaemon >/dev/null 2>&1; then
		log "MTP process did not stop"
		return 1
	fi
	checkpoint mtp_remove_process_stopped

	if [ -d "$GADGET" ]; then
		detach_gadget || { log "could not unbind MTP gadget"; return 1; }
		rm -f "$GADGET/configs/c.1/ffs.mtp" "$GADGET/configs/c.1/mtp.gs0" || return 1
	fi
	if mounted_at "$FFS_DIR"; then
		umount "$FFS_DIR" >> "$LOG" 2>&1 || return 1
	fi
	mounted_at "$FFS_DIR" && { log "MTP FunctionFS is still mounted"; return 1; }
	rmdir "$GADGET/functions/ffs.mtp" 2>/dev/null || true
	rmdir "$FFS_DIR" 2>/dev/null || true
	checkpoint mtp_remove_end
	return 0
}

stop_mtp() {
	checkpoint stop_begin
	storage_linked && { log "UMS owns USB; stop refused until storage-detach"; return 1; }
	remove_umtp_function || return 1
	set_adb_mode || return 1
	checkpoint stop_adb_ready
	[ -L "$GADGET/configs/c.1/ffs.adb" ] && gadget_bound || {
		log "ADB restoration did not complete"
		return 1
	}
	log "MTP stopped; ADB restored"
	checkpoint stop_end
}

start_mtp() {
	checkpoint mtp_start_begin
	if [ -e "$STORAGE_MARKER" ] || storage_open; then
		log "USB storage session/backing LUN is active; MTP restore refused"
		return 1
	fi
	mounted_at /mnt/SDCARD || { log "SD card is not mounted; MTP restore refused"; return 1; }
	[ -x /bin/setusbconfig ] || { log "missing /bin/setusbconfig"; return 1; }
	[ -x "$UMTP_BIN" ] || { log "missing $UMTP_BIN"; return 1; }
	[ -r "$UMTP_CONFIG" ] || { log "missing $UMTP_CONFIG"; return 1; }

	if pidof umtprd >/dev/null 2>&1 && mounted_at "$FFS_DIR" &&
		[ -L "$GADGET/configs/c.1/ffs.mtp" ] && gadget_bound; then
		log "MTP already active; USB enumeration unchanged"
		return 0
	fi
	remove_umtp_function || return 1
	set_adb_mode || {
		log "setusbconfig adb failed"
		return 1
	}
	wait_for_adb_gadget || { log "ADB gadget was not created"; return 1; }
	checkpoint mtp_start_adb_ready

	udc="$(cat "$GADGET/UDC" 2>/dev/null)"
	if [ -z "$udc" ] || [ "$udc" = none ]; then
		udc="$(ls /sys/class/udc 2>/dev/null | head -n 1)"
	fi
	[ -n "$udc" ] || { log "USB device controller not found"; return 1; }

	detach_gadget || { log "could not unbind before preparing MTP"; return 1; }
	checkpoint mtp_start_detached
	mkdir -p "$GADGET/functions/ffs.mtp" "$FFS_DIR"
	mount -t functionfs mtp "$FFS_DIR" >> "$LOG" 2>&1 || {
		log "could not mount FunctionFS MTP endpoint"
		set_adb_mode || true
		return 1
	}
	ln -s "$GADGET/functions/ffs.mtp" "$GADGET/configs/c.1/ffs.mtp" 2>/dev/null || {
		log "could not link FunctionFS MTP function"
		remove_umtp_function
		set_adb_mode || true
		return 1
	}

	"$UMTP_BIN" -conf "$UMTP_CONFIG" >> "$LOG" 2>&1 &
	umtp_pid=$!
	i=0
	while [ ! -e "$FFS_DIR/ep1" ] && [ "$i" -lt 50 ]; do
		i=$((i + 1))
		usleep 100000 2>/dev/null || sleep 1
	done
	if [ ! -e "$FFS_DIR/ep1" ] || ! kill -0 "$umtp_pid" 2>/dev/null; then
		log "uMTP-Responder did not create its endpoints"
		remove_umtp_function
		set_adb_mode || true
		return 1
	fi
	checkpoint mtp_start_ffs_ready

	# Re-enumerate only after both ADB and MTP functions are ready. The stock
	# ADB watcher may win the rebind race; it then binds the already-complete
	# composite config, which is equivalent.
	attach_usb_phy || {
		log "could not restore USB device role for ADB + MTP"
		remove_umtp_function
		set_adb_mode || true
		return 1
	}
	checkpoint mtp_start_phy_ready
	if ! gadget_bound; then
		printf '%s\n' "$udc" > "$GADGET/UDC" 2>> "$LOG" || {
			log "could not bind ADB + MTP gadget"
			remove_umtp_function
			set_adb_mode || true
			return 1
		}
	fi
	gadget_bound || { log "ADB + MTP controller binding did not complete"; return 1; }

	log "MTP active: /mnt/SDCARD exposed by uMTP-Responder (pid $umtp_pid)"
	checkpoint mtp_start_end
	return 0
}

lock || exit 1

case "$ACTION" in
	start|restore) start_mtp ;;
	stop) stop_mtp ;;
	storage-start) storage_start ;;
	storage-detach) storage_detach ;;
	status)
		if storage_open || [ -e "$STORAGE_MARKER" ]; then
			echo "storage"
		elif pidof umtprd >/dev/null 2>&1 && mounted_at "$FFS_DIR" &&
			[ -L "$GADGET/configs/c.1/ffs.mtp" ] && gadget_bound; then
			echo "mtp"
		else
			echo "adb"
		fi
		;;
	*)
		echo "usage: $0 {start|stop|restore|status|storage-start /dev/mmcblk1p1|storage-detach}" >&2
		exit 2
		;;
esac
