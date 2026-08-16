#!/usr/bin/env bash
# Build the headless WatchaGotchi UI simulator against the LVGL 8.4 source
# from the SenseCAP firmware repo. Portable (macOS bash 3.2 safe).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
# LVGL 8.4 source: env override, else the SenseCAP BSP checkout (Mac dev
# box), else a plain lvgl v8.4.0 clone (cloud/CI containers).
if [ -z "$LVGL" ]; then
    for cand in "$HOME/esp/SenseCAP-Watcher-Firmware/components/lvgl" \
                /workspace/lvgl/lvgl "$HERE/../../lvgl"; do
        [ -d "$cand/src" ] && LVGL="$cand" && break
    done
fi
[ -d "$LVGL/src" ] || { echo "LVGL not found (set LVGL=/path/to/lvgl-8.4)"; exit 1; }
cd "$HERE"
SRCS=$(find "$LVGL/src" -name '*.c' ! -path '*rlottie*' ! -path '*/gpu/*' ! -path '*sdl*' ! -path '*/libs/ffmpeg/*')
cc -O1 -w -o sim_screens sim_screens.c ../main/tama_ui.c ../main/tama_logic.c ../main/tama_sprites.c $SRCS \
    -I"$HERE" -I"$LVGL" -I"$LVGL/src" -I"$HERE/../main" \
    -DLV_CONF_INCLUDE_SIMPLE -lm
echo "built ./sim_screens"
