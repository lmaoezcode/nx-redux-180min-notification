# USB Gamepad Tool (experimental)

The USB Gamepad tool turns a tg5040 handheld into a USB HID gamepad. It is
designed first for the TrimUI Brick Pro, while the input-name probe and shared
tg5040 mapping also cover the Brick and Smart Pro.

## User flow

1. Before connecting the handheld, start **USB Gamepad**.
2. Connect the **bottom USB-C data/charging port** to a PC or Android device
   with a data-capable cable.
3. The host enumerates `NX Redux USB Gamepad` with the wired HORIPAD-compatible
   input layout: a POV hat, four axes, fourteen raw buttons, and a standard
   Guide/Home control. On Chromium's Windows Gamepad API the mapped result is
   D-pad B12-B15 and HOME B16.
4. Press **F1** to switch between Xbox/position-first and Nintendo/label-first
   face-button layouts. Press **F2** to switch the LCD backlight off or on.
5. Hold **MENU + SELECT for 1.5 seconds** to leave controller mode.

The profile targets Windows, Linux, macOS, and Android HID hosts (Android: see the
phone section below; untested on hardware). It is not
XInput and does not implement the authenticated/proprietary protocols needed
by Xbox, PlayStation, or Nintendo Switch consoles.

## Profiles and game compatibility

The gamepad profile uses the established wired HORIPAD identity and packet
layout (`0f0d:00c1`). SDL classifies this identity as a Switch input-only
controller and reads its simple report without the proprietary Switch Pro
handshake. SDL's public controller database maps raw button 12 to Guide/Home.
Chromium's Windows mapper places the POV directions at standard B12-B15 and
promotes that same raw button 12 to standard Meta/Home B16. The USB product
string remains `NX Redux USB Gamepad`; this is compatibility behavior, not
HORI firmware.

This distinction fixes the V6 design error: advertising seventeen contiguous
raw buttons did not make raw button 16 the standardized Home control, and its
changed report shape failed to enumerate on the tested host. V8 instead emits
the known eight-byte input-only layout and lets the host's established mapping
produce B16. Chromium also reserves B17 for HORIPAD raw button 13, the Switch
Capture/Share control. The Brick Pro does not send that bit: B17 is an unused
slot created by the host's standard mapping, not a second hidden button.
MENU remains the exit chord; F1/F2 and the volume rocker are local controls and
are not advertised as gamepad buttons.

Some Windows games only read XInput. For those, enable Steam Input for the game
so Steam can translate the known DirectInput layout into
the virtual Xbox layout expected by the game.

A real USB XInput profile is not safely expressible through this stock kernel's
FunctionFS implementation. XInput requires a vendor-specific interface and a
16-byte class descriptor. Linux 4.9 FunctionFS rejects descriptor type `0x21`
for vendor-class interfaces; it only accepts the fixed HID/CCID forms. Raw USB
gadget support, which would avoid that parser, is not part of this kernel
generation. Do not add a `045e:028e` identity without the required descriptor:
Windows may bind `xusb22` and then fail the device with Code 10.

## Compatibility and USB lifecycle

Brick Pro's stock 4.9.191 kernel has ConfigFS and FunctionFS, but it was built
with `CONFIG_USB_CONFIGFS_F_HID` disabled and ships no `usb_f_hid` module.
Although `/bin/setusbconfig` still contains old `hid` branches, those branches
cannot create `functions/hid.usb0` on this kernel. This tool therefore exposes
the HID interface through the kernel's existing FunctionFS support and handles
the small HID control protocol in userspace. It needs neither a replacement
kernel nor a write to the internal root filesystem.

While the tool is active it intentionally exposes **HID only**. ADB is removed
from the USB configuration so plugging the controller into an untrusted host
does not also expose a debugging interface. On every normal exit the binary
unbinds the gadget and runs `/bin/setusbconfig adb`. The pak launcher repeats
that recovery if the binary crashes. A reboot remains the final recovery path
after a hard power loss or `SIGKILL`.

The Allwinner BSP does not produce a reliable host-visible disconnect by
unbinding ConfigFS alone. During each mode switch the tool reads the vendor
`usbc0/usb_null` action node, waits one second, then reads `usb_device` before
binding the new gadget. This PHY reset prevents Windows from reusing the old
ADB descriptor and reporting code 43. FunctionFS control transfers also have a
two-second watchdog so a broken or stale host request cannot trap the device in
controller mode indefinitely.

`/tmp/stay_awake` keeps the device awake, `keymon.elf` is paused so MENU does
not open the OSD during the exit chord, and both are restored by the launcher's
trap.

