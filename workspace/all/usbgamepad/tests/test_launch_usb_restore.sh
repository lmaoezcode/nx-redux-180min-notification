#!/bin/sh
# Host test for the USB-mode safety block in USB Gamepad.pak/launch.sh.
#
# It extracts the block between the "USB mode safety begin/end" markers and runs
# it against stub nx_usb_mode.sh / nx_usb_guard.elf / show2.elf. No device, USB
# or SD access. Also checks the REAL nx_usb_mode.sh: with a missing or aborting
# guard it must exit non-zero and run nothing (fail-closed, AGENT.md 11.1).
#
# Usage: sh tests/test_launch_usb_restore.sh   (from workspace/all/usbgamepad)

HERE="$(CDPATH= cd "$(dirname "$0")" && pwd)"
REPO="$(CDPATH= cd "$HERE/../../../.." && pwd)"
LAUNCH="$REPO/skeleton/SYSTEM/tg5040/paks/Tools/USB Gamepad.pak/launch.sh"
REAL_MODE="$REPO/skeleton/SYSTEM/tg5040/bin/nx_usb_mode.sh"

T="$(mktemp -d "${TMPDIR:-/tmp}/nxgp-test.XXXXXX")" || exit 1
trap 'rm -rf "$T"' EXIT
FAILED=0
CASES=0

sed -n '/^# --- USB mode safety begin/,/^# --- USB mode safety end/p' "$LAUNCH" > "$T/block.sh"
[ -s "$T/block.sh" ] || { echo "FAIL: safety block not found in launch.sh"; exit 1; }

mkdir -p "$T/bin" "$T/sys/res"
# Stub helper: records every action; behaviour driven by files in $STUB_DIR.
cat > "$T/bin/nx_usb_mode.sh" <<'EOF'
#!/bin/sh
echo "$1" >> "$STUB_DIR/calls"
case "$1" in
	status) cat "$STUB_DIR/state" ;;
	stop)
		[ -e "$STUB_DIR/stop_fail" ] && exit 1
		echo adb > "$STUB_DIR/state"
		;;
	link-stop)
		# A USB Cable link left behind: gives back the mode recorded in link_prior.
		[ -e "$STUB_DIR/link_stop_fail" ] && exit 1
		cat "$STUB_DIR/link_prior" > "$STUB_DIR/state"
		;;
	restore)
		[ -e "$STUB_DIR/restore_abort" ] && exit 134
		[ -e "$STUB_DIR/restore_fail" ] && exit 1
		echo mtp > "$STUB_DIR/state"
		;;
esac
exit 0
EOF
cat > "$T/bin/show2.elf" <<'EOF'
#!/bin/sh
echo "$*" >> "$STUB_DIR/show2"
EOF
chmod +x "$T/bin/nx_usb_mode.sh" "$T/bin/show2.elf"

# $1 = missing | ok | abort | exec-fail
make_guard() {
	rm -f "$T/bin/nx_usb_guard.elf"
	case "$1" in
		ok)
			printf '#!/bin/sh\nexit 0\n' > "$T/bin/nx_usb_guard.elf" ;;
		abort)
			# What a glibc static binary built for a newer kernel does.
			printf '#!/bin/sh\necho "FATAL: kernel too old" >&2\nkill -ABRT $$\n' > "$T/bin/nx_usb_guard.elf" ;;
		exec-fail)
			printf '#!/bin/sh\nexit 126\n' > "$T/bin/nx_usb_guard.elf" ;;
	esac
	[ -e "$T/bin/nx_usb_guard.elf" ] && chmod +x "$T/bin/nx_usb_guard.elf"
}

# Starts a case: fresh state dir, initial USB mode $1, guard kind $2.
begin() {
	CASES=$((CASES + 1))
	CASE_NAME="$3"
	rm -rf "$T/state" "$T/log.txt" "$T/failmarker"
	mkdir -p "$T/state"
	: > "$T/state/calls"; : > "$T/state/show2"; : > "$T/log.txt"
	echo "$1" > "$T/state/state"
	make_guard "$2"
}

