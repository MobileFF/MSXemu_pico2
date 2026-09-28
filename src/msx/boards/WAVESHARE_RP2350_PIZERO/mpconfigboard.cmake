# CMake file for Waveshare RP2350-PiZero
#
# 2026-09-20, phase 2 of the RP2350-PiZero port (調査用/RP2350-PiZero_HDMI
# 出力適用調査.md §7). Modeled on MicroPython's stock WAVESHARE_RP2350B_CORE
# board (~/projects/micropython/ports/rp2/boards/WAVESHARE_RP2350B_CORE/) --
# same RP2350B chip, same "no official pico-sdk support, supply our own
# board header" pattern. This directory is passed to the rp2 port build via
# BOARD_DIR=<...>/src/msx/boards/WAVESHARE_RP2350_PIZERO (see bldfrm_msx.sh),
# not copied into the MicroPython checkout itself, so it stays version-
# controlled with the rest of this project like micropython_msx.cmake does.
#
# Board facts confirmed against the vendor schematic + a working reference
# port (github.com/ugufru/xroar-waveshare-rp2350-pizero, README "Target
# hardware" section, 2026-09 snapshot): RP2350B, 48 GPIO, 520KB SRAM, 16MB
# onboard QSPI flash, PSRAM pad present on the PCB but NOT populated on
# stock boards -- treated as SRAM-only, unlike WAVESHARE_RP2350B_CORE which
# does populate/enable PSRAM.

set(PICO_NUM_GPIOS 48)

# No official pico-sdk board support for this board -- add our own header
# search path and PICO_BOARD name so pico-sdk looks for
# waveshare_rp2350_pizero.h in this same directory.
list(APPEND PICO_BOARD_HEADER_DIRS ${MICROPY_BOARD_DIR})
set(PICO_BOARD "waveshare_rp2350_pizero")

if(NOT DEFINED MICROPY_HW_FLASH_STORAGE_BYTES)
    set(MICROPY_HW_FLASH_STORAGE_BYTES 14680064)  # 14 * 1024 * 1024 (16MB flash, same split as WAVESHARE_RP2350B_CORE)
endif()
