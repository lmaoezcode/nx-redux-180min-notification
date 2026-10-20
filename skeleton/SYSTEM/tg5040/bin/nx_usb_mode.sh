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
# USB Gamepad's own marker (usbgamepad.c ACTIVE_MARKER) and the USB-link marker
# written by link-start: `status` prints "link" while the latter exists.
GAMEPAD_MARKER=/tmp/nx_usb_gamepad_active
LINK_MARKER=/tmp/nx_usb_link_active
SCRIPT_DIR="${NX_USB_RUNTIME_DIR:-$(CDPATH= cd "$(dirname "$0")" 2>/dev/null && pwd)}"
UMTP_BIN="$SCRIPT_DIR/umtprd"
USBLINK_BIN="${NX_USBLINK_BIN:-$SCRIPT_DIR/usblink.elf}"
UMTP_CONFIG="$SCRIPT_DIR/../etc/umtprd.conf"
USB_GUARD="${NX_USB_GUARD:-$SCRIPT_DIR/nx_usb_guard.elf}"

# The functions live in nx_usb_lib.sh next to this script; the USB Storage wrapper
# points NX_USB_LIB at the copy it stages in RAM. Without them nothing is touched
# (checked before the guard pauses ADB).
NX_USB_LIB_FILE="${NX_USB_LIB:-$SCRIPT_DIR/nx_usb_lib.sh}"
[ -r "$NX_USB_LIB_FILE" ] || { echo "Missing USB library: $NX_USB_LIB_FILE" >&2; exit 1; }

# Native guard owns a separate mutex and arms a detached guardian before
# pausing the exact stock ADB instance. Never suspend ADB from bare shell:
# loss of that shell/session must not leave ADB stopped indefinitely.
if [ "$ACTION" != status ] && [ "${NX_USB_GUARDED:-0}" != 1 ]; then
	[ -x "$USB_GUARD" ] || { echo "Missing USB transaction guard: $USB_GUARD" >&2; exit 1; }
	export NX_USB_GUARDED=1
	exec "$USB_GUARD" /bin/sh "$0" "$@"
fi

. "$NX_USB_LIB_FILE"

lock || exit 1

case "$ACTION" in
	start|restore) refuse_if_link "$ACTION" && start_mtp ;;
	stop) refuse_if_link stop && stop_mtp ;;
	storage-start) refuse_if_link storage-start && storage_start ;;
	storage-detach) storage_detach ;;
	link-start) link_start ;;
	link-stop) link_stop ;;
	status)
		if link_active; then
			echo "link"
		else
			current_mode
		fi
		;;
	*)
		echo "usage: $0 {start|stop|restore|status|link-start|link-stop|storage-start /dev/mmcblk1p1|storage-detach}" >&2
		exit 2
		;;
esac
