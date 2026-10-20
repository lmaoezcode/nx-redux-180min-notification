"""USB lifecycle fixtures. No real disks, configfs, mounts or device processes.

Run with Python 3; Windows also needs Git Bash. Kernel I/O is represented by
plain files. This validates ordering/failure paths, not USB enumeration.
"""
from pathlib import Path
from contextlib import contextmanager
import os
import shutil
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[2]
BASH = r'C:\Program Files\Git\bin\bash.exe' if os.name == 'nt' else '/bin/bash'
WRAPPER = ROOT / 'skeleton/SYSTEM/tg5040/paks/Tools/USB Storage.pak/launch.sh'
HELPER = ROOT / 'skeleton/SYSTEM/tg5040/bin/nx_usb_mode.sh'
LIB = ROOT / 'skeleton/SYSTEM/tg5040/bin/nx_usb_lib.sh'
DIAG = ROOT / 'skeleton/SYSTEM/tg5040/bin/nx_usb_diag.sh'
SERVICES = ROOT / 'skeleton/SYSTEM/tg5040/bin/nx_usb_services.sh'
STAGED_DIR = Path(os.environ['NX_USB_TEST_STAGED_DIR']) if os.environ.get('NX_USB_TEST_STAGED_DIR') else None
if STAGED_DIR:
    WRAPPER = STAGED_DIR / 'usb-storage-user.sh'
    HELPER = STAGED_DIR / 'nx_usb_mode.sh'
    SERVICES = STAGED_DIR / 'nx_usb_services.sh'


@contextmanager
def scratch(prefix):
    """Use inherited workspace ACLs; Python 3.13's Windows 0700 temp ACL
    excludes the managed test process after TemporaryDirectory creates it.
    """
    configured = os.environ.get('NX_USB_TEST_TEMP')
    parent = Path(configured) if configured else ROOT / '.local' / 'test-temp'
    parent.mkdir(parents=True, exist_ok=True)
    path = parent / (prefix + uuid.uuid4().hex)
    path.mkdir()
    try:
        yield path
    finally:
        shutil.rmtree(path, ignore_errors=True)


def unix(path):
    value = path.as_posix()
    return '/' + value[0].lower() + value[2:] if os.name == 'nt' else value


def put(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding='utf-8', newline='\n')


def run(base, script, scenario, setup='true', guarded=True, extra_env=None):
    env = os.environ.copy()
    if os.name == 'nt':
        # Git Bash otherwise deep-copies directory targets instead of making
        # symlinks, which would not model Linux configfs at all.
        env['MSYS'] = 'winsymlinks:sys'
    env.update(FIXTURE=unix(base), SCENARIO=scenario, SDCARD_PATH=unix(base / 'sd'),
               SYSTEM_PATH=unix(base / 'system'), PLATFORM='tg5040', NX_USB_GUARDED='1')
    if extra_env:
        env.update(extra_env)
    if not guarded:
        # Exercise the script's own guard gate: no NX_USB_GUARDED, guard missing.
        env.pop('NX_USB_GUARDED')
        env.setdefault('NX_USB_GUARD', unix(base / 'system/bin/no-such-guard.elf'))
    command = ('chmod +x "$FIXTURE/bin"/* "$FIXTURE/system/bin"/* '
               '"$FIXTURE/stock/usb_storage" "$FIXTURE/setter.sh" 2>/dev/null; '
               'export PATH="$FIXTURE/bin:$PATH"; ' + setup + '; sh ' + script)
    result = subprocess.run([BASH, '-c', command], env=env,
                            capture_output=True, text=True, timeout=20)
    return result


def prepare(base):
    for directory in ('bin', 'system/bin', 'stock', 'sd', 'proc', 'gadget/configs/c.1',
                      'gadget/functions/ffs.adb', 'gadget/functions/ffs.mtp',
                      'udc/controller', 'usb-role'):
        (base / directory).mkdir(parents=True, exist_ok=True)
    for name in ('sync', 'usleep', 'sleep', 'show2.elf'):
        put(base / 'bin' / name, '#!/bin/sh\nexit 0\n')
    put(base / 'stock/usb_storage', '''#!/bin/sh
echo ui-start >> "$FIXTURE/events"
[ "$SCENARIO" != cancel_prepare ] || exit 0
while ! /usr/bin/grep -q 'stock UI ready' "$FIXTURE/storage.log"; do /usr/bin/sleep 0.02; done
echo ui >> "$FIXTURE/events"
''')
    put(base / 'system/bin/nx_usb_guard.elf', '#!/bin/sh\nexit 0\n')
    put(base / 'system/bin/nx_usb_lib.sh', '# fixture library (the wrapper tests stub the helper itself)\n')
    put(base / 'mounts', '/dev/mmcblk1p1 ' + unix(base / 'sd') + ' exfat rw 0 0\n')
    put(base / 'swaps', 'Filename Type Size Used Priority\n')
    put(base / 'events', '')
    put(base / 'usb-role/otg_role', 'usb_device\n')
    put(base / 'usb-role/usb_device', '')


def transformed_wrapper(base):
    script = WRAPPER.read_text(encoding='utf-8')
    replacements = {
        '/usr/trimui/apps/usb_storage': unix(base / 'stock'),
        '/tmp/nx-usb-storage-stage.sh': unix(base / 'stage.sh'),
        '/tmp/nx-usb-storage-mode.sh': unix(base / 'stage-mode.sh'),
        '/tmp/nx-usb-lib.sh': unix(base / 'stage-lib.sh'),
        '/tmp/nx-usb-diag.sh': unix(base / 'diag.sh'),
        '/tmp/nx-usb-services.sh': unix(base / 'services.sh'),
        '/tmp/nx-usb-guard.elf': unix(base / 'guard.sh'),
        '/tmp/nx-usb-guard.txt': unix(base / 'guard.log'),
        '/tmp/nx-usb-storage.txt': unix(base / 'storage.log'),
        '/tmp/nx-usb-metrics.txt': unix(base / 'metrics.log'),
        '/tmp/nx-mtp.log': unix(base / 'mode.log'),
        '/tmp/nx-usb-restored-services.txt': unix(base / 'services.log'),
        '/tmp/nx_usb_storage_active': unix(base / 'active'),
        '/tmp/nx_usb_storage_session.lock': unix(base / 'session.lock'),
        '/tmp/nx_usb_storage_remount_blocked': unix(base / 'blocked'),
        '/tmp/stay_awake': unix(base / 'awake'),
        '/proc/mounts': unix(base / 'mounts'),
        '/proc/swaps': unix(base / 'swaps'),
        '/proc/[0-9]*': unix(base / 'proc/[0-9]*'),
        '/sys/devices/platform/soc/usbc0/usb_device': unix(base / 'role-device'),
    }
    for before, after in replacements.items():
        script = script.replace(before, after)
    put(base / 'launch.sh', script)


