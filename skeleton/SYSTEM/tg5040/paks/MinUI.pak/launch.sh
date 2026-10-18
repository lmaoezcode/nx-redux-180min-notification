#!/bin/sh
# MinUI.pak

# A shell executing this file from SD pins the filesystem even with cwd=/tmp.
# Re-exec a RAM copy before starting any long-lived child processes.
if [ "${NX_MINUI_IN_RAM:-0}" != 1 ]; then
	if cp -f "$0" /tmp/nx-minui-launch.sh; then
		export NX_MINUI_IN_RAM=1
		cd /tmp || exit 1
		exec /bin/sh /tmp/nx-minui-launch.sh "$@"
	fi
	echo "Could not stage supervisor; USB Storage may remain blocked"
fi

# recover from readonly SD card -------------------------------
# touch /mnt/writetest
# sync
# if [ -f /mnt/writetest ] ; then
# 	rm -f /mnt/writetest
# else
# 	e2fsck -p /dev/root > /mnt/SDCARD/RootRecovery.txt
# 	reboot
# fi

export PLATFORM="tg5040"
export SDCARD_PATH="/mnt/SDCARD"
export BIOS_PATH="$SDCARD_PATH/Bios"
export ROMS_PATH="$SDCARD_PATH/Roms"
export SAVES_PATH="$SDCARD_PATH/Saves"
export CHEATS_PATH="$SDCARD_PATH/Cheats"
export SYSTEM_PATH="$SDCARD_PATH/.system"
export CORES_PATH="$SYSTEM_PATH/cores"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export DATETIME_PATH="$SHARED_USERDATA_PATH/datetime.txt"
export SHARED_SYSTEM_PATH="$SDCARD_PATH/.system/shared"
export HOME="$USERDATA_PATH"

# Keep kernel messages off the 115200-baud serial console (dmesg still records them).
# At loglevel 8 the xradio Wi-Fi join burst stalls the Brick Pro's I2C stick polling,
# and the stock sunxi_i2c timeout path then panics (NextUI#824).
echo "0 7 1 7" > /proc/sys/kernel/printk 2>/dev/null

#######################################

if [ -f "/tmp/poweroff" ]; then
	poweroff_next
	exit 0
fi
if [ -f "/tmp/reboot" ]; then
	reboot_next
	exit 0
fi

#######################################

mkdir -p "$BIOS_PATH"
mkdir -p "$ROMS_PATH"
mkdir -p "$SAVES_PATH"
mkdir -p "$CHEATS_PATH"
mkdir -p "$USERDATA_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"

export TRIMUI_MODEL=`strings /usr/trimui/bin/MainUI | grep ^Trimui`
if [ "$TRIMUI_MODEL" = "Trimui Brick" ]; then
	export DEVICE="brick"
elif [ "$TRIMUI_MODEL" = "Trimui Brick Pro" ]; then
	export DEVICE="brickpro"
else
	export DEVICE="smartpro"
fi

export IS_NEXT="yes"

#######################################

##Remove Old Led Daemon
if [ -f "/etc/LedControl" ]; then
	rm -Rf "/etc/LedControl" 2> /dev/null
fi
if [ -f "/etc/init.d/lcservice" ]; then
	/etc/init.d/lcservice disable
	rm /etc/init.d/lcservice 2> /dev/null
fi

# clear shadercache unconditionally, until it properly invalidates itself
rm -rf $SDCARD_PATH/.shadercache

#PD11 pull high for VCC-5v
echo 107 > /sys/class/gpio/export
echo -n out > /sys/class/gpio/gpio107/direction
echo -n 1 > /sys/class/gpio/gpio107/value

#rumble motor PH3
echo 227 > /sys/class/gpio/export
echo -n out > /sys/class/gpio/gpio227/direction
echo -n 0 > /sys/class/gpio/gpio227/value

if [ "$TRIMUI_MODEL" = "Trimui Smart Pro" ]; then
	#Left/Right Pad PD14/PD18
	echo 110 > /sys/class/gpio/export
	echo -n out > /sys/class/gpio/gpio110/direction
	echo -n 1 > /sys/class/gpio/gpio110/value

	echo 114 > /sys/class/gpio/export
	echo -n out > /sys/class/gpio/gpio114/direction
	echo -n 1 > /sys/class/gpio/gpio114/value
fi

#DIP Switch PH19
echo 243 > /sys/class/gpio/export
echo -n in > /sys/class/gpio/gpio243/direction

syslogd -S

#######################################

