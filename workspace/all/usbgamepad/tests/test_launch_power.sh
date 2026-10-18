#!/bin/sh
# Host test for the "USB power mode" block of USB Gamepad.pak/launch.sh: the
# `pc`/`phone` switch file, the power/port sampler and its log handling. It
# extracts the block (and the safety block it logs through) from launch.sh and
# runs it against a fake sysfs tree. No device, USB or PMIC access.
#
# Usage: sh tests/test_launch_power.sh   (from workspace/all/usbgamepad)

HERE="$(CDPATH= cd "$(dirname "$0")" && pwd)"
REPO="$(CDPATH= cd "$HERE/../../../.." && pwd)"
LAUNCH="$REPO/skeleton/SYSTEM/tg5040/paks/Tools/USB Gamepad.pak/launch.sh"

T="$(mktemp -d "${TMPDIR:-/tmp}/nxgp-power.XXXXXX")" || exit 1
trap 'rm -rf "$T"' EXIT
FAILED=0
N=0

{
	sed -n '/^# --- USB mode safety begin/,/^# --- USB mode safety end/p' "$LAUNCH"
	sed -n '/^# --- USB power mode begin/,/^# --- USB power mode end/p' "$LAUNCH"
} > "$T/block.sh"
grep -q 'usb_power_read_mode' "$T/block.sh" || { echo "FAIL: power block not found in launch.sh"; exit 1; }

# The extracted block runs in a subshell per check with these variables.
drive() { # snippet
	LOG_DIR="$T/logs" LOG_FILE="$T/logs/usb-gamepad.txt" USERDATA_PATH="$T/userdata" \
	NXGP_SYSFS_ROOT="$T/fs" sh -c "
		mkdir -p '$T/logs' '$T/userdata'
		. '$T/block.sh'
		POWER_SAMPLES='$T/samples.txt'; POWER_LOG='$T/logs/usb-gamepad-power.txt'
		usleep() { sleep 1; }
		$1
	"
}

check() { # description, command...
	desc="$1"; shift
	N=$((N + 1))
	if "$@"; then return 0; fi
	echo "FAIL: $desc"
	FAILED=1
}
has() { grep -q -- "$2" "$1"; }
hasnt() { ! grep -q -- "$2" "$1"; }

fresh() {
	rm -rf "$T/userdata" "$T/logs" "$T/fs" "$T/samples.txt" "$T/out"
	mkdir -p "$T/userdata" "$T/logs" "$T/fs/sys/class/power_supply/axp2202-usb" \
		"$T/fs/sys/class/power_supply/axp2202-battery" "$T/fs/sys/class/udc/5100000.udc-controller" \
		"$T/fs/sys/class/axp"
	echo 1 > "$T/fs/sys/class/power_supply/axp2202-usb/online"
	echo Charging > "$T/fs/sys/class/power_supply/axp2202-battery/status"
	echo 60 > "$T/fs/sys/class/power_supply/axp2202-battery/capacity"
	echo configured > "$T/fs/sys/class/udc/5100000.udc-controller/state"
	echo SENTINEL > "$T/fs/sys/class/axp/axp_reg"
}

mode_of() { # file content as printed (use printf), expected mode, expected args
	fresh
	[ "$1" = "-" ] || printf "$1" > "$T/userdata/usb-gamepad-power"
	drive 'usb_power_read_mode; echo "$POWER_MODE|$POWER_ARGS"' > "$T/out" 2>/dev/null
	[ "$(cat "$T/out")" = "$2|$3" ]
}

# --- the switch file ---------------------------------------------------------
check "absent file = pc, no arguments"    mode_of '-' pc ''
check "phone"                             mode_of 'phone\n' phone '--power phone'
check "phone without newline"             mode_of 'phone' phone '--power phone'
check "mixed case with spaces and CRLF"   mode_of '  PhOnE \r\n' phone '--power phone'
check "pc"                                mode_of 'pc\n' pc ''
check "empty file = pc"                   mode_of '' pc ''
check "garbage = pc"                      mode_of 'android\n' pc ''
check "first line only"                   mode_of 'pc\nphone\n' pc ''
fresh; printf 'android\n' > "$T/userdata/usb-gamepad-power"
drive 'usb_power_read_mode' >/dev/null 2>&1
check "garbage is logged"                 has "$T/logs/usb-gamepad.txt" "ignoring unknown value 'android'"
fresh
drive 'usb_power_read_mode' >/dev/null 2>&1
check "absent file logs nothing"          test ! -e "$T/logs/usb-gamepad.txt"

