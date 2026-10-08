# MTP on tg5040

NX Redux uses uMTP-Responder in FunctionFS mode because the stock `mtp.gs0`
function advertises a vendor-specific `ff/ff/00` interface. Windows does not
match that interface to its inbox `wpdmtp.inf` driver. uMTP-Responder advertises
the standards-compliant Still Image interface `06/01/01`, which Windows binds
as a WPD/MTP device while the stock FunctionFS ADB interface remains present.

Runtime files:

- `skeleton/SYSTEM/tg5040/bin/umtprd`
- `skeleton/SYSTEM/tg5040/etc/umtprd.conf`
- `skeleton/SYSTEM/tg5040/bin/nx_usb_mode.sh`

The ARM64 binary is from Debian's `umtp-responder_1.3.10-1_arm64.deb`
(2020 build, SHA-256 of the deb:
`02EA5506335D8D45EE7E767794C143170A236B397BDAD5E78DFC71A528BD21B7`).
It only requires GLIBC 2.17; the Brick Pro stock userspace provides GLIBC 2.33.
The corresponding Debian copyright file is shipped under
`skeleton/SYSTEM/tg5040/licenses/umtp-responder-copyright.txt`.

`nx_usb_mode.sh restore` creates `functions/ffs.mtp`, mounts FunctionFS at
`/dev/ffs-umtp`, starts the responder, adds the function beside `ffs.adb`, and
re-enumerates the bottom USB-C gadget. An already healthy MTP configuration
is left alone. Restore requires a genuinely mounted SD and refuses a live
USB Storage session or nonempty backing LUN. `stop` verifies that MTP processes
and the FunctionFS mount have stopped, then verifies ADB restoration.

## USB Storage ownership

The pak stages its own shell controller and a copy of `nx_usb_mode.sh` in RAM.
It uses the stock `usb_storage` UI directly, with rootfs libraries, NOT stock
`launch.sh`. Stock's automatic filesystem repair is therefore not executed.
The tg5040 updater, platform boot script and supervisor also re-exec RAM
copies: changing only cwd does not close their open script descriptors.
Platform boot keeps its install assets at the original `.tmp_update` path;
the supervisor keeps the normal pak cwd for NextUI, but boot daemons inherit
RAM cwd. Other platforms are not re-exec'd by the common updater.

1. Remember MTP/ADB and stop MTP.
2. Stage `nx_usb_services.sh`, snapshot the exact executables of running
   keymon, audiomon, musicplayerd, osdmusic and the model's stock OSD; stop
   only those owners. Music receives its state-saving shutdown command.
   Stock inputd is left running for the stock USB UI's B button. A remaining
   service is not SIGKILL'd; the tool must refuse UMS.
3. Sync, genuinely unmount SD (no lazy/force).
4. `storage-start /dev/mmcblk1p1` checks whole-card mount aliases, other
   partitions and swap, then verifies the stock mass-storage + ADB gadget.
5. Run the stock UI.
6. `storage-detach` unbinds the gadget, unlinks mass-storage functions and
   clears/verifies all backing LUN files BEFORE remount or MTP restoration.
   If no mass-storage links or open LUNs exist, no unrelated ADB/MTP unbind
   is needed; a safely refused entry must not freeze the supervisor.
7. Remount with the original filesystem/options, restore only services that
   were running before entry, retry the previous USB mode up to three times,
   and persist the RAM logs. The wrapper is the sole UMS restore owner;
   the supervisor's fallback is retained only for USB Gamepad.

Failed detach/remount leaves `/tmp/nx_usb_storage_remount_blocked`; the
supervisor must not resume SD programs or restore MTP behind that guard.
Other SD holders are diagnosed, never killed indiscriminately. A real busy
card may still reject UMS until those holders are addressed on hardware.
Service recovery state remains in RAM on a failed restore. Process identity
is checked by full executable path, not just PID/comm; foreign same-name
processes are not signalled. No service is restarted while the SD is absent
or a USB Storage/blocked marker is present.
Windows must finish transfers and eject the drive before B is pressed;
disconnecting the gadget cannot flush pending data in Windows' own cache.

## Temporary heat diagnosis

The launcher stages `nx_usb_diag.sh` in `/tmp`. It samples every 2 seconds,
at most 1800 samples (roughly an hour plus sampling overhead). It records CPU
busy percentage/frequency/governor, raw thermal/power-supply attributes,
USB connection state, card I/O counters, and top process CPU-tick deltas every
10 seconds. It does not change the CPU governor or claim a cause of heating.
Samples stay in RAM, including while the card is exported. After a safe
remount, the pak saves `usb-storage.txt`, `usb-thermal.txt`, `usb-mode.txt`
and `usb-services.txt`
under `.userdata/tg5040/logs`, independent of general debug logging settings.

Host fixtures: `python scripts/tests/test-usb-safety.py`. They exercise safe
ordering, aliases/swap, partial configuration, detach/remount failures,
MTP guards/idempotence, service stop/restore and boot RAM re-exec, plus bounded
sensor logging. `none` is treated as an unbound controller, not a healthy
MTP/ADB device. The coordinator stops the old responder before unlinking its
FunctionFS interface, and waits/verifies UDC detach instead of assuming an
immediate blank read. It does not use the top port GPIO or usb_null.

On this Windows workspace (the repo path contains spaces), use a space-free
`NX_USB_TEST_TEMP`, e.g. a task folder under the Windows temporary directory.
`NX_USB_TEST_STAGED_DIR` can select the seven exact card-install candidates
instead of source files for another fixture run.

Fixtures cannot confirm USB
enumeration, actual unmount holders, transfer integrity or temperature.