export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:$SHARED_SYSTEM_PATH/lib:/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=$SYSTEM_PATH/bin:$SHARED_SYSTEM_PATH/bin:$PATH


# leds_off
echo 0 > /sys/class/led_anim/max_scale
if [ "$DEVICE" = "brick" ] || [ "$DEVICE" = "brickpro" ]; then
	echo 0 > /sys/class/led_anim/max_scale_lr
	echo 0 > /sys/class/led_anim/max_scale_f1f2
fi
if [ "$DEVICE" = "brickpro" ]; then
	# Brick Pro's fifth zone (triggers)
	echo 0 > /sys/class/led_anim/max_scale_rear
fi

# start the device's own stock gpio input daemon, by absolute path: Smart Pro
# and Brick wire their buttons differently, so a single vendored inputd can't
# serve both (v1.2.0 shipped one and broke all Smart Pro buttons). The tg5040
# OSD trigger is keymon's MENU long-press, which the stock inputd fully covers.
/usr/trimui/bin/trimui_inputd &

echo schedutil > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo 408000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo 1008000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq

keymon.elf & # &> $SDCARD_PATH/keymon.txt &

# Make the SD card available over MTP as soon as NX Redux starts. This is
# intentionally automatic: connecting the bottom USB-C port needs no app.
nx_usb_mode.sh restore > /dev/null 2>&1 &

# USB/heat diagnostic runs from RAM, with a bounded one-hour log. It must not
# become another SD script descriptor that prevents USB Storage unmounting.
if [ -r "$SYSTEM_PATH/bin/nx_usb_diag.sh" ]; then
	cp -f "$SYSTEM_PATH/bin/nx_usb_diag.sh" /tmp/nx-usb-diag.sh
	/bin/sh /tmp/nx-usb-diag.sh > /dev/null 2>&1 &
fi

# Overlay-mount the SD card's OSD tree onto /usr/trimui/osd — the SD card is
# the source of truth for the OSD (daemon binary, assets, toast scripts,
# every widget). trimui_osdd has /usr/trimui/osd hardcoded in the
# closed-source binary, so it can't read from the SD card directly. tg5050
# overlay-mounts the SD tree itself as the lower layer, but tg5040's kernel
# 4.9 overlayfs rejects the exFAT SD card outright ("filesystem on '...' not
# supported" — its exfat driver's dentry revalidation isn't supported by
# overlayfs; hardware-verified on Brick 2026-07-26). So here the assembled
# tree (~1MB, no font) is staged into tmpfs first and the overlay is built
# from that staging copy instead of straight off the SD card. The copy lands
# in RAM, never on the rootfs, so rootfs writes stay at zero; SD edits take
# effect next boot via re-staging. Stock-only files the SD tree doesn't ship
# (regular.ttf, the 16MB CJK font) show through from the rootfs layer
# underneath.

# /etc writes below (bt/wifi init scripts) still need a writable rootfs
mount -o remount,rw /

OSD_DST="/usr/trimui/osd"
OSD_SRC="$SYSTEM_PATH/osd"
# The daemon binary and its resolution-locked assets (bg.png, block*.png:
# 1024x768/124px grid on Brick vs 1280x720/120px grid on Smart Pro) are
# model-specific and firmware builds are NOT interchangeable, so they live in
# an osd-$DEVICE layer stacked above the shared tree. No layer shipped for
# this model -> leave the firmware's OSD alone: the shared widgets alone have
# no daemon, no bg.png and no osdlayout.json, which would leave the OSD dead
# instead of merely un-themed. On staging or mount failure the daemon below
# still starts from whatever the rootfs holds; the marker records the
# failure for diagnosis.
OSD_SRC_MODEL="$SYSTEM_PATH/osd-$DEVICE"
if [ -d "$OSD_SRC_MODEL" ] && ! grep -q " $OSD_DST " /proc/mounts; then
	OSD_STAGE="/tmp/nx_osd"
	rm -rf "$OSD_STAGE"
	mkdir -p "$OSD_STAGE"
	if cp -r "$OSD_SRC/." "$OSD_STAGE/" && cp -r "$OSD_SRC_MODEL/." "$OSD_STAGE/"; then
		chmod +x "$OSD_STAGE/trimui_osdd" "$OSD_STAGE/trimui_osdd.xbox" "$OSD_STAGE"/*.sh \
			"$OSD_STAGE"/widgets/*/*.sh 2> /dev/null
		# Recolour the daemon's active slider / toast frame images in the
		# staged copy to the theme's primary accent (no-op for the default
		# theme; the white focus ring is left as shipped). Must run before
		# trimui_osdd loads its images.
		"$SYSTEM_PATH/bin/osdmusic.elf" --tint-osd "$OSD_STAGE" "$OSD_STAGE" 2> /dev/null
		mount -t overlay overlay \
			-o ro,lowerdir="$OSD_STAGE:$OSD_DST" "$OSD_DST" \
			|| touch /tmp/nx_osd_mount_failed
	else
		touch /tmp/nx_osd_mount_failed
	fi
