#!/bin/sh
# Host test for the per-game play-time helper hook in the tg5040 supervisor
# (skeleton/SYSTEM/tg5040/paks/MinUI.pak/launch.sh: nx_playtime_start/stop).
#
# Extracts the real hook block, redirects its /tmp paths into a scratch dir and
# drives it with fake daemons. The requirement under test: a missing, broken,
# hanging or crashing helper must NEVER stop a game from launching, and a
# healthy helper must be handed to the game (NX_PLAYTIME_PID) and stopped after.
#
# Usage: sh scripts/tests/test-playtime-hook.sh   (SH=busybox\ sh to try ash)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SUP="$ROOT/skeleton/SYSTEM/tg5040/paks/MinUI.pak/launch.sh"
SHELL_UNDER_TEST="${SH:-sh}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
FAIL=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAIL=$((FAIL + 1)); }

# --- extract the hook (from NX_PLAYTIME_DAEMON="" through nx_playtime_blocked) ---
tr -d '\r' < "$SUP" | awk '
	/^NX_PLAYTIME_DAEMON=""/ { p = 1 }
	p { print }
	p && /^nx_playtime_blocked\(\)/ { s = 1 }
	s && /^}/ { exit }
' > "$T/hook.raw"
[ -s "$T/hook.raw" ] || { echo "FAIL: could not extract the hook from the supervisor"; exit 1; }
mkdir -p "$T/tmp" "$T/system/bin"
# Keep every path inside the scratch dir; never touch the real /tmp.
sed -e "s#/tmp/#$T/tmp/#g" -e "s#cd /tmp #cd $T/tmp #g" "$T/hook.raw" > "$T/hook.sh"
grep -q "$T/tmp/nx-playtime.pid" "$T/hook.sh" || { echo "FAIL: path rewrite did not apply"; exit 1; }

# The supervisor must really call the hook around each launch.
tr -d '\r' < "$SUP" | grep -q '^[[:space:]]*nx_playtime_start "\$NEXT_PATH"' && pass "supervisor calls nx_playtime_start before the launch" || fail "supervisor does not call nx_playtime_start"
tr -d '\r' < "$SUP" | awk '/nx_playtime_start "\$NEXT_PATH"/{a=NR} /^[[:space:]]*eval "\$CMD"/{b=NR} /^[[:space:]]*nx_playtime_stop$/{c=NR} END{exit !(a && b && c && a<b && b<c)}' && pass "start < eval \$CMD < stop ordering" || fail "start/eval/stop ordering is wrong"

# --- fake daemons -----------------------------------------------------------
# is_game: exit 0 when the queued command mentions a ROM, 1 for a tool.
write_daemon() { # $1 = behaviour of --run: ok | nopid | crash | wrongpid
	cat > "$T/system/bin/playtimed.elf" <<EOF
#!/bin/sh
case "\$1" in
	--is-game) grep -q 'Roms' "\$2" && exit 0; exit 1 ;;
	--run)
		# The real pid, independent of what the hook remembers, so a helper the
		# hook forgot to kill is still detected.
		echo \$\$ > "$T/tmp/real.pid"
		case "$1" in
			ok)       echo \$\$ > "$T/tmp/nx-playtime.pid"; exec sleep 30 ;;
			nopid)    exec sleep 30 ;;
			crash)    exit 3 ;;
			wrongpid) echo 1 > "$T/tmp/nx-playtime.pid"; exec sleep 30 ;;
		esac ;;
esac
exit 2
EOF
	chmod +x "$T/system/bin/playtimed.elf"
}
run_case() { # $1 = case name; reads $T/cmd (the queued command), prints "rc|pid|alive" for the helper after start
	rm -f "$T/tmp/real.pid"
	$SHELL_UNDER_TEST -c '
		SYSTEM_PATH="$1"; export SYSTEM_PATH
		. "$2"
		nx_playtime_start "$3"; rc=$?
		child="$NX_PLAYTIME_CHILD"
		real="$(cat "$4" 2>/dev/null)"
		alive=no; [ -n "$real" ] && kill -0 "$real" 2>/dev/null && alive=yes
		echo "start_rc=$rc pid=${NX_PLAYTIME_PID:-unset} child=${child:-none} alive=$alive"
		nx_playtime_stop
		after=no; [ -n "$real" ] && kill -0 "$real" 2>/dev/null && after=yes
		[ "$after" = no ] || kill "$real" 2>/dev/null   # never leave a test process behind
		echo "after_stop pid=${NX_PLAYTIME_PID:-unset} child=${NX_PLAYTIME_CHILD:-none} still_alive=$after"
	' sh "$T/system" "$T/hook.sh" "$T/cmd" "$T/tmp/real.pid"
}
rm -f "$T/tmp/"* 2>/dev/null
printf '%s\n' "\"/mnt/SDCARD/Emus/FC.pak/launch.sh\" \"/mnt/SDCARD/Roms/Nintendo ES (FC)/Contra.zip\"" > "$T/cmd.game"
printf '%s\n' "\"/mnt/SDCARD/.system/paks/Tools/Settings.pak/launch.sh\"" > "$T/cmd.tool"

