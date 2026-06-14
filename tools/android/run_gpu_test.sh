#!/bin/sh
# tools/android/run_gpu_test.sh — GPU-1 on-device cert loop (the S25 is the
# test rig; CI has no real GPU — design doc §4, salty-bug loop).
#
# Builds the standalone arm64 gpu_test executable via the Android CMake build
# (so it cross-compiles for the SAME target as the .so), pushes it to
# /data/local/tmp on the connected device, and runs it on the real Adreno.
#
#   ./tools/android/run_gpu_test.sh
#
# Expects: a device on adb (`adb devices` shows it), the Android SDK at
# /root/android-sdk (or ANDROID_HOME). The gpu_test binary needs no extra
# libs at runtime: it dlopen()s libvulkan.so itself (no DT_NEEDED), so it
# runs on devices WITHOUT Vulkan too (it just reports gpu_available=0).
#
# What to read in the output:
#   gpu_available = 1            -> the S25's Adreno/Xclipse Vulkan is usable
#   [gpu-matmul-matches-cpu] PASS for every size, with the MEASURED max_rel
#                                -> GPU numerics agree with the CPU reference
#                                   (report the max_rel numbers; we tighten TOL
#                                    to the real rounding floor from this data)
#   [gpu-faster-on-big] cpu=.. gpu=.. ms per size
#                                -> small sizes: GPU SLOWER (expected; launch
#                                   overhead). big sizes: GPU FASTER. The size
#                                   where gpu_ms crosses below cpu_ms = T_GPU.
#
# REPORT BACK: the gpu_available line, every max_rel, and the cpu/gpu ms table
# (so the commander can set T_GPU + tighten TOL).
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
ANDROID_DIR="$ROOT/android"
ABI=arm64-v8a

ADB="${ADB:-adb}"
DEST=/data/local/tmp/gpu_test

echo "[1/4] building the Android project (compiles gpu_test for $ABI) ..."
( cd "$ANDROID_DIR" && ./gradlew :app:assembleDebug --offline )

echo "[2/4] locating the cross-compiled gpu_test binary ..."
BIN=$(find "$ANDROID_DIR/app/build" -name gpu_test -type f -path "*$ABI*" | head -1)
[ -n "$BIN" ] || { echo "gpu_test not found — did the CMake build run?"; exit 1; }
echo "      $BIN"

echo "[3/4] pushing to the device ($DEST) ..."
"$ADB" push "$BIN" "$DEST"
"$ADB" shell chmod 755 "$DEST"

echo "[4/4] running on the device's real GPU ..."
echo "--------------------------------------------------------------------"
"$ADB" shell "$DEST"
echo "--------------------------------------------------------------------"
echo "Done. Report gpu_available, the max_rel values, and the cpu/gpu ms table."