for scenario in ('busy', 'alias', 'partition2', 'clean', 'adb_before', 'missing',
                 'stop_fail', 'start_fail', 'detach_fail', 'remount_fail',
                 'service_stop_fail', 'service_restore_fail', 'mode_restore_fail', 'cancel_prepare',
                 'link_active'):
    with scratch('nxusb-test-') as base:
        prepare(base)
        if scenario == 'missing':
            put(base / 'mounts', '')
        put(base / 'metrics.log', 'SAMPLE fixture thermal\n')
        put(base / 'system/bin/nx_usb_services.sh', '''#!/bin/sh
# The wrapper starts the stock UI in the background before preparing services.
# Wait for the fake UI's first event so the recorded order is deterministic
# instead of depending on process scheduling.
if [ "$1" = prepare ]; then
 i=0
 while ! /usr/bin/grep -q '^ui-start$' "$FIXTURE/events" && [ "$i" -lt 250 ]; do /usr/bin/sleep 0.02; i=$((i+1)); done
fi
echo "services:$1" >> "$FIXTURE/events"
case "$1" in
 prepare) [ "$SCENARIO" != service_stop_fail ] ;;
 restore)
  [ ! -f "$FIXTURE/exported" ] && [ -s "$FIXTURE/mounts" ] || touch "$FIXTURE/violation"
  [ "$SCENARIO" != service_restore_fail ]
  ;;
esac
''')
        put(base / 'system/bin/nx_usb_mode.sh', '''#!/bin/sh
echo "$1" >> "$FIXTURE/events"
case "$1" in
 status) case "$SCENARIO" in adb_before) echo adb ;; link_active) echo link ;; *) echo mtp ;; esac ;;
 stop) [ "$SCENARIO" != stop_fail ] ;;
 storage-start)
  [ "$SCENARIO" != start_fail ] || exit 1
  if test -s "$FIXTURE/mounts"; then exit 1; fi
  touch "$FIXTURE/exported"
  ;;
 storage-detach)
  if [ "$SCENARIO" = detach_fail ] && [ -f "$FIXTURE/exported" ]; then exit 1; fi
  rm -f "$FIXTURE/exported"
  ;;
 restore)
  [ ! -f "$FIXTURE/exported" ] && [ -s "$FIXTURE/mounts" ] || touch "$FIXTURE/violation"
  [ "$SCENARIO" != mode_restore_fail ]
  ;;
esac
''')
        put(base / 'bin/umount', '''#!/bin/sh
echo unmount >> "$FIXTURE/events"
[ "$SCENARIO" != busy ] || exit 1
case "$SCENARIO" in
 alias) echo "/dev/mmcblk1p1 /alias exfat rw 0 0" > "$FIXTURE/mounts" ;;
 partition2) echo "/dev/mmcblk1p2 /alias exfat rw 0 0" > "$FIXTURE/mounts" ;;
 *) : > "$FIXTURE/mounts" ;;
esac
''')
        put(base / 'bin/mount', '''#!/bin/sh
echo mount >> "$FIXTURE/events"
[ ! -f "$FIXTURE/exported" ] || { touch "$FIXTURE/violation"; exit 1; }
[ "$SCENARIO" != remount_fail ] || exit 1
echo "/dev/mmcblk1p1 $SDCARD_PATH exfat rw 0 0" >> "$FIXTURE/mounts"
''')
        transformed_wrapper(base)
        result = run(base, '"$FIXTURE/launch.sh"', scenario)
        events = (base / 'events').read_text().splitlines()
        details = (scenario, result.returncode, events, result.stderr,
                   (base / 'storage.log').read_text() if (base / 'storage.log').exists() else '')
        assert not (base / 'violation').exists(), details
        successful_entry = scenario in ('clean', 'adb_before', 'detach_fail', 'remount_fail',
                                       'service_restore_fail', 'mode_restore_fail')
        assert ('ui' in events) == successful_entry, details
        if 'ui' in events:
            assert events.index('ui-start') < events.index('services:prepare') < events.index('stop') < events.index('unmount'), details
            assert events.index('unmount') < events.index('storage-start') < events.index('ui'), details
        if scenario in ('detach_fail', 'remount_fail', 'missing'):
            assert (base / 'blocked').exists(), details
            assert 'restore' not in events, details
            if scenario == 'detach_fail':
                assert 'mount' not in events and (base / 'exported').exists(), details
        else:
            assert not (base / 'exported').exists(), details
            assert not (base / 'active').exists(), details
            assert (base / 'sd/.userdata/tg5040/logs/usb-thermal.txt').exists(), details
            if scenario in ('adb_before', 'service_stop_fail', 'cancel_prepare', 'link_active'):
                assert 'restore' not in events, details
            else:
                assert events[-1] == 'restore', details
            if 'mount' in events:
                assert events.index('storage-detach') < events.index('mount'), details
            if 'services:restore' in events:
                assert events.index('storage-detach') < events.index('services:restore'), details
                if 'mount' in events:
                    assert events.index('mount') < events.index('services:restore'), details
        if scenario in ('service_restore_fail', 'mode_restore_fail'):
            assert result.returncode != 0, details
        if scenario == 'link_active':
            # A USB link owns the gadget: refused before the UI, services, MTP stop
            # or unmount are touched, and nothing is restored because nothing changed.
            assert result.returncode != 0, details
            for untouched in ('ui-start', 'services:prepare', 'stop', 'unmount', 'storage-start'):
                assert untouched not in events, details
            assert 'USB link is active' in details[4], details
        print('PASS wrapper', scenario, flush=True)