fi # end osd overlay mount

# Start OSD overlay daemon (system-wide quick menu). Settings > System >
# Button layout = Xbox needs the daemon's OK/cancel buttons swapped; that is
# a separately patched copy (scripts/patch-osdd-layout.sh), picked here at
# boot — the setting applies after a restart, like everywhere else.
OSDD=trimui_osdd
if [ "$(nextval.elf buttonlayout | sed -n 's/.*"buttonlayout": \([0-9]*\).*/\1/p')" = "1" ] \
	&& [ -x "$OSD_DST/trimui_osdd.xbox" ]; then
	OSDD=trimui_osdd.xbox
fi
if [ -x "$OSD_DST/$OSDD" ]; then
	# libosdwait.so (workspace/all/osdwait) interposes the daemon's SDL_WaitEvent:
	# with no SDL window to block on, SDL 2.30 polls every 1 ms (~920 wakeups/s,
	# ~4.5% of a core while the panel is hidden and idle); the shim polls every
	# 20 ms while hidden and 1 ms while shown. Bare start if the library is absent.
	# OSD catches TERM rather than exiting. Its two SD-mapped libraries must
	# live in RAM so USB Storage can safely pause the same hidden instance.
	OSD_RAM_LIB=/tmp/nx-osd-libs
	mkdir -p "$OSD_RAM_LIB"
	if cp -f "$SYSTEM_PATH/lib/libbz2.so.1.0" "$OSD_RAM_LIB/libbz2.so.1.0" &&
		cp -f "$SYSTEM_PATH/lib/libosdwait.so" "$OSD_RAM_LIB/libosdwait.so"; then
		OSDWAIT="$OSD_RAM_LIB/libosdwait.so"
		OSD_LIB_PATH="$OSD_RAM_LIB:$LD_LIBRARY_PATH"
	else
		OSDWAIT="$SYSTEM_PATH/lib/libosdwait.so"
		OSD_LIB_PATH="$LD_LIBRARY_PATH"
		echo "OSD RAM library staging failed; USB Storage must refuse entry"
	fi
	if [ -f "$OSDWAIT" ]; then
		cd "$OSD_DST" && LD_LIBRARY_PATH="$OSD_LIB_PATH" LD_PRELOAD="$OSDWAIT" ./$OSDD &
	else
		cd "$OSD_DST" && ./$OSDD &
	fi
fi
cd /tmp || exit 1

# Ensure .asoundrc is clean at boot — /etc/asound.conf handles speaker routing.
# audiomon will write .asoundrc when USB/BT devices connect.
rm -f $USERDATA_PATH/.asoundrc /tmp/nx_audio_sink
audiomon.elf & # &> $SDCARD_PATH/audiomon.txt &
# Debug logging (Developer setting). App/game logs are redirected by every
# pak's launch.sh through $LOGS_PATH. When the setting is off, point that at
# tmpfs (wiped before every launch) so nothing lands on the SD card; when on,
# keep the persistent .userdata/<plat>/logs directory. Re-evaluated before
# each launch so toggling it in Settings applies without a reboot.
nx_update_logs_path() {
	dbglog=$(nextval.elf debugLogging | sed -n 's/.*"debugLogging": \([0-9]*\).*/\1/p')
	if [ "$dbglog" = "1" ]; then
		export NX_DEBUG_LOGGING=1
		export LOGS_PATH="$USERDATA_PATH/logs"
		rm -rf /tmp/nx-logs
	else
		export NX_DEBUG_LOGGING=0
		export LOGS_PATH="/tmp/nx-logs"
		rm -rf "$LOGS_PATH"
	fi
	mkdir -p "$LOGS_PATH"
}
nx_update_logs_path
# musicplayerd runs for the whole session, so its log cannot live in the
# per-launch tmpfs dir (the wipe would unlink its open file); when debug
# logging is off it goes to /dev/null instead.
MPD_LOG=/dev/null
[ "$NX_DEBUG_LOGGING" = "1" ] && MPD_LOG="$LOGS_PATH/music-playerd.txt"
musicplayerd.elf </dev/null >> "$MPD_LOG" 2>&1 &

