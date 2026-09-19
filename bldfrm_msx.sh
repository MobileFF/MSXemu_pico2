#!/bin/bash
# MSX1 Emulator firmware build script for Raspberry Pi Pico 2 (RP2350)
#
# Prerequisites (same environment as PB-1000 emulator):
#   MicroPython : ~/projects/micropython   (github.com/micropython/micropython)
#   mpy-cross   : cd ~/projects/micropython && make -C mpy-cross  (first time only)
#
# Usage:
#   chmod +x bldfrm_msx.sh
#   ./bldfrm_msx.sh
#
# Output:
#   ~/projects/micropython/ports/rp2/build-RPI_PICO2/firmware.uf2
#
# To flash: hold BOOTSEL, plug USB, copy firmware.uf2 to RPI-RP2 drive.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Paths ─────────────────────────────────────────────────────────────────────
SRC_ORIG="${SCRIPT_DIR}/src"          # Google Drive source (authoritative)
DST_DIR="${HOME}/projects/msx_emu"    # Local copy (fast I/O for build)
MP_RPI_PORT="${HOME}/projects/micropython/ports/rp2"
BUILD_DIR="${MP_RPI_PORT}/build-RPI_PICO2"
MSX_MODULES="${DST_DIR}/src/msx/micropython_msx.cmake"

echo "=== MSX1 Emulator — Firmware Build ==="
echo "Source  : ${SRC_ORIG}"
echo "Local   : ${DST_DIR}/src"
echo "Modules : ${MSX_MODULES}"
echo "Build   : ${BUILD_DIR}"
echo ""

# ── Validate environment ───────────────────────────────────────────────────────
if [ ! -d "${MP_RPI_PORT}" ]; then
    echo "ERROR: MicroPython RP2 port not found at ${MP_RPI_PORT}"
    echo "  Clone MicroPython: cd ~/projects && git clone https://github.com/micropython/micropython.git"
    echo "  Then: cd micropython && make -C mpy-cross && git submodule update --init"
    exit 1
fi

# ── Copy source to local directory ────────────────────────────────────────────
echo "[copy] Copying source files to ${DST_DIR}/src …"
rm -rf "${DST_DIR}/src"
cp -r "${SRC_ORIG}" "${DST_DIR}/src"
echo "[copy] Done."
echo ""

# ── Clean previous build (forces cmake re-configure with current modules) ─────
echo "[clean] Removing ${BUILD_DIR} …"
rm -rf "${BUILD_DIR}"

# ── Build ─────────────────────────────────────────────────────────────────────
echo "[make] Building firmware (this will run cmake automatically)…"
echo ""

make -C "${MP_RPI_PORT}" \
     BOARD=RPI_PICO2 \
     USER_C_MODULES="${MSX_MODULES}" \
     WERROR=0 \
     MICROPY_C_HEAP_SIZE=65536 \
     -j"$(nproc)" \
     2>&1 | tee /tmp/msx_build.log

# ── Report ────────────────────────────────────────────────────────────────────
UF2="${BUILD_DIR}/firmware.uf2"
OUT_DIR="${SCRIPT_DIR}/firmware"
echo ""
if [ -f "${UF2}" ]; then
    SIZE=$(du -h "${UF2}" | cut -f1)
    mkdir -p "${OUT_DIR}"
    # Named firmware_msx.uf2 (not firmware.uf2) so it isn't confused with
    # UF2s from other Pico2 projects worked on in parallel on this machine.
    cp "${UF2}" "${OUT_DIR}/firmware_msx.uf2"
    echo "=== Build SUCCESS ==="
    echo "Output : ${UF2}  (${SIZE})"
    echo "Copied : ${OUT_DIR}/firmware_msx.uf2"
    echo ""
    echo "Flash  : hold BOOTSEL → plug USB → copy firmware/firmware_msx.uf2 to RPI-RP2 drive"
else
    echo "=== Build FAILED ==="
    echo "Log    : /tmp/msx_build.log"
    echo "Hint   : check for missing #include or undefined symbol above"
    exit 1
fi
