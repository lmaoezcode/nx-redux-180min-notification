#!/bin/sh

# Temporary, bounded USB/heat diagnosis. Run a COPY in /tmp, not this SD file.
# All samples stay in RAM; USB Storage copies them only after a safe remount.
DIAG_LOCK=/tmp/nx_usb_diag.lock
DIAG_LOG=/tmp/nx-usb-metrics.txt
PROC_PREV=/tmp/nx-usb-proc-prev.txt
PROC_NOW=/tmp/nx-usb-proc-now.txt
GADGET=/sys/kernel/config/usb_gadget/g1
CPU_ROOT=/sys/devices/system/cpu/cpu0/cpufreq
MAX_SAMPLES=1800

mkdir "$DIAG_LOCK" 2>/dev/null || exit 0
finish() {
	rm -f "$PROC_PREV" "$PROC_NOW"
	rmdir "$DIAG_LOCK" 2>/dev/null || true
}
trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
exec >"$DIAG_LOG" 2>&1
echo "NX USB diagnostic v1; interval=2s; limit=1800 samples; RAM only"
echo "thermal values: kernel raw (normally millidegrees C); power values: kernel raw"
: > "$PROC_PREV"
prev_total=0
prev_idle=0
sample=0

while [ "$sample" -lt "$MAX_SAMPLES" ]; do
	read -r uptime rest < /proc/uptime
	set -- $(awk '/^cpu / { total=0; for(i=2;i<=9;i++) total+=$i; printf "%.0f %.0f\n", total, $5+$6; exit }' /proc/stat)
	total="${1:-0}"
	idle="${2:-0}"
	busy=NA
	if [ "$prev_total" -gt 0 ] && [ "$total" -gt "$prev_total" ]; then
		busy=$((100 * ((total - prev_total) - (idle - prev_idle)) / (total - prev_total)))
	fi
	prev_total="$total"
	prev_idle="$idle"
	mode=adb
	[ ! -L "$GADGET/configs/c.1/ffs.mtp" ] || mode=mtp
	[ ! -L "$GADGET/configs/c.1/f1" ] || mode=storage
	echo "SAMPLE uptime=$uptime mode=$mode cpu_busy_pct=$busy"
	for field in scaling_cur_freq scaling_min_freq scaling_max_freq scaling_governor; do
		[ ! -r "$CPU_ROOT/$field" ] || echo "cpu $field=$(cat "$CPU_ROOT/$field")"
	done
	for zone in /sys/class/thermal/thermal_zone*; do
		[ -r "$zone/temp" ] || continue
		echo "thermal ${zone##*/} type=$(cat "$zone/type" 2>/dev/null) raw=$(cat "$zone/temp")"
	done
	for supply in /sys/class/power_supply/*; do
		for field in status online capacity current_now voltage_now temp; do
			[ -r "$supply/$field" ] || continue
			echo "power ${supply##*/} $field=$(cat "$supply/$field")"
		done
	done
	for controller in /sys/class/udc/*; do
		[ ! -r "$controller/state" ] || echo "usb ${controller##*/} state=$(cat "$controller/state")"
	done
	[ ! -r /sys/class/block/mmcblk1/stat ] || echo "card_stat $(cat /sys/class/block/mmcblk1/stat)"
	if [ $((sample % 5)) -eq 0 ]; then
		# /proc stat CPU ticks, not ps's misleading lifetime average. starttime
		# prevents a reused PID being compared with a different old process.
		awk '{ rest=$0; sub(/^.*\) /,"",rest); split(rest,a," ");
			name=$0; sub(/^[^(]*\(/,"",name); sub(/\) [A-Za-z] .*/,"",name);
			print $1, a[20], a[12]+a[13], name }' /proc/[0-9]*/stat 2>/dev/null > "$PROC_NOW"
		if [ "$sample" -gt 0 ]; then
			echo "process CPU ticks over ~10s (top 8):"
			awk -v prev="$PROC_PREV" 'FILENAME==prev { start[$1]=$2; ticks[$1]=$3; next }
				start[$1]==$2 && ($3-ticks[$1])>0 { print $3-ticks[$1], "pid=" $1, "name=" $4 }' \
				"$PROC_PREV" "$PROC_NOW" | sort -nr | head -n 8
		fi
		cp -f "$PROC_NOW" "$PROC_PREV"
	fi
	sample=$((sample + 1))
	[ "$sample" -ge "$MAX_SAMPLES" ] || sleep 2
done
echo "END: diagnostic sample limit reached; USB mode was not changed"