def transformed_helper(base):
    replacements = {
        '/sys/kernel/config/usb_gadget/g1': unix(base / 'gadget'),
        '/tmp/nx_usb_mode.lock': unix(base / 'mode.lock'),
        '/tmp/nx-mtp.log': unix(base / 'mode.log'),
        '/tmp/nx_usb_storage_active': unix(base / 'active'),
        '/tmp/nx_usb_gamepad_active': unix(base / 'gamepad-active'),
        '/tmp/nx_usb_link_active': unix(base / 'link-active'),
        '/tmp/nx_usb_owner': unix(base / 'owner'),
        '/dev/ffs-umtp': unix(base / 'ffs'),
        '/mnt/SDCARD': unix(base / 'sd'),
        '/proc/mounts': unix(base / 'mounts'),
        '/proc/swaps': unix(base / 'swaps'),
        '/bin/setusbconfig': unix(base / 'setter.sh'),
        '/sys/class/udc': unix(base / 'udc'),
        '/sys/devices/platform/soc/usbc0': unix(base / 'usb-role'),
    }
    for source, target in ((HELPER, 'nx_usb_mode.sh'), (LIB, 'nx_usb_lib.sh')):
        script = source.read_text(encoding='utf-8')
        for before, after in replacements.items():
            script = script.replace(before, after)
        # ONLY the block-node existence probe is mocked; device selection,
        # alias resolution, function/LUN/ADB/bind verification run unchanged.
        script = script.replace('[ -b "$STORAGE_DEVICE" ]', '[ -f "$FIXTURE/block-device-present" ]')
        put(base / 'system/bin' / target, script)


for scenario in ('mtp_missing_sd', 'mtp_marker', 'mtp_open_lun', 'mtp_idempotent', 'mtp_none_status',
                 'stop_clean', 'stop_process_busy', 'stop_ffs_busy', 'stop_setter_fail',
                 'stop_none_bind',
                 'detach_clean', 'detach_udc_fail', 'detach_lun_fail', 'detach_no_storage_bound',
                 'detach_rebind_once', 'detach_rebind_always',
                 'start_clean', 'start_mount', 'start_alias', 'start_partition2',
                 'start_swap', 'start_mtp_busy', 'start_missing_mounts',
                 'start_wrong_backing', 'start_no_bind', 'start_none_bind', 'start_no_adb'):
    with scratch('nxusb-mode-') as base:
        prepare(base)
        put(base / 'block-device-present', '')
        put(base / 'gadget/UDC', 'controller\n')
        put(base / 'ffs/ep1', '')
        put(base / 'system/bin/umtprd', '#!/bin/sh\nexit 1\n')
        put(base / 'system/etc/umtprd.conf', 'fixture\n')
        put(base / 'bin/pidof', '#!/bin/sh\n[ -f "$FIXTURE/process" ]\n')
        put(base / 'bin/killall', '''#!/bin/sh
[ "$SCENARIO" != stop_process_busy ] || exit 1
rm -f "$FIXTURE/process"
''')
        put(base / 'bin/readlink', '''#!/bin/sh
for arg; do last="$arg"; done
case "$last" in
 /dev/by-label/SD) echo /dev/mmcblk1p1 ;;
 /dev/mmcblk*) echo "$last" ;;
 *) /usr/bin/readlink "$@" ;;
esac
''')
        put(base / 'bin/umount', '''#!/bin/sh
[ "$SCENARIO" != stop_ffs_busy ] || exit 1
awk -v root="$FIXTURE/ffs" '$2 != root' "$FIXTURE/mounts" > "$FIXTURE/mounts.next"
mv "$FIXTURE/mounts.next" "$FIXTURE/mounts"
''')
        put(base / 'setter.sh', '''#!/bin/sh
echo "setter:$1" >> "$FIXTURE/events"
[ "$SCENARIO" != stop_setter_fail ] || exit 1
ln -sf "$FIXTURE/gadget/functions/ffs.adb" "$FIXTURE/gadget/configs/c.1/ffs.adb"
echo controller > "$FIXTURE/gadget/UDC"
[ "$SCENARIO" != stop_none_bind ] || echo none > "$FIXTURE/gadget/UDC"
if [ "$1" = mass_storage,adb ]; then
 mkdir -p "$FIXTURE/gadget/functions/mass_storage.usb0/lun.0"
 echo /dev/mmcblk1 > "$FIXTURE/gadget/functions/mass_storage.usb0/lun.0/file"
 ln -sf "$FIXTURE/gadget/functions/mass_storage.usb0" "$FIXTURE/gadget/configs/c.1/f1"
 case "$SCENARIO" in
  start_wrong_backing) echo /dev/mmcblk0 > "$FIXTURE/gadget/functions/mass_storage.usb0/lun.0/file" ;;
  start_no_bind) : > "$FIXTURE/gadget/UDC" ;;
  start_none_bind) echo none > "$FIXTURE/gadget/UDC" ;;
  start_no_adb) rm -f "$FIXTURE/gadget/configs/c.1/ffs.adb" ;;
 esac
fi
''')
        action = 'restore'
        setup = 'true'
        if scenario.startswith('stop_'):
            action = 'stop'
            put(base / 'process', '')
            with (base / 'mounts').open('a') as file:
                file.write('mtp ' + unix(base / 'ffs') + ' functionfs rw 0 0\n')
        elif scenario.startswith('detach_') or scenario == 'mtp_open_lun':
            action = 'storage-detach' if scenario.startswith('detach_') else 'restore'
            if scenario != 'detach_no_storage_bound':
                put(base / 'gadget/functions/mass_storage.usb0/lun.0/file', '/dev/mmcblk1\n')
                setup = 'ln -s "$FIXTURE/gadget/functions/mass_storage.usb0" "$FIXTURE/gadget/configs/c.1/f1"'
            if scenario == 'detach_udc_fail':
                (base / 'gadget/UDC').unlink()
                (base / 'gadget/UDC').mkdir()
            elif scenario == 'detach_lun_fail':
                (base / 'gadget/functions/mass_storage.usb0/lun.0/file').unlink()
                (base / 'gadget/functions/mass_storage.usb0/lun.0/file').mkdir()
            elif scenario.startswith('detach_rebind_'):
                # Models stock adbd rebinding the UDC right after an unbind: the
                # first read after each unbind sees (and restores) "controller".
                # "once" settles after one rebind; "always" never settles.
                put(base / 'bin/cat', '''#!/bin/sh
case "$1" in
 */gadget/UDC)
  value="$(/usr/bin/cat "$1" 2>/dev/null)"
  if [ -z "$value" ]; then
   n="$(/usr/bin/cat "$FIXTURE/rebinds" 2>/dev/null || echo 0)"
   if [ "$SCENARIO" = detach_rebind_always ] || [ "$n" -lt 1 ]; then
    echo $((n + 1)) > "$FIXTURE/rebinds"
    echo controller > "$1"
    echo controller
    exit 0
   fi
  fi
  ;;
esac
exec /usr/bin/cat "$@"
''')
        elif scenario.startswith('start_'):
            action = 'storage-start /dev/mmcblk1p1'
            put(base / 'mounts', '')
            if scenario == 'start_mount':
                put(base / 'mounts', '/dev/mmcblk1p1 ' + unix(base / 'sd') + ' exfat rw 0 0\n')
            elif scenario == 'start_alias':
                put(base / 'mounts', '/dev/by-label/SD /alias exfat rw 0 0\n')
            elif scenario == 'start_partition2':
                put(base / 'mounts', '/dev/mmcblk1p2 /alias ext4 rw 0 0\n')
            elif scenario == 'start_swap':
                put(base / 'swaps', 'Filename Type Size Used Priority\n/dev/mmcblk1p2 partition 1000 0 -2\n')
            elif scenario == 'start_mtp_busy':
                put(base / 'process', '')
            elif scenario == 'start_missing_mounts':
                (base / 'mounts').unlink()
        elif scenario == 'mtp_missing_sd':
            put(base / 'mounts', '')
        elif scenario == 'mtp_marker':
            put(base / 'active', '')
        elif scenario in ('mtp_idempotent', 'mtp_none_status'):
            put(base / 'process', '')
            with (base / 'mounts').open('a') as file:
                file.write('mtp ' + unix(base / 'ffs') + ' functionfs rw 0 0\n')
            setup = 'ln -s "$FIXTURE/gadget/functions/ffs.mtp" "$FIXTURE/gadget/configs/c.1/ffs.mtp"'
            if scenario == 'mtp_none_status':
                action = 'status'
                put(base / 'gadget/UDC', 'none\n')
        transformed_helper(base)
        result = run(base, '"$FIXTURE/system/bin/nx_usb_mode.sh" ' + action, scenario, setup)
        expected_ok = scenario in ('mtp_idempotent', 'mtp_none_status', 'stop_clean', 'detach_clean', 'detach_no_storage_bound',
                                   'detach_rebind_once', 'start_clean')
        details = (scenario, result.returncode, result.stderr,
                   (base / 'mode.log').read_text() if (base / 'mode.log').exists() else '')
        assert (result.returncode == 0) == expected_ok, details
        if scenario == 'mtp_none_status':
            assert result.stdout.strip() == 'adb', details
        if scenario == 'detach_no_storage_bound':
            assert (base / 'gadget/UDC').read_text().strip() == 'controller', details
        events = (base / 'events').read_text()
        if scenario in ('mtp_missing_sd', 'mtp_marker', 'mtp_open_lun', 'mtp_idempotent',
                        'start_mount', 'start_alias', 'start_partition2', 'start_swap'):
            assert not events, details
        if scenario == 'detach_rebind_once':
            # The unbind was repeated after the rebind and the UDC finally stayed unbound.
            assert (base / 'rebinds').read_text().strip() == '1', details
            assert 'repeating unbind' in details[3], details
        if scenario == 'detach_rebind_always':
            # A persistent rebind must keep failing closed: LUN still open, error reported.
            assert 'UDC remained bound' in details[3], details
            assert (base / 'gadget/functions/mass_storage.usb0/lun.0/file').read_text().strip(), details
        if scenario in ('detach_clean', 'detach_rebind_once') or scenario in ('start_wrong_backing', 'start_no_bind', 'start_none_bind', 'start_no_adb'):
            assert not (base / 'gadget/functions/mass_storage.usb0/lun.0/file').read_text().strip(), details
            check = run(base, '-c "test ! -L \\\"$FIXTURE/gadget/configs/c.1/f1\\\""', scenario)
            assert check.returncode == 0, details
        print('PASS coordinator', scenario, flush=True)

