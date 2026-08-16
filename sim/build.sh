#!/usr/bin/env bash
# Build the headless WatchaGotchi UI simulator against the LVGL 8.4 source
# from the SenseCAP firmware repo. Portable (macOS bash 3.2 safe).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
LVGL="${LVGL:-$HOME/esp/SenseCAP-Watcher-Firmware/components/lvgl}"
[ -d "$LVGL/src" ] || { echo "LVGL not found at $LVGL (set LVGL=...)"; exit 1; }
cd "$HERE"
SRCS=$(find "$LVGL/src" -name '*.c' ! -path '*rlottie*' ! -path '*/gpu/*' ! -path '*sdl*' ! -path '*/libs/ffmpeg/*')
cc -O1 -w -o sim_screens sim_screens.c ../main/tama_ui.c ../main/tama_sprites.c $SRCS \
    -I"$HERE" -I"$LVGL" -I"$LVGL/src" -I"$HERE/../main" \
    -DLV_CONF_INCLUDE_SIMPLE -lm
echo "built ./sim_screens"