# Runs one shell snippet with the block sourced; sets RC.
drive() {
	STUB_DIR="$T/state" LOG_FILE="$T/log.txt" SYSTEM_PATH="$T/sys" \
	NX_USB_GUARD="$T/bin/nx_usb_guard.elf" PATH="$T/bin:$PATH" \
	sh -c ". '$T/block.sh'; USB_FAIL_MARKER='$T/failmarker'; $1"
	RC=$?
}

check() { # description, command...
	desc="$1"; shift
	if "$@"; then return 0; fi
	echo "FAIL [$CASE_NAME]: $desc"
	echo "  calls: $(tr '\n' ' ' < "$T/state/calls")"
	echo "  log:   $(tr '\n' '|' < "$T/log.txt")"
	FAILED=1
}
called()     { grep -qx "$1" "$T/state/calls"; }
not_called() { ! grep -qx "$1" "$T/state/calls"; }
count_is()   { [ "$(grep -cx "$1" "$T/state/calls")" = "$2" ]; }
only_status(){ ! grep -vx status "$T/state/calls" | grep -q .; }
logged()     { grep -q -- "$1" "$T/log.txt"; }
shown()      { [ -s "$T/state/show2" ]; }
not_shown()  { [ ! -s "$T/state/show2" ]; }
state_is()   { [ "$(cat "$T/state/state")" = "$1" ]; }
rc_is()      { [ "$RC" = "$1" ]; }
exists()     { [ -e "$1" ]; }

# 1. Guard file missing: refuse, nothing but read-only status may run.
begin mtp missing "guard missing"
drive 'usb_preflight'
check "preflight refuses" rc_is 1
check "only status was called" only_status
check "no stop/restore" not_called stop
check "log names the guard" logged "USB guard missing"
check "user is told" shown

# 2. Guard aborts with 'kernel too old' (the 9 Oct failure).
begin mtp abort "guard abort"
drive 'usb_preflight'
check "preflight refuses" rc_is 1
check "only status was called" only_status
check "log has abort rc" logged "rc=134"
check "kernel-too-old text kept in log" logged "kernel too old"
check "user is told" shown
check "USB mode untouched" state_is mtp

# 3. Guard exists but cannot be executed (rc 126).
begin mtp exec-fail "guard not runnable"
drive 'usb_preflight'
check "preflight refuses" rc_is 1
check "no stop/restore" only_status

# 4. Happy path from MTP: stop then exactly one restore, back to mtp.
begin mtp ok "happy path mtp"
drive 'usb_preflight && usb_enter_gamepad_mode && restore_usb_mode'
check "all steps ok" rc_is 0
check "one stop" count_is stop 1
check "one restore" count_is restore 1
check "back to mtp" state_is mtp
check "nothing shown" not_shown
check "no failure marker" test ! -e "$T/failmarker"

# 5. Session that started from ADB must end in ADB: no restore (MTP) call.
begin adb ok "pre-mode adb"
drive 'usb_preflight && usb_enter_gamepad_mode && restore_usb_mode'
check "all steps ok" rc_is 0
check "restore not called" not_called restore
check "still adb" state_is adb

# 6. restore fails: one attempt only, reported, state left as is.
begin mtp ok "restore fails"
touch "$T/state/restore_fail"
drive 'usb_preflight && usb_enter_gamepad_mode; restore_usb_mode'
check "restore_usb_mode reports failure" rc_is 1
check "restore tried exactly once" count_is restore 1
check "log says not restored" logged "USB_MODE_NOT_RESTORED"
check "log has expected/actual" logged "expected=mtp actual=adb"
check "user is told" shown
check "marker written" exists "$T/failmarker"
check "left in known ADB state" state_is adb

# 7. Guard aborts DURING restore (rc 134): reported, no second attempt.
begin mtp ok "guard aborts in restore"
touch "$T/state/restore_abort"
drive 'usb_preflight && usb_enter_gamepad_mode; restore_usb_mode'
check "failure reported" rc_is 1
check "no retry" count_is restore 1
check "rc 134 logged" logged "restore_rc=134"
check "user is told" shown

# 8. stop fails: session does not start; cleanup still verifies the mode.
begin mtp ok "stop fails"
touch "$T/state/stop_fail"
drive 'usb_preflight; usb_enter_gamepad_mode; echo $? > "$STUB_DIR/enter_rc"; restore_usb_mode'
check "enter reported failure" test "$(cat "$T/state/enter_rc")" = 1
check "mode unchanged so no restore call" not_called restore
check "user is told" shown
check "still mtp" state_is mtp