# The helper is a door (nx_usb_mode.sh) plus a library (nx_usb_lib.sh). A missing or
# misplaced library must stop everything BEFORE the guard pauses ADB or any gadget
# file is touched.
for scenario in ('lib_missing', 'lib_env_override', 'lib_env_missing', 'lib_status_unguarded'):
    with scratch('nxusb-lib-') as base:
        prepare(base)
        put(base / 'block-device-present', '')
        put(base / 'gadget/UDC', 'controller\n')
        put(base / 'ffs/ep1', '')
        put(base / 'bin/pidof', '#!/bin/sh\n[ -f "$FIXTURE/process" ]\n')
        transformed_helper(base)
        extra = None
        guarded = True
        if scenario == 'lib_missing':
            (base / 'system/bin/nx_usb_lib.sh').unlink()
        elif scenario == 'lib_env_override':
            # The USB Storage wrapper runs a staged copy of the door from /tmp and
            # points NX_USB_LIB at its staged library.
            (base / 'elsewhere').mkdir()
            (base / 'system/bin/nx_usb_lib.sh').rename(base / 'elsewhere/lib.sh')
            extra = {'NX_USB_LIB': unix(base / 'elsewhere/lib.sh')}
        elif scenario == 'lib_env_missing':
            extra = {'NX_USB_LIB': unix(base / 'elsewhere/none.sh')}
        elif scenario == 'lib_status_unguarded':
            guarded = False
        result = run(base, '"$FIXTURE/system/bin/nx_usb_mode.sh" status', scenario,
                     guarded=guarded, extra_env=extra)
        events = (base / 'events').read_text().splitlines()
        details = (scenario, result.returncode, events, result.stdout, result.stderr)
        if scenario in ('lib_missing', 'lib_env_missing'):
            assert result.returncode == 1 and 'Missing USB library' in result.stderr, details
            assert result.stdout.strip() == '' and events == [], details
        else:
            # status needs neither the guard nor a bound function; it prints a mode.
            assert result.returncode == 0 and result.stdout.strip() == 'adb', details
        print('PASS usb lib', scenario, flush=True)

