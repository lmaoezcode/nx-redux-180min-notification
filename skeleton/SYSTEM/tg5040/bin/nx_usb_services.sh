#!/bin/sh

# Quiesce only known NX services. This script is copied to RAM by USB Storage.
# Never kill arbitrary SD holders: a remaining holder must make umount fail.
ACTION="${1:-}"
SD_ROOT="${SDCARD_PATH:-/mnt/SDCARD}"
SYSTEM_ROOT="${SYSTEM_PATH:-$SD_ROOT/.system}"
OSD_ROOT=/usr/trimui/osd
OSD_RAM_LIB=/tmp/nx-osd-libs
STATE=/tmp/nx-usb-services.state
LOCK=/tmp/nx_usb_services.lock
SERVICE_LOG=/tmp/nx-usb-restored-services.txt

mounted() {
	awk -v root="$SD_ROOT" '$2 == root { found = 1 } END { exit !found }' /proc/mounts
}

pids() {
	# pidof is one native scan. Do NOT fork readlink for every kernel task on
	# every poll (the old supposed five-second wait took over a minute).
	for service_pid in $(pidof "$1" 2>/dev/null); do
		service_exe="$(readlink "/proc/$service_pid/exe" 2>/dev/null)"
		case "$1:$service_exe" in
			keymon.elf:"$SYSTEM_ROOT/bin/keymon.elf"|\
			audiomon.elf:"$SYSTEM_ROOT/bin/audiomon.elf"|\
			musicplayerd.elf:"$SYSTEM_ROOT/bin/musicplayerd.elf"|\
			osdmusic.elf:"$SYSTEM_ROOT/bin/osdmusic.elf"|\
			trimui_osdd:"$OSD_ROOT/trimui_osdd"|trimui_osdd:/tmp/nx_osd/trimui_osdd|\
			trimui_osdd.xbox:"$OSD_ROOT/trimui_osdd.xbox"|trimui_osdd.xbox:/tmp/nx_osd/trimui_osdd.xbox)
				printf '%s\n' "$service_pid" ;;
		esac
	done
}

process_info() {
	IFS= read -r service_stat < "/proc/$1/stat" || return 1
	service_rest="${service_stat##*) }"
	[ "$service_rest" != "$service_stat" ] || return 1
	set -- $service_rest
	[ "$#" -ge 20 ] || return 1
	service_state="$1"
	shift 19
	printf '%s %s\n' "$1" "$service_state"
}

same_process() {
	case " $(pids "$1" | tr '\n' ' ') " in *" $2 "*) ;; *) return 1 ;; esac
	service_now="$(process_info "$2")" || return 1
	[ "${service_now%% *}" = "$3" ]
}

clock_seconds() {
	read -r service_uptime service_idle < /proc/uptime || return 1
	printf '%s\n' "${service_uptime%%.*}"
}

wait_stopped() {
	service_deadline=$(($(clock_seconds) + 5))
	service_tries=0
	while same_process "$1" "$2" "$3" && [ "$service_tries" -lt 50 ] &&
		[ "$(clock_seconds)" -lt "$service_deadline" ]; do
		service_tries=$((service_tries + 1))
		usleep 100000 2>/dev/null || sleep 1
	done
	! same_process "$1" "$2" "$3"
}

wait_paused() {
	service_deadline=$(($(clock_seconds) + 2))
	service_tries=0
	while same_process "$1" "$2" "$3" && [ "$service_tries" -lt 20 ] &&
		[ "$(clock_seconds)" -lt "$service_deadline" ]; do
		service_now="$(process_info "$2")" || return 1
		case "${service_now#* }" in T|t) return 0 ;; esac
		service_tries=$((service_tries + 1))
		usleep 100000 2>/dev/null || sleep 1
	done
	return 1
}

