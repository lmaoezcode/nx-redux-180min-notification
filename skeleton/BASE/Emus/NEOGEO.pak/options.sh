#!/bin/sh
# NAME: Neo Geo (fbneo)

PAK_DIR="$(dirname "$0")"
EMU_TAG=NEOGEO
CORE="${SYSTEM_PATH:-$SDCARD_PATH/.system}/paks/Emus/FBN.pak/fbneo_libretro.so"
CONFIG_DIR="$USERDATA_PATH/$EMU_TAG-fbneo"
CACHE="$CONFIG_DIR/options.json"
BIOS_DIR="$SDCARD_PATH/Bios/$EMU_TAG"
mkdir -p "$CONFIG_DIR"

if [ ! -f "$CACHE" ] || [ "$CORE" -nt "$CACHE" ] || { [ -d "$BIOS_DIR" ] && [ "$BIOS_DIR" -nt "$CACHE" ]; }; then
    minarch.elf --dump-options "$CORE" "$CACHE" "$BIOS_DIR" >> "$LOGS_PATH/emu-options.txt" 2>&1
fi

SYS_CFG="$SYSTEM_PATH/system.cfg"
[ -n "${DEVICE:-}" ] && [ -f "$SYSTEM_PATH/system-$DEVICE.cfg" ] && SYS_CFG="$SYSTEM_PATH/system-$DEVICE.cfg"
DEF_CFG="$PAK_DIR/default.cfg"

if [ -n "${1:-}" ]; then
    ROM_DIR="$(dirname "$1")"
    ROM_DIR_NAME="${ROM_DIR##*/}"
    ALT="${1##*/}"
    [ ! -f "$ROM_DIR/$ROM_DIR_NAME.m3u" ] || ALT="$ROM_DIR_NAME.m3u"
    options.elf --json "$CACHE" \
        --minarch-system "$SYS_CFG" --minarch-default "$DEF_CFG" \
        --minarch-dir "$CONFIG_DIR" --minarch-game "$ALT" \
        --game "$ALT" 2>> "$LOGS_PATH/emu-options.txt"
else
    options.elf --json "$CACHE" \
        --minarch-system "$SYS_CFG" --minarch-default "$DEF_CFG" \
        --minarch-dir "$CONFIG_DIR" \
        --game "Neo Geo (fbneo)" 2>> "$LOGS_PATH/emu-options.txt"
fi