# USB Cable netplay link (usblink): nx_usb_mode.sh owns the gadget while it runs.
# usblink.elf is a fixture that only records its arguments; the mode logic,
# guard gate, markers and MTP stop/restore run unchanged.
LINK_SCENARIOS = (
    'link_start_from_mtp', 'link_start_from_adb', 'link_start_fail_gives_mtp_back',
    'link_start_storage_marker', 'link_start_gamepad_marker', 'link_start_no_usblink',
    'link_start_storage_mode', 'link_start_twice', 'link_start_twice_fail',
    'link_start_guard_missing',
    'link_stop_adb', 'link_stop_mtp', 'link_stop_mtp_fails', 'link_stop_no_marker',
    'link_stop_no_binary', 'link_stop_guard_missing',
    'link_status', 'link_status_none',
    'link_blocks_stop', 'link_blocks_restore', 'link_blocks_storage_start',
    'link_blocks_ok_without_marker',
    'heal_none', 'heal_link', 'heal_owner_only', 'heal_unknown_owner',
    'status_owner_gamepad', 'check_ok', 'check_guard_missing', 'check_guard_abort',
)
for scenario in LINK_SCENARIOS:
    with scratch('nxusb-link-') as base:
        prepare(base)
        put(base / 'block-device-present', '')
        put(base / 'gadget/UDC', 'controller\n')
        # No endpoint file = the MTP responder never became ready, so start_mtp gives
        # up. A responder that merely exits is a race: a zombie child passes kill -0.
        if scenario != 'link_stop_mtp_fails':
            put(base / 'ffs/ep1', '')
        put(base / 'bin/pidof', '#!/bin/sh\n[ -f "$FIXTURE/process" ]\n')
        put(base / 'bin/killall', '#!/bin/sh\nrm -f "$FIXTURE/process"\n')
        put(base / 'bin/umount', """#!/bin/sh
awk -v root="$FIXTURE/ffs" '$2 != root' "$FIXTURE/mounts" > "$FIXTURE/mounts.next"
mv "$FIXTURE/mounts.next" "$FIXTURE/mounts"
""")
        put(base / 'bin/mount', '#!/bin/sh\nexit 0\n')
        put(base / 'bin/readlink', """#!/bin/sh
for arg; do last="$arg"; done
case "$last" in
 /dev/mmcblk*) echo "$last" ;;
 *) /usr/bin/readlink "$@" ;;
esac
""")
        put(base / 'setter.sh', """#!/bin/sh
echo "setter:$1" >> "$FIXTURE/events"
ln -sf "$FIXTURE/gadget/functions/ffs.adb" "$FIXTURE/gadget/configs/c.1/ffs.adb"
echo controller > "$FIXTURE/gadget/UDC"
""")
        # The MTP responder start_mtp launches: alive long enough for its liveness check.
        put(base / 'system/bin/umtprd', '#!/bin/sh\nexec /usr/bin/sleep 2\n')
        put(base / 'system/etc/umtprd.conf', 'fixture\n')
        put(base / 'system/bin/usblink.elf', """#!/bin/sh
echo "usblink:$1" >> "$FIXTURE/events"
if [ "$1" = start ] && { [ "$SCENARIO" = link_start_fail_gives_mtp_back ] || [ "$SCENARIO" = link_start_twice_fail ]; }; then
 exit 1
fi
exit 0
""")
        if scenario in ('link_start_no_usblink', 'link_stop_no_binary'):
            (base / 'system/bin/usblink.elf').unlink()

        def mtp_active():
            put(base / 'process', '')
            with (base / 'mounts').open('a') as file:
                file.write('mtp ' + unix(base / 'ffs') + ' functionfs rw 0 0\n')
            (base / 'gadget/configs/c.1/ffs.mtp').symlink_to(base / 'gadget/functions/ffs.mtp')

        action = 'link-start'
        guarded = True
        extra = None
        if scenario in ('link_start_from_mtp', 'link_start_fail_gives_mtp_back'):
            mtp_active()
        elif scenario == 'link_start_storage_marker':
            put(base / 'active', '')
        elif scenario == 'link_start_gamepad_marker':
            put(base / 'gamepad-active', '')
        elif scenario == 'link_start_storage_mode':
            put(base / 'gadget/functions/mass_storage.usb0/lun.0/file', '/dev/mmcblk1\n')
        elif scenario in ('link_start_twice', 'link_start_twice_fail'):
            put(base / 'link-active', 'mtp\n')
        elif scenario == 'link_start_guard_missing':
            guarded = False
        elif scenario in ('link_stop_adb', 'link_stop_no_binary'):
            action = 'link-stop'
            put(base / 'link-active', 'adb\n')
            put(base / 'owner', 'owner=link prior=adb since=1.00\n')
        elif scenario in ('link_stop_mtp', 'link_stop_mtp_fails'):
            action = 'link-stop'
            put(base / 'link-active', 'mtp\n')
            put(base / 'owner', 'owner=link prior=mtp since=1.00\n')
        elif scenario == 'heal_none':
            action = 'heal'
        elif scenario == 'heal_link':
            action = 'heal'
            put(base / 'link-active', 'mtp\n')
            put(base / 'owner', 'owner=link prior=mtp since=1.00\n')
        elif scenario == 'heal_owner_only':
            action = 'heal'
            put(base / 'owner', 'owner=link prior=adb since=1.00\n')
        elif scenario == 'heal_unknown_owner':
            action = 'heal'
            put(base / 'owner', 'owner=gamepad prior=mtp since=1.00\n')
        elif scenario == 'status_owner_gamepad':
            action = 'status'
            put(base / 'owner', 'owner=gamepad prior=mtp since=1.00\n')
        elif scenario in ('check_ok', 'check_guard_missing', 'check_guard_abort'):
            # check is read-only and runs without the guard gate; it names the guard
            # through NX_USB_GUARD, and an unguarded run proves the gate is skipped.
            action = 'check'
            guarded = False
            if scenario == 'check_ok':
                put(base / 'system/bin/nx_usb_guard.elf', '#!/bin/sh\nexit 0\n')
                extra = {'NX_USB_GUARD': unix(base / 'system/bin/nx_usb_guard.elf')}
            elif scenario == 'check_guard_abort':
                put(base / 'system/bin/nx_usb_guard.elf', '#!/bin/sh\nkill -ABRT $$\n')
                extra = {'NX_USB_GUARD': unix(base / 'system/bin/nx_usb_guard.elf')}
        elif scenario == 'link_stop_no_marker':
            action = 'link-stop'
        elif scenario == 'link_stop_guard_missing':
            action = 'link-stop'
            put(base / 'link-active', 'mtp\n')
            guarded = False
        elif scenario in ('link_status', 'link_status_none'):
            action = 'status'
            if scenario == 'link_status':
                put(base / 'link-active', 'mtp\n')
        elif scenario.startswith('link_blocks_'):
            action = {'link_blocks_stop': 'stop', 'link_blocks_restore': 'restore',
                      'link_blocks_storage_start': 'storage-start /dev/mmcblk1p1',
                      'link_blocks_ok_without_marker': 'stop'}[scenario]
            if scenario == 'link_blocks_ok_without_marker':
                mtp_active()
            else:
                put(base / 'link-active', 'mtp\n')
            if scenario == 'link_blocks_storage_start':
                put(base / 'mounts', '')
        transformed_helper(base)
        result = run(base, '"$FIXTURE/system/bin/nx_usb_mode.sh" ' + action, scenario, guarded=guarded,
                     extra_env=extra)
        events = (base / 'events').read_text().splitlines()
        mode_log = (base / 'mode.log').read_text() if (base / 'mode.log').exists() else ''
        marker = (base / 'link-active').read_text().strip() if (base / 'link-active').exists() else None
        owner = (base / 'owner').read_text().strip() if (base / 'owner').exists() else None
        details = (scenario, result.returncode, events, marker, result.stdout, result.stderr, mode_log, owner)

        ok_scenarios = ('link_start_from_mtp', 'link_start_from_adb', 'link_start_twice',
                        'link_stop_adb', 'link_stop_mtp', 'link_stop_no_marker', 'link_stop_no_binary',
                        'link_status', 'link_status_none', 'link_blocks_ok_without_marker',
                        'heal_none', 'heal_link', 'heal_owner_only', 'status_owner_gamepad', 'check_ok')
        assert (result.returncode == 0) == (scenario in ok_scenarios), details

        if scenario == 'link_start_from_mtp':
            # MTP is stopped (ADB-only gadget, the shape upstream tested) BEFORE the daemon starts.
            assert events == ['setter:adb', 'usblink:start'], details
            assert marker == 'mtp', details
        elif scenario == 'link_start_from_adb':
            assert events == ['usblink:start'] and marker == 'adb', details
        elif scenario == 'link_start_fail_gives_mtp_back':
            # The daemon failed: repaired (usblink stop), MTP restored, no marker left.
            assert events[:3] == ['setter:adb', 'usblink:start', 'usblink:stop'], details
            assert events.count('setter:adb') == 2 and marker is None, details
            assert 'MTP active' in mode_log and 'usblink start failed' in mode_log, details
        elif scenario in ('link_start_storage_marker', 'link_start_gamepad_marker',
                          'link_start_no_usblink', 'link_start_storage_mode', 'link_start_guard_missing'):
            assert events == [] and marker is None, details
            if scenario == 'link_start_guard_missing':
                assert 'Missing USB transaction guard' in result.stderr, details
        elif scenario in ('link_start_twice', 'link_start_twice_fail'):
            # Idempotent: only makes sure the daemon is up, keeps the recorded mode.
            assert events == ['usblink:start'] and marker == 'mtp', details
        elif scenario == 'link_stop_adb':
            assert events == ['usblink:stop'] and marker is None, details
        elif scenario == 'link_stop_no_binary':
            assert events == [] and marker is None, details
        elif scenario == 'link_stop_no_marker':
            assert events == ['usblink:stop'] and marker is None, details
        elif scenario == 'link_stop_mtp':
            assert events[0] == 'usblink:stop' and 'setter:adb' in events and marker is None, details
            assert 'MTP active' in mode_log and 'mode restored: mtp' in mode_log, details
        elif scenario == 'link_stop_mtp_fails':
            # MTP did not come back: the marker stays so status keeps saying "link"
            # and the next link-stop retries instead of reporting a false success.
            assert events[0] == 'usblink:stop' and marker == 'mtp', details
            assert 'MTP restore after the USB link failed' in mode_log, details
        elif scenario == 'link_stop_guard_missing':
            assert events == [] and marker == 'mtp', details
        elif scenario == 'link_status':
            assert result.stdout.strip() == 'link' and events == [], details
        elif scenario == 'link_status_none':
            assert result.stdout.strip() == 'adb' and events == [], details
        elif scenario in ('link_blocks_stop', 'link_blocks_restore', 'link_blocks_storage_start'):
            assert events == [] and marker == 'mtp', details
            assert 'USB link is active' in mode_log, details
        elif scenario == 'link_blocks_ok_without_marker':
            assert 'setter:adb' in events, details
        elif scenario == 'heal_none':
            assert events == [] and owner is None and 'nothing to do' in mode_log, details
        elif scenario == 'heal_link':
            # Same path as link-stop: daemon repaired, MTP back, marker AND record gone.
            assert events[0] == 'usblink:stop' and marker is None and owner is None, details
        elif scenario == 'heal_owner_only':
            assert events == ['usblink:stop'] and owner is None, details
        elif scenario == 'heal_unknown_owner':
            # No heal action for it yet: refuse, leave the record, touch nothing.
            assert events == [] and owner == 'owner=gamepad prior=mtp since=1.00', details
            assert "owner 'gamepad' has no heal action yet" in mode_log, details
        elif scenario == 'status_owner_gamepad':
            assert result.stdout.strip() == 'gamepad' and events == [], details
        elif scenario == 'check_ok':
            assert result.stdout.strip() == 'ok' and result.stderr == '', details
        elif scenario == 'check_guard_missing':
            assert result.returncode == 1 and result.stdout.strip() == 'guard-missing', details
            assert 'Missing USB transaction guard' not in result.stderr, details
        elif scenario == 'check_guard_abort':
            assert result.returncode == 1 and result.stdout.startswith('guard-abort rc='), details
        # The owner record follows the link marker (S1 writes it next to the old marker).
        if scenario == 'link_start_from_mtp':
            assert owner is not None and owner.startswith('owner=link prior=mtp since='), details
        elif scenario == 'link_start_from_adb':
            assert owner is not None and owner.startswith('owner=link prior=adb since='), details
        elif scenario in ('link_start_fail_gives_mtp_back', 'link_start_storage_marker',
                          'link_start_gamepad_marker', 'link_start_no_usblink', 'link_start_storage_mode',
                          'link_stop_adb', 'link_stop_mtp', 'link_stop_no_binary', 'link_stop_no_marker'):
            assert owner is None, details
        elif scenario == 'link_stop_mtp_fails':
            assert owner == 'owner=link prior=mtp since=1.00', details
        print('PASS usb link', scenario, flush=True)