# 1) helper not installed: nothing happens, launch proceeds
rm -f "$T/system/bin/playtimed.elf"; cp "$T/cmd.game" "$T/cmd"
out="$(run_case missing)"
echo "$out" | grep -q 'start_rc=0 pid=unset child=none' && pass "no helper installed: start returns 0, no pid exported" || fail "no helper installed: $out"

# 2) a tool (not a game): helper is not started
write_daemon ok; cp "$T/cmd.tool" "$T/cmd"
out="$(run_case tool)"
echo "$out" | grep -q 'start_rc=0 pid=unset child=none' && pass "tool launch is not tracked" || fail "tool launch: $out"

# 3) healthy helper: pid handed to the game, killed after stop
rm -f "$T/tmp/nx-playtime.pid"; cp "$T/cmd.game" "$T/cmd"
out="$(run_case ok)"
echo "$out" | grep -Eq 'start_rc=0 pid=[0-9]+ child=[0-9]+ alive=yes' && pass "healthy helper: pid exported and helper alive during the game" || fail "healthy helper: $out"
echo "$out" | grep -q 'after_stop pid=unset child=none still_alive=no' && pass "healthy helper: stopped and pid unset after the game" || fail "healthy helper stop: $out"

# 4) helper never publishes its pid: launch must continue, helper must be cleaned up
rm -f "$T/tmp/nx-playtime.pid"; write_daemon nopid
s=$(date +%s); out="$(run_case nopid)"; e=$(date +%s)
echo "$out" | grep -q 'start_rc=0 pid=unset' && pass "silent helper: start returns 0 and exports no pid" || fail "silent helper: $out"
[ $((e - s)) -le 5 ] && pass "silent helper: gave up within 5 s (took $((e - s)) s)" || fail "silent helper took $((e - s)) s"
echo "$out" | grep -q 'alive=no' && pass "silent helper: killed, not left running" || fail "silent helper left running: $out"

# 5) helper crashes at once: must not delay or break the launch
rm -f "$T/tmp/nx-playtime.pid"; write_daemon crash
out="$(run_case crash)"
echo "$out" | grep -q 'start_rc=0 pid=unset' && pass "crashing helper: start returns 0, no pid exported" || fail "crashing helper: $out"

# 6) helper publishes a pid that is not its own: must not be trusted
rm -f "$T/tmp/nx-playtime.pid"; write_daemon wrongpid
out="$(run_case wrongpid)"
echo "$out" | grep -q 'start_rc=0 pid=unset' && pass "mismatched pid is rejected" || fail "mismatched pid accepted: $out"

# 7) daily-limit gate: nx_playtime_blocked is true ONLY when the helper answers 3
tr -d '\r' < "$SUP" | awk '/nx_playtime_blocked "\$NEXT_PATH"/{a=NR} /nx_playtime_start "\$NEXT_PATH"/{b=NR} /^[[:space:]]*eval "\$CMD"/{c=NR} END{exit !(a && b && c && a<b && b<c)}' && pass "gate runs before start/eval" || fail "gate is not ahead of start/eval"
gate() { # $1 = what --check does: block | allow | crash | missing ; prints rc of nx_playtime_blocked
	rm -f "$T/system/bin/playtimed.elf"
	if [ "$1" != missing ]; then
		cat > "$T/system/bin/playtimed.elf" <<EOF
#!/bin/sh
case "\$1" in
	--check) case "$1" in block) exit 3 ;; allow) exit 0 ;; crash) exit 139 ;; esac ;;
esac
exit 2
EOF
		chmod +x "$T/system/bin/playtimed.elf"
	fi
	$SHELL_UNDER_TEST -c '
		SYSTEM_PATH="$1"; export SYSTEM_PATH
		. "$2"
		nx_playtime_blocked "$3"; echo "rc=$?"' sh "$T/system" "$T/hook.sh" "$T/cmd.game" 2>/dev/null | tail -1
}
[ "$(gate block)" = "rc=0" ] && pass "gate: helper says 3 -> the launch is blocked" || fail "gate block: $(gate block)"
[ "$(gate allow)" = "rc=1" ] && pass "gate: helper says 0 -> the launch goes ahead" || fail "gate allow: $(gate allow)"
[ "$(gate crash)" = "rc=1" ] && pass "gate: crashing helper fails open" || fail "gate crash: $(gate crash)"
[ "$(gate missing)" = "rc=1" ] && pass "gate: missing helper fails open" || fail "gate missing: $(gate missing)"

if [ "$FAIL" -eq 0 ]; then echo "test-playtime-hook ($SHELL_UNDER_TEST): all checks passed"; exit 0; fi
echo "test-playtime-hook: $FAIL FAILURE(S)"; exit 1