# --- one sample --------------------------------------------------------------
fresh
drive 'POWER_MODE=pc; usb_power_sample_once 1' > "$T/out" 2>/dev/null
check "pc sample has VBUS"                has "$T/out" 'usb_online=1'
check "pc sample has battery"             has "$T/out" 'batt=Charging cap=60'
check "pc sample has UDC state"           has "$T/out" 'udc=configured'
check "pc sample reads the four registers" sh -c "grep -q ' 0x19=' '$T/out' && grep -q ' 0x17=' '$T/out' && grep -q ' 0xe3=' '$T/out' && grep -q ' 0xe7=' '$T/out'"
check "pc sample selected registers last 0xe7" test "$(cat "$T/fs/sys/class/axp/axp_reg")" = 0xe7

fresh
drive 'POWER_MODE=phone; usb_power_sample_once 0' > "$T/out" 2>/dev/null
check "phone sample has the sysfs fields" has "$T/out" 'usb_online=1 batt=Charging'
check "phone sample has no register"      hasnt "$T/out" ' 0x19='
check "phone sample leaves the PMIC node alone" test "$(cat "$T/fs/sys/class/axp/axp_reg")" = SENTINEL

fresh; rm -rf "$T/fs"
drive 'usb_power_sample_once 1' > "$T/out" 2>/dev/null
check "no sysfs at all still gives a line" has "$T/out" 'usb_online= batt= cap= udc='
check "no node, no registers"             hasnt "$T/out" ' 0x19='

fresh; rm -f "$T/fs/sys/class/axp/axp_reg"; mkdir -p "$T/fs/sys/class/axp/axp2202"; echo SENTINEL2 > "$T/fs/sys/class/axp/axp2202/axp_reg"
drive 'usb_power_sample_once 1' > "$T/out" 2>/dev/null
check "falls back to the axp2202 node"    test "$(cat "$T/fs/sys/class/axp/axp2202/axp_reg")" = 0xe7

# --- sampler and its log ------------------------------------------------------
fresh
drive 'POWER_MODE=phone; usb_power_sampler_start; sleep 2; usb_power_sampler_stop' >/dev/null 2>&1
check "session header written"            has "$T/logs/usb-gamepad-power.txt" '^--- session power mode=phone'
check "at least one sample logged"        has "$T/logs/usb-gamepad-power.txt" '^t=.* usb_online=1'
check "RAM sample file removed"           test ! -e "$T/samples.txt"
check "phone sampler never wrote the PMIC node" test "$(cat "$T/fs/sys/class/axp/axp_reg")" = SENTINEL

fresh
i=0; while [ "$i" -lt 7000 ]; do echo "old-$i"; i=$((i + 1)); done > "$T/logs/usb-gamepad-power.txt"
echo "t=1 usb_online=1" > "$T/samples.txt"
drive 'POWER_MODE=pc; usb_power_sampler_stop' >/dev/null 2>&1
check "log is trimmed to a bounded size"  test "$(wc -l < "$T/logs/usb-gamepad-power.txt")" -le 4001
check "newest entries survive trimming"   has "$T/logs/usb-gamepad-power.txt" 'usb_online=1'

# --- how launch.sh uses it -----------------------------------------------------
check "launch.sh is valid sh"             sh -n "$LAUNCH"
check "binary gets POWER_ARGS, nothing else changed" grep -q 'usbgamepad.elf --profile pc --layout "\$INITIAL_LAYOUT" \$POWER_ARGS >> "\$LOG_FILE" 2>&1' "$LAUNCH"
check "cleanup stops the sampler"         sh -c "sed -n '/^cleanup() {/,/^}/p' '$LAUNCH' | grep -q usb_power_sampler_stop"

if [ "$FAILED" = 0 ]; then
	echo "launch power-mode tests: $N checks passed"
	exit 0
fi
echo "launch power-mode tests: FAILED"
exit 1