# Services use fake /proc links, stat/start-times, and signals. Never signal
# a host process or execute a device binary from this test.
service_names = ('trimui_osdd', 'trimui_osdd.xbox', 'osdmusic.elf',
                 'musicplayerd.elf', 'audiomon.elf', 'keymon.elf')
for scenario in ('services_clean', 'services_empty', 'services_stop_busy',
                 'services_prepare_no_sd', 'services_existing_state',
                 'services_restore_no_sd', 'services_restore_blocked',
                 'services_restore_active', 'services_restore_missing_binary',
                 'services_restore_idempotent', 'services_osd_sd_map', 'services_osd_sd_fd',
                 'services_external_pause', 'services_pid_reuse'):
    with scratch('nxusb-services-') as base:
        prepare(base)
        for index, name in enumerate(service_names):
            directory = 'osd' if name.startswith('trimui_osdd') else 'system/bin'
            executable = base / directory / name
            put(executable, '#!/bin/sh\n'
                'echo "start:' + name + ':cwd=$PWD" >> "$FIXTURE/events"\n'
                'mkdir -p "$FIXTURE/proc/' + str(200 + index) + '"\n'
                'ln -sf "' + unix(executable) + '" "$FIXTURE/proc/' + str(200 + index) + '/exe"\n')
            put(base / 'proc' / str(200 + index) / 'stat',
                str(200 + index) + ' (' + name + ') S ' + '0 ' * 18 + str(3000 + index) + '\n')
            put(base / 'proc' / str(200 + index) / 'maps', '')
        put(base / 'osd-ram/libbz2.so.1.0', '')
        put(base / 'osd-ram/libosdwait.so', '')
        put(base / 'proc/uptime', '10.0 0.0\n')
        put(base / 'system/bin/musicplayerctl.elf',
            '#!/bin/sh\necho "music:$1" >> "$FIXTURE/events"\n')
        put(base / 'bin/pidof', '''#!/bin/sh
for p in "$FIXTURE"/proc/[0-9]*; do
 [ -L "$p/exe" ] || continue
 t=$(/usr/bin/readlink "$p/exe")
 [ "${t##*/}" != "$1" ] || echo "${p##*/}"
done
''')
        put(base / 'bin/fixture-kill', '''#!/bin/sh
echo "sig:$1:$2" >> "$FIXTURE/events"
case "$1" in
 -STOP|-CONT)
  state=T; [ "$1" != -CONT ] || state=S
  awk -v s="$state" '{ $3=s; print }' "$FIXTURE/proc/$2/stat" > "$FIXTURE/proc/$2/stat.next"
  mv "$FIXTURE/proc/$2/stat.next" "$FIXTURE/proc/$2/stat"
  if [ "$SCENARIO" = services_pid_reuse ] && [ "$1" = -STOP ]; then
   awk '{ $22+=100; print }' "$FIXTURE/proc/$2/stat" > "$FIXTURE/proc/$2/stat.next"
   mv "$FIXTURE/proc/$2/stat.next" "$FIXTURE/proc/$2/stat"
  fi
  exit 0 ;;
esac
[ "$SCENARIO" != services_stop_busy ] || [ "$2" != 205 ] || exit 1
rm -f "$FIXTURE/proc/$2/exe"
''')
        script = SERVICES.read_text()
        for before, after in {
            '/usr/trimui/osd': unix(base / 'osd'),
            '/tmp/nx_osd': unix(base / 'osd-stage'),
            '/tmp/nx-osd-libs': unix(base / 'osd-ram'),
            '/tmp/nx-usb-services.state': unix(base / 'services.state'),
            '/tmp/nx_usb_services.lock': unix(base / 'services.lock'),
            '/tmp/nx-usb-restored-services.txt': unix(base / 'service.log'),
            '/tmp/nx_usb_storage_remount_blocked': unix(base / 'blocked'),
            '/tmp/nx_usb_storage_active': unix(base / 'active'),
            '/tmp/show_osdd': unix(base / 'show'),
            '/tmp/hide_osdd': unix(base / 'hide'),
            '/tmp/trimui_osd/osdd_show_up': unix(base / 'osd-shown'),
            '/proc/mounts': unix(base / 'mounts'),
            'kill -TERM': '"$FIXTURE/bin/fixture-kill" -TERM',
            'kill -STOP': '"$FIXTURE/bin/fixture-kill" -STOP',
            'kill -CONT': '"$FIXTURE/bin/fixture-kill" -CONT',
        }.items():
            script = script.replace(before, after)
        script = script.replace('/proc/', unix(base / 'proc') + '/')
        # Bound a deliberately failed fake TERM to two mock polls. Production
        # has both a wall-clock deadline and a maximum iteration count.
        script = script.replace('"$service_tries" -lt 50', '"$service_tries" -lt 2')
        put(base / 'services.sh', script)
        setup = 'chmod +x "$FIXTURE/osd"/* "$FIXTURE/system/bin"/*; '
        if scenario != 'services_empty':
            for index, name in enumerate(service_names):
                directory = 'osd' if name.startswith('trimui_osdd') else 'system/bin'
                setup += ('mkdir -p "$FIXTURE/proc/' + str(200 + index) + '"; '
                          'ln -s "$FIXTURE/' + directory + '/' + name + '" '
                          '"$FIXTURE/proc/' + str(200 + index) + '/exe"; ')
        setup += 'mkdir -p "$FIXTURE/proc/999"; ln -s /foreign/keymon.elf "$FIXTURE/proc/999/exe"'
        restore_case = scenario.startswith('services_restore_')
        if restore_case or scenario == 'services_existing_state':
            put(base / 'services.state', '\n'.join(service_names) + '\n')
        if scenario in ('services_prepare_no_sd', 'services_restore_no_sd'):
            put(base / 'mounts', '')
        if scenario == 'services_restore_blocked':
            put(base / 'blocked', '')
        if scenario == 'services_restore_active':
            put(base / 'active', '')
        if restore_case and scenario != 'services_restore_idempotent':
            setup += '; rm -f "$FIXTURE"/proc/20*/exe'
        if scenario == 'services_restore_missing_binary':
            (base / 'system/bin/audiomon.elf').unlink()
        if scenario == 'services_osd_sd_map':
            put(base / 'proc/200/maps', 'r-xp ' + unix(base / 'sd/.system/lib/example.so') + '\n')
        if scenario == 'services_osd_sd_fd':
            setup += '; mkdir -p "$FIXTURE/proc/200/fd"; ln -s "$FIXTURE/sd/file" "$FIXTURE/proc/200/fd/9"'
        if scenario == 'services_external_pause':
            put(base / 'proc/200/stat', '200 (trimui_osdd) T ' + '0 ' * 18 + '3000\n')
        action = 'restore' if restore_case else 'prepare'
        result = run(base, '"$FIXTURE/services.sh" ' + action, scenario, setup)
        events = (base / 'events').read_text().splitlines()
        details = (scenario, result.returncode, events, result.stdout, result.stderr)
        expected_ok = scenario in ('services_clean', 'services_empty', 'services_restore_idempotent', 'services_external_pause')
        assert (result.returncode == 0) == expected_ok, details
        assert not any(row.endswith(':999') for row in events), details
        if scenario in ('services_prepare_no_sd', 'services_existing_state',
                        'services_restore_no_sd', 'services_restore_blocked',
                        'services_restore_active', 'services_restore_idempotent'):
            assert not events, details
        if scenario == 'services_clean':
            assert events.index('music:shutdown') < events.index('sig:-TERM:203'), details
            assert 'sig:-STOP:200' in events and 'sig:-TERM:200' not in events, details
        if scenario == 'services_external_pause':
            assert not any(row.endswith(':200') for row in events), details
        if scenario in ('services_osd_sd_map', 'services_osd_sd_fd'):
            assert not events, details
        if scenario in ('services_clean', 'services_empty', 'services_stop_busy'):
            second = run(base, '"$FIXTURE/services.sh" restore', scenario)
            assert second.returncode == 0, (details, second.stdout, second.stderr)
            assert not (base / 'services.state').exists(), details
            starts = (base / 'events').read_text().splitlines()[len(events):]
            assert sum(row.startswith('start:') for row in starts) == (0 if scenario == 'services_empty' else 3 if scenario == 'services_stop_busy' else 4), starts
            assert all(':cwd=/tmp' in row or row.startswith('sig:-CONT:') for row in starts), starts
        elif scenario == 'services_pid_reuse':
            second = run(base, '"$FIXTURE/services.sh" restore', scenario)
            assert second.returncode == 0, (details, second.stdout, second.stderr)
            assert 'sig:-CONT:200' not in (base / 'events').read_text().splitlines(), details
        elif scenario == 'services_restore_missing_binary':
            assert (base / 'services.state').exists(), details
        print('PASS service lifecycle', scenario, flush=True)