# Boot-time radio/ssh init below is backgrounded; the launcher runs the CPU at
# full range until all of it has exited, polling this marker (15 s fallback —
# see workspace/all/nextui/cpu_policy.h). /tmp is tmpfs, so the marker never
# outlives a boot; the rm is belt and braces. A relaunch after a game or tool
# finds the marker present and caps at its first frame as before.
BOOT_DONE_MARKER=/tmp/nx_boot_done
rm -f "$BOOT_DONE_MARKER"
BOOT_JOBS=""

# BT handling
# NOTE: On tg5040 (xradio combo chip), running bluetoothd+hciattach degrades
# WiFi throughput from ~330 KB/s to ~2 KB/s due to coexistence interference.
# Only start BT when the user has it enabled.
bluetoothon=$(nextval.elf bluetooth | sed -n 's/.*"bluetooth": \([0-9]*\).*/\1/p')
# somehow trimui deploys aic?
cp -f $SYSTEM_PATH/etc/bluetooth/bt_init.sh /etc/bluetooth/bt_init.sh
if [ "$bluetoothon" -eq 1 ]; then
	/etc/bluetooth/bt_init.sh start > /dev/null 2>&1 &
else
	/etc/bluetooth/bt_init.sh stop > /dev/null 2>&1 &
fi
BOOT_JOBS="$BOOT_JOBS $!"

# wifi handling
# on by default, disable based on systemval setting
wifion=$(nextval.elf wifi | sed -n 's/.*"wifi": \([0-9]*\).*/\1/p')
cp -f $SYSTEM_PATH/etc/wifi/wifi_init.sh /etc/wifi/wifi_init.sh
if [ "$wifion" -eq 0 ]; then
	/etc/wifi/wifi_init.sh stop > /dev/null 2>&1 &
else 
	/etc/wifi/wifi_init.sh start > /dev/null 2>&1 &
fi
BOOT_JOBS="$BOOT_JOBS $!"

# SSH handling - developer setting
sshonboot=$(nextval.elf sshOnBoot | sed -n 's/.*"sshOnBoot": \([0-9]*\).*/\1/p')
if [ "$sshonboot" -eq 1 ]; then
	/etc/init.d/sshd start > /dev/null 2>&1 &
	BOOT_JOBS="$BOOT_JOBS $!"
fi

# Touch the marker once every boot job above has exited (kill -0 = alive).
(
	for p in $BOOT_JOBS; do
		while kill -0 $p 2>/dev/null; do sleep 0.2; done
	done
	touch "$BOOT_DONE_MARKER"
) > /dev/null 2>&1 &

#######################################

AUTO_PATH=$USERDATA_PATH/auto.sh
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH"
fi

cd "$SYSTEM_PATH/paks/MinUI.pak" || exit 1

#######################################

# kill show2.elf if running
killall -9 show2.elf > /dev/null 2>&1

EXEC_PATH="/tmp/nextui_exec"
NEXT_PATH="/tmp/next"
# The per-game helper executes from RAM with cwd in RAM. It closes every SD
# descriptor after each atomic checkpoint and is stopped before returning to
# nextui, so it cannot pin the card during a later USB Storage unmount.
NX_PLAYTIME_DAEMON=""
if [ -r "$SYSTEM_PATH/bin/playtimed.elf" ]; then
	if cp "$SYSTEM_PATH/bin/playtimed.elf" /tmp/nx-playtimed.new && \
		chmod +x /tmp/nx-playtimed.new && mv -f /tmp/nx-playtimed.new /tmp/nx-playtimed.elf; then
		NX_PLAYTIME_DAEMON=/tmp/nx-playtimed.elf
	fi
fi