osd_has_no_sd_holders() {
	[ -r "$OSD_RAM_LIB/libbz2.so.1.0" ] && [ -r "$OSD_RAM_LIB/libosdwait.so" ] || return 1
	[ -r "/proc/$1/maps" ] || return 1
	awk -v root="$SD_ROOT/" 'index($0, root) { found=1 } END { exit found }' "/proc/$1/maps" || return 1
	for service_fd in "/proc/$1/cwd" "/proc/$1/exe" "/proc/$1"/fd/*; do
		service_target="$(readlink "$service_fd" 2>/dev/null)"
		case "$service_target" in "$SD_ROOT"|"$SD_ROOT"/*) return 1 ;; esac
	done
}

prepare() {
	mounted || return 1
	[ ! -e "$STATE" ] || { echo "Service recovery state already exists; refusing another session"; return 1; }
	# Snapshot every owned service before stopping any; partial failures can
	# restore those already stopped without duplicating a still-live service.
	: > "$STATE" || return 1
	for service in trimui_osdd trimui_osdd.xbox osdmusic.elf musicplayerd.elf audiomon.elf keymon.elf; do
		for service_pid in $(pids "$service"); do
			service_info="$(process_info "$service_pid")" || return 1
			service_start="${service_info%% *}"
			service_original_state="${service_info#* }"
			service_action=TERM
			case "$service" in trimui_osdd*) service_action=STOP ;; esac
			case "$service_original_state" in T|t) service_action=KEEP ;; esac
			printf '%s %s %s %s\n' "$service" "$service_pid" "$service_start" "$service_action" >> "$STATE"
		done
	done
	rm -f /tmp/show_osdd
	touch /tmp/hide_osdd
	while read -r service service_pid service_start service_action; do
		[ "$service_action" != KEEP ] || continue
		same_process "$service" "$service_pid" "$service_start" || continue
		case "$service" in
			trimui_osdd*)
				osd_has_no_sd_holders "$service_pid" || {
					echo "OSD still uses SD libraries/files; reboot with the RAM-library launcher required"
					return 1
				}
				# Stock OSD catches SIGTERM without exiting. Freeze a RAM-only,
				# hidden instance instead; keep its exact identity for SIGCONT.
				service_hide_deadline=$(($(clock_seconds) + 2))
				while [ -e /tmp/trimui_osd/osdd_show_up ] && [ "$(clock_seconds)" -lt "$service_hide_deadline" ]; do
					usleep 100000 2>/dev/null || sleep 1
				done
				[ ! -e /tmp/trimui_osd/osdd_show_up ] || { echo "OSD could not hide"; return 1; }
				same_process "$service" "$service_pid" "$service_start" && kill -STOP "$service_pid" || return 1
				wait_paused "$service" "$service_pid" "$service_start" || return 1
				echo "Paused RAM-only OSD: $service pid=$service_pid"
				continue
				;;
		esac
		echo "Stopping owned service: $service pid=$service_pid"
		if [ "$service" = musicplayerd.elf ] && [ -x "$SYSTEM_ROOT/bin/musicplayerctl.elf" ]; then
			# This command asks the owner to persist playback state and close files.
			"$SYSTEM_ROOT/bin/musicplayerctl.elf" shutdown </dev/null >> "$SERVICE_LOG" 2>&1 || true
		fi
		same_process "$service" "$service_pid" "$service_start" && kill -TERM "$service_pid" 2>/dev/null || true
		wait_stopped "$service" "$service_pid" "$service_start" || { echo "Service did not stop: $service (no SIGKILL)"; return 1; }
	done < "$STATE"
	sync
	echo "Known NX services stopped; real unmount is still mandatory"
}

start_one() {
	[ -z "$(pids "$1")" ] || return 0
	case "$1" in
		keymon.elf|audiomon.elf|musicplayerd.elf|osdmusic.elf)
			[ -x "$SYSTEM_ROOT/bin/$1" ] || return 1
			(cd /tmp && exec "$SYSTEM_ROOT/bin/$1") </dev/null >> "$SERVICE_LOG" 2>&1 &
			;;
		trimui_osdd|trimui_osdd.xbox)
			[ -x "$OSD_ROOT/$1" ] || return 1
			if [ -r "$OSD_RAM_LIB/libosdwait.so" ]; then
				(cd "$OSD_ROOT" && LD_LIBRARY_PATH="$OSD_RAM_LIB:$LD_LIBRARY_PATH" LD_PRELOAD="$OSD_RAM_LIB/libosdwait.so" exec "./$1") </dev/null >> "$SERVICE_LOG" 2>&1 &
			else
				(cd "$OSD_ROOT" && exec "./$1") </dev/null >> "$SERVICE_LOG" 2>&1 &
			fi
			;;
		*) echo "Unknown service in recovery state; ignoring: $1"; return 1 ;;
	esac
	service_deadline=$(($(clock_seconds) + 3))
	service_tries=0
	while [ -z "$(pids "$1")" ] && [ "$service_tries" -lt 30 ] && [ "$(clock_seconds)" -lt "$service_deadline" ]; do
		service_tries=$((service_tries + 1))
		usleep 100000 2>/dev/null || sleep 1
	done
	[ -n "$(pids "$1")" ]
}

restore() {
	[ -f "$STATE" ] || return 0
	mounted && [ ! -e /tmp/nx_usb_storage_remount_blocked ] &&
		[ ! -e /tmp/nx_usb_storage_active ] || { echo "Service restore blocked: SD unavailable or USB Storage active"; return 1; }
	service_result=0
	# Start the audio owner before the renderer/OSD; the snapshot is not eval'd.
	for service in keymon.elf audiomon.elf musicplayerd.elf osdmusic.elf trimui_osdd trimui_osdd.xbox; do
		while read -r saved_service saved_pid saved_start saved_action; do
			[ "$saved_service" = "$service" ] || continue
			case "$saved_action" in
				KEEP) continue ;;
				STOP)
					if same_process "$service" "$saved_pid" "$saved_start"; then
						kill -CONT "$saved_pid" || service_result=1
						echo "Resumed original OSD: $service pid=$saved_pid"
						continue
					fi
					;;
			esac
			start_one "$service" || { echo "Service restore failed: $service"; service_result=1; }
		done < "$STATE"
	done
	[ "$service_result" -eq 0 ] || return 1
	rm -f "$STATE"
	echo "Previously running NX services restored"
}

mkdir "$LOCK" 2>/dev/null || exit 1
trap 'rmdir "$LOCK" 2>/dev/null' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
case "$ACTION" in
	prepare) prepare ;;
	restore) restore ;;
	*) echo "usage: $0 {prepare|restore}" >&2; exit 2 ;;
esac