# Execute only each real bootstrap prefix, ending before any GPIO, update,
# mount or shutdown command. The prefix must re-exec its copy from RAM.
boot_sources = (
    ('updater', STAGED_DIR / 'updater' if STAGED_DIR else ROOT / 'skeleton/BOOT/common/updater',
     '# A waiting parent shell', '/mnt/SDCARD/.tmp_update/$PLATFORM.sh',
     'PLATFORM=tg5040\n', '/tmp/nx-updater.sh', 'NX_UPDATER_IN_RAM'),
    ('platform', STAGED_DIR / 'tg5040.sh' if STAGED_DIR else ROOT / 'workspace/tg5040/install/boot.sh',
     '#!/bin/sh', 'PLATFORM="tg5040"', '', '/tmp/nx-tg5040-boot.sh', 'NX_TG5040_BOOT_IN_RAM'),
    ('supervisor', STAGED_DIR / 'minui-launch.sh' if STAGED_DIR else ROOT / 'skeleton/SYSTEM/tg5040/paks/MinUI.pak/launch.sh',
     '#!/bin/sh', '# recover from readonly', '', '/tmp/nx-minui-launch.sh', 'NX_MINUI_IN_RAM'),
)
for name, source, begin, end, preamble, ram_file, marker in boot_sources:
    with scratch('nxusb-boot-') as base:
        prepare(base)
        prefix = source.read_text().split(begin, 1)[1].split(end, 1)[0]
        target = unix(base / 'ram-copy.sh')
        script = '#!/bin/sh\n' + preamble + begin + prefix
        script = script.replace(ram_file, target)
        script += 'printf "script=%s cwd=%s staged=%s\\n" "$0" "$PWD" "$' + marker + '"\n'
        put(base / 'boot-probe.sh', script)
        result = run(base, '"$FIXTURE/boot-probe.sh"', 'boot_' + name)
        assert result.returncode == 0 and 'script=' + target in result.stdout, result
        assert 'cwd=/tmp staged=1' in result.stdout, result.stdout
        print('PASS boot RAM re-exec', name, flush=True)
