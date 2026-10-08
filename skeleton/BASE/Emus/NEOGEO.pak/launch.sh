#!/bin/sh

# Accept an untagged Roms/NEOGEO folder without moving games or saves. Use the
# shipped FBN launcher and core; keep this folder's NEOGEO tag for user data.
FBN_LAUNCHER="${SYSTEM_PATH:-$SDCARD_PATH/.system}/paks/Emus/FBN.pak/launch.sh"
if [ ! -f "$FBN_LAUNCHER" ]; then
    echo "Neo Geo: the FBN emulator is missing"
    exit 1
fi
exec "$FBN_LAUNCHER" "$@"