# 9. USB Storage active before start: refuse.
begin storage ok "storage active"
drive 'usb_preflight'
check "preflight refuses" rc_is 1
check "only status was called" only_status
check "user is told" shown

# 13. A USB link left behind (status = link): one guarded link-stop gives the
# previous mode back, then the session starts from that mode.
begin link ok "link left behind, mtp before"
echo mtp > "$T/state/link_prior"
drive 'usb_preflight && usb_enter_gamepad_mode && restore_usb_mode'
check "preflight heals and the session runs" rc_is 0
check "link-stop ran exactly once" count_is link-stop 1
check "link-stop came before stop" sh -c "grep -n -x -e link-stop -e stop '$T/state/calls' | head -1 | grep -q link-stop"
check "mode before the session is the one link-stop restored" logged "USB mode before session: mtp"
check "link marker noted in the log" logged "USB link marker found"
check "back to mtp at the end" state_is mtp
check "nothing shown to the user" not_shown

begin link ok "link left behind, adb before"
echo adb > "$T/state/link_prior"
drive 'usb_preflight && usb_enter_gamepad_mode && restore_usb_mode'
check "healed to adb, session runs" rc_is 0
check "no MTP restore for an adb start" not_called restore
check "ends in adb" state_is adb

# 14. link-stop fails: the gadget is not ours; refuse, change nothing else.
begin link ok "link left behind, link-stop fails"
echo mtp > "$T/state/link_prior"
touch "$T/state/link_stop_fail"
drive 'usb_preflight'
check "preflight refuses" rc_is 1
check "tried link-stop once" count_is link-stop 1
check "never stopped or restored" sh -c "! grep -q -x -e stop -e restore '$T/state/calls'"
check "log says busy" logged "USB mode before session is 'link'"
check "user is told" shown
check "state untouched" state_is link

# 15. Guard missing: a link marker must not make the tool call link-stop at all.
begin link missing "link left behind, guard missing"
echo mtp > "$T/state/link_prior"
drive 'usb_preflight'
check "refused before any helper call" rc_is 1
check "only status-free refusal (no link-stop)" not_called link-stop
check "user is told" shown

# 10. Cleanup before the session ever started (early exit): no helper call.
begin mtp missing "cleanup without session"
drive 'restore_usb_mode'
check "returns ok" rc_is 0
check "helper not called at all" test ! -s "$T/state/calls"

# 11. REAL nx_usb_mode.sh: guard missing -> exit 1, runs nothing.
CASES=$((CASES + 1)); CASE_NAME="real script, guard missing"
mkdir -p "$T/real"; : > "$T/real/ran"
cp "$REAL_MODE" "$T/real/nx_usb_mode.sh"
NX_USB_GUARD="$T/real/does-not-exist.elf" /bin/sh "$T/real/nx_usb_mode.sh" restore >/dev/null 2>"$T/real/err"
RC=$?
check "exit status 1" rc_is 1
check "says guard is missing" grep -q "Missing USB transaction guard" "$T/real/err"
check "no lock left behind" test ! -e /tmp/nx_usb_mode.lock

# 12. REAL nx_usb_mode.sh: guard aborts -> non-zero, inner script never runs.
CASES=$((CASES + 1)); CASE_NAME="real script, guard aborts"
cat > "$T/real/guard.elf" <<EOF
#!/bin/sh
echo "FATAL: kernel too old" >&2
echo ran > "$T/real/ran"
kill -ABRT \$\$
EOF
chmod +x "$T/real/guard.elf"
rm -f "$T/real/ran"
(ulimit -c 0; NX_USB_GUARD="$T/real/guard.elf" /bin/sh "$T/real/nx_usb_mode.sh" restore >/dev/null 2>"$T/real/err")
RC=$?
check "non-zero exit" test "$RC" -ne 0
check "guard output reached stderr" grep -q "kernel too old" "$T/real/err"
check "no lock left behind" test ! -e /tmp/nx_usb_mode.lock

if [ "$FAILED" = 0 ]; then
	echo "launch usb restore tests: $CASES cases passed"
	exit 0
fi
echo "launch usb restore tests: FAILED"
exit 1