assert '$(dirname "$0")/$PLATFORM' not in (ROOT / 'workspace/tg5040/install/boot.sh').read_text()
assert 'cd "$SYSTEM_PATH/paks/MinUI.pak"' in (ROOT / 'skeleton/SYSTEM/tg5040/paks/MinUI.pak/launch.sh').read_text()

# A logger fixture with a bounded loop and no real /proc or hardware reads.
with scratch('nxusb-diag-') as base:
    prepare(base)
    put(base / 'uptime', '42.0 0.0\n')
    put(base / 'stat', 'cpu 100 0 50 900 20 0 0 0 0 0\n')
    put(base / 'cpu/scaling_cur_freq', '1008000\n')
    put(base / 'thermal/thermal_zone0/type', 'cpu\n')
    put(base / 'thermal/thermal_zone0/temp', '43000\n')
    put(base / 'power/battery/status', 'Charging\n')
    put(base / 'power/battery/current_now', '500000\n')
    put(base / 'udc/controller/state', 'configured\n')
    put(base / 'card-stat', '0 0 0 0 0 0 0 0 0 0 0\n')
    put(base / 'bin/sleep', '''#!/bin/sh
awk '{ $2+=40; $5+=60; print }' "$FIXTURE/stat" > "$FIXTURE/stat.next"
mv "$FIXTURE/stat.next" "$FIXTURE/stat"
''')
    script = DIAG.read_text()
    for before, after in {
        '/tmp/nx_usb_diag.lock': unix(base / 'diag.lock'),
        '/tmp/nx-usb-metrics.txt': unix(base / 'metrics.log'),
        '/tmp/nx-usb-proc-prev.txt': unix(base / 'prev'),
        '/tmp/nx-usb-proc-now.txt': unix(base / 'now'),
        '/sys/kernel/config/usb_gadget/g1': unix(base / 'gadget'),
        '/sys/devices/system/cpu/cpu0/cpufreq': unix(base / 'cpu'),
        '/sys/class/thermal': unix(base / 'thermal'),
        '/sys/class/power_supply': unix(base / 'power'),
        '/sys/class/udc': unix(base / 'udc'),
        '/sys/class/block/mmcblk1/stat': unix(base / 'card-stat'),
        '/proc/uptime': unix(base / 'uptime'),
        '/proc/stat': unix(base / 'stat'),
        '/proc/[0-9]*': unix(base / 'proc/[0-9]*'),
        'MAX_SAMPLES=1800': 'MAX_SAMPLES=6',
    }.items():
        script = script.replace(before, after)
    put(base / 'diag.sh', script)
    result = run(base, '"$FIXTURE/diag.sh"', 'diag')
    log = (base / 'metrics.log').read_text()
    assert result.returncode == 0 and log.count('SAMPLE uptime=') == 6, (result, log)
    assert 'raw=43000' in log and 'current_now=500000' in log and 'END:' in log, log
    assert 'cpu_busy_pct=40' in log, log
    assert not (base / 'diag.lock').exists(), log
    print('PASS diagnostic bounded log + sensor fields', flush=True)

assert '/usr/trimui/apps/usb_storage/launch.sh' not in WRAPPER.read_text()
assert 'fsck.' not in WRAPPER.read_text()
assert 'umount -l' not in WRAPPER.read_text() and 'umount -f' not in WRAPPER.read_text()
print('PASS no stock launcher/automatic repair/lazy or force unmount', flush=True)
