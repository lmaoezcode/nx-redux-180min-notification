#!/bin/sh
# Harmless prerequisite for H2: prove the native launcher created a detached
# session with standard descriptors redirected before any USB state is changed.

trap '' HUP

LOG=/tmp/nx-detached-smoke.log
: > "$LOG"
printf 'pid=%s\n' "$$" >> "$LOG"
printf 'stat=' >> "$LOG"
cat "/proc/$$/stat" >> "$LOG" 2>/dev/null
for smoke_fd in 0 1 2; do
	printf 'fd%s=%s\n' "$smoke_fd" "$(readlink "/proc/$$/fd/$smoke_fd" 2>/dev/null)" >> "$LOG"
done
: > /tmp/nx-detached-smoke.started
sleep 3
printf 'completed=yes\n' >> "$LOG"
: > /tmp/nx-detached-smoke.completed
