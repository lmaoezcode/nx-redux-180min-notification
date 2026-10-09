# Cross-building and host-testing small helpers on Windows (WSL2, no Docker)

For the small, self-contained tg5040 helpers (`nx_usb_guard.elf`, `playtimed.elf`)
a plain aarch64 cross compiler in WSL2 is enough, and the result links
statically, so it does not depend on the device's glibc. Anything that links
SDL2/libretro/rcheevos (`minarch`, `settings`, `nextui`, ...) still needs the
project toolchain image (Docker, see `BUILD.md`): use this page only for the
helpers below.

Verified on Ubuntu 26.04 under WSL2 (GCC 15.2, qemu-user 10).

## 1. One-time setup (inside the WSL distro)

```sh
sudo apt-get update
sudo apt-get install -y build-essential gcc-aarch64-linux-gnu qemu-user \
    busybox-static zip unzip jq pkg-config sqlite3 \
    libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev
```

`qemu-user` lets you run the aarch64 binaries and their tests on the PC.
WSL cannot register binfmt (the apt hook prints a `systemd-binfmt` error): that
is harmless, call `qemu-aarch64 <binary>` explicitly or through a small wrapper.

## 2. Work on a Linux-side copy of the repo

Windows checks scripts out as CRLF when `core.autocrlf=true`, and `/mnt/c`
has no exec bit, so many shell tests misbehave there. Copy to ext4 and
normalise:

```sh
rsync -a --delete --exclude='.git' --exclude='.tmp*' --exclude='.zig*' \
    --exclude='releases' --exclude='toolchains' --exclude='build' \
    "/mnt/c/path/to/nx redux/" ~/nx/
cd ~/nx
find . -type f \( -name '*.sh' -o -name 'updater' -o -name '*.py' -o -name '*.c' -o -name '*.h' \) \
    -exec sed -i 's/\r$//' {} +
find . -type f \( -name '*.sh' -o -name 'updater' \) -exec chmod +x {} +
```

(A local `.git/info/attributes` with `*.sh text eol=lf` keeps new checkouts LF
without adding a tracked `.gitattributes`.)

## 3. Build the helpers

```sh
# usbguard: same flags as workspace/tg5040/usbguard/Makefile (it already links -static)
aarch64-linux-gnu-gcc -std=c11 -Os -Wall -Wextra -Werror -static \
    -o nx_usb_guard.elf workspace/tg5040/usbguard/usbguard.c

# playtimed: the Makefile does not link statically, so add -static here to make
# the binary independent of the device's glibc
cd workspace/all/playtimed
aarch64-linux-gnu-gcc daily.c playtimed.c -std=gnu99 -O2 -Wall -Wextra -Werror -static \
    -o build/tg5040/playtimed.elf
```

Check the result: `readelf -h x.elf` must say `AArch64` / `EXEC` and
`readelf -d x.elf | grep -c NEEDED` must print `0`.

Also check the kernel ABI the static binary demands:

```sh
readelf -n x.elf | grep -A1 'ABI tag'     # must say "ABI: 3.7.0" (or lower than the device kernel)
```

**Do not build `nx_usb_guard.elf` or `playtimed.elf` with the Docker toolchain
(`ghcr.io/loveretro/tg5040-toolchain`) using `-static`.** Its glibc is built for
Linux 4.19+ (`file` says "for GNU/Linux 4.19.0"), and a static binary carries that
requirement with it: on the Brick Pro's older kernel it aborts at start with
`FATAL: kernel too old`. `nx_usb_mode.sh` then fails closed ("MTP did not stop. USB
Storage cancelled."), so nothing is exported, but USB Storage does not work. The
dynamically linked binaries the Docker build produces (`minarch`, `nextui`,
`settings`, ...) are fine because they run on the device's own glibc. Build the two
static helpers with the WSL/`aarch64-linux-gnu-gcc` commands above and verify with
`qemu-aarch64 nx_usb_guard.elf --selftest`.

## 4. Test the aarch64 binaries on the PC

```sh
qemu-aarch64 nx_usb_guard.elf --selftest          # expects selftest_passed=7 expected=7

# playtimed: test build + a wrapper so the Python tests can exec it
aarch64-linux-gnu-gcc -DPLAYTIMED_TEST daily.c playtimed.c -std=gnu99 -O2 -Wall -Wextra -Werror \
    -static -o /tmp/playtimed_test
printf '#!/bin/sh\nexec qemu-aarch64 /tmp/playtimed_test "$@"\n' > /tmp/pt && chmod +x /tmp/pt
python3 tests/test_daemon.py /tmp/pt
aarch64-linux-gnu-gcc daily.c tests/test_daily.c -std=gnu99 -Wall -Wextra -Werror -static -o /tmp/td
qemu-aarch64 /tmp/td
```

Shell logic can be tried under the device's shell: `SH="busybox sh" sh scripts/tests/test-playtime-hook.sh`.

## 5. Host test baseline (2026-10-08)

Of the 41 scripts in `scripts/tests/`, with the packages above installed:
passing: the USB safety suite, the play-time hook, audiomon, cpu-policy, cheat,
rgba, scraper, search-hint, menu-animations, minarch-bench, extras installs for
cheatdb/gen1recomp/psp and about a dozen more. Known failures that also fail on
a clean `HEAD` (not caused by local work):

- `test-update-portmaster-refresh.sh`: the refresh block ends with
  `[ -n "$PM_VER" ] && ...`, which returns 1 when the catalog has no `meta.txt`,
  and the fixture never creates one.
- `test-portmaster-launch.sh` (`xtras marker`), `test-extras-portmaster-install.sh`
  (`version record`): the version marker is not written in the fixture.
- `test-dc-netplay-sh.sh`: the tg5050 launcher writes to `/sys/...`.
- `test-art-style.sh`: the test's own C trips `-Werror=format-truncation` on GCC 14+.

Not runnable on a PC: tests that need a device or adb (`deep-sleep-e2e`,
`rumble-switch-e2e`, `wifi-connect-e2e`, `kernel-usb-inspection`) and the six
that call `docker`.

## 6. Pitfalls seen on Windows

- A repo path containing a space (`nx redux`) breaks some test fixtures that
  build shell commands from `$ROOT`. Set `NX_USB_TEST_TEMP` to a space-free
  directory for `test-usb-safety.py`, or work in the WSL copy.
- Git Bash rewrites `/mnt/c/...` arguments passed to `wsl.exe`; call `wsl.exe`
  from PowerShell, or put the commands in a script and pass only its path.
- MSYS2's `ucrt64` gcc has no POSIX signals/`localtime_r`; build the daemon in
  WSL.
- A cross-built static binary will not be byte-identical to one built by the
  project's Docker toolchain. For a reproducible release build use `BUILD.md`.
