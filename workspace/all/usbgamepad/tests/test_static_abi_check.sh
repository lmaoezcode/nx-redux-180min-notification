#!/bin/sh
# Host test for scripts/check-static-abi.py (packaging gate for static helpers).
# Builds tiny synthetic AArch64 ELF files with a chosen .note.ABI-tag.
# Usage: sh tests/test_static_abi_check.sh   (from workspace/all/usbgamepad)

HERE="$(CDPATH= cd "$(dirname "$0")" && pwd)"
REPO="$(CDPATH= cd "$HERE/../../../.." && pwd)"
CHECK="$REPO/scripts/check-static-abi.py"
PY=""
for cand in "${PYTHON:-}" python3 python; do
	[ -n "$cand" ] && "$cand" -c 'import sys; sys.exit(sys.version_info[0] != 3)' >/dev/null 2>&1 && { PY="$cand"; break; }
done
[ -n "$PY" ] || { echo "SKIP/FAIL: no working python3 found"; exit 1; }
T="$(mktemp -d "${TMPDIR:-/tmp}/nxabi-test.XXXXXX")" || exit 1
trap 'rm -rf "$T"' EXIT
FAILED=0
N=0

"$PY" -I - "$T" <<'PYEOF' || exit 1
import struct, sys, os
out = sys.argv[1]
def elf(name, abi=None, interp=False, etype=2):
    phs = []
    blobs = []
    if interp:
        blobs.append((3, b"/lib/ld-linux-aarch64.so.1\0"))
    if abi:
        desc = struct.pack("<IIII", 0, *abi)
        blobs.append((4, struct.pack("<III", 4, 16, 1) + b"GNU\0" + desc))
    hdr = 64
    ph = 56 * len(blobs)
    off = hdr + ph
    body = b""
    for t, data in blobs:
        phs.append(struct.pack("<IIQQQQQQ", t, 4, off + len(body), 0, 0, len(data), len(data), 4))
        body += data
    ehdr = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8
    ehdr += struct.pack("<HHIQQQIHHHHHH", etype, 183, 1, 0, hdr if blobs else 0, 0, 0, 64, 56, len(blobs), 64, 0, 0)
    open(os.path.join(out, name), "wb").write(ehdr + b"".join(phs) + body)
elf("static_ok_3_7.elf", (3, 7, 0))
elf("static_old_2_6.elf", (2, 6, 32))
elf("static_bad_4_19.elf", (4, 19, 0))
elf("static_no_note.elf")
elf("dynamic_4_19.elf", (4, 19, 0), interp=True)
elf("shared_4_19.so", (4, 19, 0), etype=3)
open(os.path.join(out, "script.sh"), "w").write("#!/bin/sh\n")
PYEOF

run() { # args...
	"$PY" -I "$CHECK" "$@" > "$T/out" 2>&1
	RC=$?
}
expect() { # description, expected rc
	N=$((N + 1))
	if [ "$RC" != "$2" ]; then
		echo "FAIL: $1 (rc=$RC, wanted $2)"; sed 's/^/  | /' "$T/out"; FAILED=1
	fi
}
mention() { # description, pattern
	grep -q -- "$2" "$T/out" || { echo "FAIL: $1 (output lacks '$2')"; sed 's/^/  | /' "$T/out"; FAILED=1; }
}

run "$T/static_ok_3_7.elf";   expect "ABI 3.7.0 accepted" 0
run "$T/static_old_2_6.elf";  expect "ABI 2.6.32 accepted" 0
run "$T/static_no_note.elf";  expect "static without note accepted" 0
run "$T/static_bad_4_19.elf"; expect "ABI 4.19.0 rejected" 1
mention "names the file" "static_bad_4_19.elf"
mention "explains the failure" "kernel too old"
run "$T/dynamic_4_19.elf";    expect "dynamic ABI 4.19 not a static helper" 0
run "$T/shared_4_19.so";      expect "shared object skipped" 0
run "$T/script.sh";           expect "non-ELF skipped" 0
run "$T";                     expect "directory scan fails on the bad file" 1
run --max 4.19.0 "$T/static_bad_4_19.elf"; expect "--max raises the limit" 0
run "$T/missing.elf";         expect "unreadable path is an error, not a pass" 2
run;                          expect "no arguments is a usage error" 2

# Real-world sample when the author's backup exists (skipped elsewhere).
BK="/d/NXRedux-backups/NXRedux-v2-overlay-backup-20261009-133324/files/.system/bin/nx_usb_guard.elf"
if [ -f "$BK" ]; then
	run "$BK"; expect "the Docker-built guard that broke restore is rejected" 1
fi

if [ "$FAILED" = 0 ]; then
	echo "static ABI check tests: $N checks passed"
	exit 0
fi
echo "static ABI check tests: FAILED"
exit 1