nx_playtime_start() {
	unset NX_PLAYTIME_PID
	[ -n "$NX_PLAYTIME_DAEMON" ] || return 0
	"$NX_PLAYTIME_DAEMON" --is-game "$1" || return 0
	rm -f /tmp/nx-playtime.pid
	(cd /tmp && exec "$NX_PLAYTIME_DAEMON" --run "$$") </dev/null >/tmp/nx-playtime.log 2>&1 &
	NX_PLAYTIME_CHILD=$!
	# Never send pause signals or suppress minarch's fallback before the helper
	# installs handlers, loads the day's ledger and publishes readiness.
	nx_playtime_tries=0
	while [ "$nx_playtime_tries" -lt 50 ]; do
		if [ "$(cat /tmp/nx-playtime.pid 2>/dev/null)" = "$NX_PLAYTIME_CHILD" ]; then
			export NX_PLAYTIME_PID="$NX_PLAYTIME_CHILD"
			return 0
		fi
		kill -0 "$NX_PLAYTIME_CHILD" 2>/dev/null || break
		sleep 0.02
		nx_playtime_tries=$((nx_playtime_tries + 1))
	done
	# A helper failure must never prevent the actual game from launching.
	kill -TERM "$NX_PLAYTIME_CHILD" 2>/dev/null || true
	wait "$NX_PLAYTIME_CHILD" 2>/dev/null || true
	NX_PLAYTIME_CHILD=""
	return 0
}

nx_playtime_stop() {
	if [ -n "$NX_PLAYTIME_CHILD" ]; then
		kill -TERM "$NX_PLAYTIME_CHILD" 2>/dev/null || true
		wait "$NX_PLAYTIME_CHILD" 2>/dev/null || true
	fi
	NX_PLAYTIME_CHILD=""
	unset NX_PLAYTIME_PID
}

touch "$EXEC_PATH"  && sync
while [ -f $EXEC_PATH ]; do
	nx_update_logs_path
	nextui.elf &> "$LOGS_PATH/nextui.txt"

	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		nx_update_logs_path
		# Preserve the user's USB data mode across tools that temporarily take
		# ownership of the gadget. Ordinary games do not touch the gadget and
		# must not tear down/re-enumerate MTP every time they exit.
		USB_MODE_BEFORE=$(nx_usb_mode.sh status 2>/dev/null)
		# A USB Cable link left behind reports "link"; the mode it will hand back
		# (written by nx_usb_mode.sh link-start) is the one to keep.
		[ "$USB_MODE_BEFORE" != "link" ] || USB_MODE_BEFORE=$(cat /tmp/nx_usb_link_active 2>/dev/null)
		[ "$USB_MODE_BEFORE" = "mtp" ] || USB_MODE_BEFORE=adb
		# The launcher may be sitting at its 600 MHz idle cap (nextui/
		# cpu_policy.h); hand every pak the boot-default range instead, the
		# same state the restore block below leaves. Paks own their clocks
		# from here (minarch, DC/N64/NDS/PSP, PortMaster all set their own).
		echo schedutil > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
		echo 408000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq 2>/dev/null
		echo 1008000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null
		# The child cannot change its parent's cwd. Move the supervisor out of
		# the SD filesystem before USB Storage tries a real (non-lazy) unmount.
		PAK_RETURN_DIR=""
		case "$CMD" in
			*"USB Storage.pak"*) PAK_RETURN_DIR="$PWD"; cd /tmp || exit 1 ;;
		esac
		nx_playtime_start "$NEXT_PATH"
		eval "$CMD"
		nx_playtime_stop
		# If USB could not release the block device, do not launch SD programs
		# or restore MTP behind the safety guard. Logs and recovery stay in RAM.
		if [ -f /tmp/nx_usb_storage_remount_blocked ]; then
			echo "USB card recovery is blocked; SD programs will not be launched"
			while [ -f /tmp/nx_usb_storage_remount_blocked ]; do sleep 2; done
		fi
		[ -z "$PAK_RETURN_DIR" ] || cd "$PAK_RETURN_DIR"
		rm -f $NEXT_PATH
		# USB Storage now owns its complete restore transaction and retries.
		# Only Gamepad needs this supervisor fallback; ordinary games leave USB
		# alone and must not trigger re-enumeration.
		case "$CMD" in
			*"USB Gamepad.pak"*)
				if [ "$USB_MODE_BEFORE" = "mtp" ]; then
					nx_usb_mode.sh restore > /dev/null 2>&1 &
				else
					nx_usb_mode.sh stop > /dev/null 2>&1 &
				fi
				;;
		esac
		# Restore CPU state (games/tools may change governor and freq)
		echo schedutil > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
		echo 408000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq 2>/dev/null
		echo 1008000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null
	fi

	if [ -f "/tmp/poweroff" ]; then
		musicplayerctl.elf shutdown >/dev/null 2>&1 || true
		poweroff_next
		exit 0
	fi
	if [ -f "/tmp/reboot" ]; then
		musicplayerctl.elf shutdown >/dev/null 2>&1 || true
		reboot_next
		exit 0
	fi
done

musicplayerctl.elf shutdown >/dev/null 2>&1 || true
poweroff_next # just in case