## Phones and other low-power hosts (power mode)

AOSP ships a key layout for this identity (`Vendor_0f0d_Product_00c1.kl`, "HORIPAD
for Nintendo Switch"), and it expects exactly the raw button order this profile
sends, so no separate Android profile is needed. It has **not been confirmed on a
phone yet**: a Xiaomi 11 Ultra (Android 12, MIUI) showed no controller over either
USB-C to USB-C or a USB-C to USB-A OTG adapter, while the same phone accepted a
Flydigi pad. The 18 October log shows two sessions that stayed `not attached` and
one where the phone began enumerating (`ENABLE`, report-descriptor request of 80
bytes) and then dropped the bus. The cause is not known; two candidates are the
port role (both ends dual-role and preferring sink) and the host cutting VBUS
because the handheld charges from it.

To test those, a one-line file selects a **power mode**:

| `.userdata/<platform>/usb-gamepad-power` | Behaviour |
|---|---|
| absent, `pc` or anything else | unchanged: stock charging and port role |
| `phone` | while the controller runs: charger off, input current at its minimum, port held as USB device; all put back on exit, on error, and by `--restore` after a kill |

The `phone` mode reuses `workspace/all/usblink/usblink_power.c`, which the USB cable
netplay link uses for the same registers (AXP2202 `0x19`/`0x17` charger, `0xe3`
USB-C mode); `usbgamepad.elf --power pc|phone` is the switch, and the launcher
passes it only for `phone`.

Every session also appends power and port samples (twice a second) to
`.userdata/<platform>/logs/usb-gamepad-power.txt`: `usb_online` (VBUS),
battery status, UDC state, and in `pc` mode the PMIC registers `0x19 0x17 0xe3
0xe7` (reg `0xe7` low nibble: 3 attached as device, 6 as host). In `phone` mode
the binary logs the register changes itself. Host tests:
`workspace/all/usbgamepad/tests/test_power_mode.c`, `test_launch_power.sh`.

## HID report

The eight-byte PC input report contains:

| Bytes | Data |
|---|---|
| 0-1 | Fourteen raw button bits plus two padding bits |
| 2 | POV hat (low nibble) plus padding (high nibble) |
| 3-6 | Left X/Y and right X/Y, unsigned 8-bit, neutral 128 |
| 7 | Padding |

Face buttons are position-first: bottom, right, left, top become raw b1, b2,
b0 and b3, matching the public HORIPAD mapping. The physical L2/R2 axes are
thresholded into raw b6/b7 and are not exposed as host axes. The D-pad is a POV
hat. HOME is raw b12; the standardized host mapping exposes it as Guide/Home
and, in Chromium on Windows, B16. Raw b13/Capture becomes the standard B17 slot
but is intentionally kept clear. MENU is reserved for the exit chord; F1/F2
and the volume rocker are intentionally left out rather than presented as
gamepad buttons.

F1 changes only the outgoing face-button interpretation. Xbox mode follows
positions (bottom/right/left/top = host A/B/X/Y); Nintendo mode follows the
letters printed on the caps (physical A/B/X/Y = host A/B/X/Y). The switch
needs no USB reconnect. If a face button is held, the transition waits for all
four face buttons to be released, emits one neutral face-button report, and
only then applies the new mapping. This prevents a held button from becoming a
different held button halfway through a press. The selected layout is saved in
the tg5040 userdata directory and restored on the next launch.

F2 writes raw brightness 0 through the same `/dev/disp` ioctl used by
`sleepmon`, while `/tmp/stay_awake` keeps the USB bridge running. The launcher's
captured brightness value is restored on F2, normal exit, a handled signal, or
the crash-recovery `--restore` path. This turns off the panel backlight; it does
not suspend the SoC because suspension would also stop USB controller mode.

F1/F2 do not pulse the local motor. The emulated `0f0d:00c1` wired HORIPAD is
an input-only controller: the corresponding HORI product does not provide
vibration feedback, SDL marks the VID/PID as `SwitchInputOnlyController`, and
SDL returns `Unsupported` for rumble on that input-only path. The FunctionFS
interface therefore intentionally exposes only an interrupt-IN endpoint. The
Brick Pro's physical motor can only become game-controlled through a separate
output-capable protocol plus a host output-report decoder; adding a made-up OUT
endpoint to this HORIPAD identity would not create compatible rumble and could
break the Steam/game compatibility established by v8.

V11 retains the byte-identical v8 PC FunctionFS and HID report descriptors,
but uses a new serial and `bcdDevice` so Windows does not reuse a failed v9/v10
device instance. It provides a valid neutral report before UDC binding and
services ep0 before the continuous input stream, so HID enumeration requests
take priority over local controls and diagnostics. FunctionFS lifecycle events
and the UDC state are logged explicitly. If the host reaches a connected USB
state but never enables the HID function, the bridge performs a full
disconnect/rebind recovery, up to three times, instead of remaining forever in
the `BIND`-only state observed in the failed logs.

The four stick axes use HID usages X/Y/Z/Rz, and the underlying Brick Pro events
remain ABS_X/ABS_Y/ABS_RX/ABS_RY. They are reduced from signed 16-bit source
values to the HORIPAD range 0-255 while preserving endpoints and neutral 128.

## Polling and source-rate diagnostics

The profile advertises a 4 ms full-speed interrupt interval and sends a report
heartbeat every 4 ms while connected (nominally 250 reports/s). New button or
stick input is still forwarded immediately. Repeating the latest state keeps a
regular USB stream but cannot manufacture new analog samples.

Every two seconds the on-device screen and log show separate counters:

- `source_frames`: every `EV_SYN/SYN_REPORT` received from `TRIMUI Player1`.
- `stick_frames`: source frames that contained a changed left/right stick.
- `axes`: changed samples for LX, LY, RX and RY separately.
- `USB`: reports successfully queued to the USB host, including repeated
  heartbeat reports.

Polling 2 measures changes of the selected stick, not repeated identical USB
packets. The measured 17.00 ms minimum and 18.05 ms median (about 55 Hz) are
therefore an effective new-state rate, not the four-millisecond endpoint rate.
On-device logs show about 100 source frames/s while LX and LY individually
change about 50 times/s, with USB reports around 250/s. That rules out a
52-Hz USB endpoint or USB-controller limit, but it does not by itself identify
which later stage coalesces states: the stock `trimui_inputd` event pattern,
the HORIPAD's 8-bit axis quantisation, this bridge's delivery, or Windows/SDL.

V11 logs additional counters to separate those stages:

- `encoded_changes`: distinct eight-byte HORIPAD states after source events
  have been reduced to 8-bit axes;
- `usb_state_changes`: distinct states among successfully queued USB reports;
- `usb_reports`: all USB reports, including unchanged heartbeat packets.

If `encoded_changes` is already around 55 Hz, the loss happens in the source
pattern/8-bit conversion. If it is around 100 but `usb_state_changes` is around
55, the bridge is coalescing it. If both are around 100 while Polling 2 remains
around 55, the remaining limiter is Windows/SDL or the tester. A raw I2C sampler
with `trimui_inputd` temporarily yielding the bus would still be required to
prove the ADC/joystick hardware's own ceiling.

## Source and build

- Bridge/configuration: `workspace/all/usbgamepad/usbgamepad.c`
- Pure input mapper: `workspace/all/usbgamepad/gamepad_mapper.c`
- Pak lifecycle: `skeleton/SYSTEM/tg5040/paks/Tools/USB Gamepad.pak/launch.sh`

The normal tg5040 build produces
`workspace/all/usbgamepad/build/tg5040/usbgamepad.elf` and the root packaging
step copies it into the single `USB Gamepad.pak`. The launcher selects the
DirectInput/SDL profile; the binary still accepts the old browser profile only
for development diagnostics.

Mapper tests have no Linux dependency:

```sh
cd workspace/all/usbgamepad
make test
```

For a quick Brick Pro test without running the full Docker build, the working
tree also produces a release ZIP. Extract it at the SD-card root so
`Tools/tg5040/USB Gamepad.pak/` is present.
That archive contains a static AArch64 build made for this experiment; the
normal NX Redux build still compiles the same source through the tg5040
toolchain.

Logs follow NX Redux's debug-logging policy. Enable **Settings → Developer →
Debug logging** to keep `usb-gamepad.txt` at
`.userdata/tg5040/logs/usb-gamepad.txt`. With debug logging disabled, the
temporary path is `/tmp/nx-logs/usb-gamepad.txt` and is cleared on a later
launch or reboot.

If a previous USB mode switch was interrupted, `--restore` removes the stale
`ffs.nxgamepad` link and mount before asking the stock `setusbconfig adb`
helper to rebuild USB debugging mode.

Hardware verification should cover Windows' Game Controllers panel, Steam
Input, Android, analog endpoints, trigger endpoints, unplug/replug, the exit
chord, and restoration of ADB after both normal and forced exits.
