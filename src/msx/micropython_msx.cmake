# micropython_msx.cmake
#
# MSX1 emulator — MicroPython C module build configuration
#
# Usage (from MicroPython ports/rp2 build):
#   cmake .. -DUSER_C_MODULES=<path>/src/msx/micropython_msx.cmake
#
# This replaces micropython.cmake (the PB-1000 build).
# The USB host and LCD controller from the PB-1000 project are reused.

add_compile_options(-Wno-error)
add_compile_definitions(CFG_TUH_HID_EP_BUFSIZE=64)
add_definitions(-DPICO_MALLOC_PANIC=0)
add_compile_definitions(MICROPY_HW_USB_CDC=0)
add_compile_definitions(MICROPY_HW_USB_MSC=0)

# ============================================================
# 0) Board switch — Waveshare RP2350-PiZero (onboard DVI) vs the default
#    Pico 2 (SPI LCD). 2026-09-20, phase 2/3 of the RP2350-PiZero port
#    (調査用/RP2350-PiZero_HDMI出力適用調査.md §7).
#
#    2026-09-22 BUG FIX: this used to check a CMake variable named
#    `BOARD`, which does not exist in this scope — MicroPython's rp2
#    CMakeLists.txt calls it `MICROPY_BOARD` (set from the `BOARD=`
#    make argument bldfrm_msx.sh passes). The `BOARD`-based check was
#    always false, so MSX_IS_PIZERO/MSX_BOARD_PIZERO/
#    PICO_PIO_USE_GPIO_BASE and the whole libdvi/disp_dvi.c block below
#    were NEVER actually applied to any pizero build so far, including
#    ones that appeared to build and boot successfully — msx.get_board_
#    type() was silently always compiling to return "pico2", which
#    real-hardware testing eventually caught (board_config.py picking
#    the wrong branch, LCD init attempted on a board with no LCD).
#    ${MICROPY_BOARD} is already defined by the time MicroPython's own
#    CMakeLists.txt includes this file (via USER_C_MODULES).
# ============================================================
set(MSX_IS_PIZERO OFF)
if(MICROPY_BOARD STREQUAL "WAVESHARE_RP2350_PIZERO")
    set(MSX_IS_PIZERO ON)
    # MSX_BOARD_PIZERO / PICO_PIO_USE_GPIO_BASE are NOT defined here via
    # add_compile_definitions() — MicroPython's QSTR-extraction pre-pass
    # doesn't see definitions added from this file (runs too early in
    # rp2/CMakeLists.txt, before USER_C_MODULES is included) and silently
    # fails to pick up new MP_QSTR_* names guarded by them (real build
    # failure, 2026-09-22). bldfrm_msx.sh passes both via CFLAGS_EXTRA
    # instead, which reaches every compilation unit including that pass.
endif()

# ============================================================
# 1) MSX emulator core + third-party libraries (INTERFACE)
# ============================================================
add_library(msx_lib INTERFACE)

target_sources(msx_lib INTERFACE
    # MSX system
    ${CMAKE_CURRENT_LIST_DIR}/msx_core.c
    ${CMAKE_CURRENT_LIST_DIR}/modmsx.c
    ${CMAKE_CURRENT_LIST_DIR}/wd179x.c

    # Z80 CPU core (superzazu/z80, MIT)
    ${CMAKE_CURRENT_LIST_DIR}/z80/z80.c

    # TMS9918A VDP (visrealm/vrEmuTms9918, MIT)
    ${CMAKE_CURRENT_LIST_DIR}/tms9918/vrEmuTms9918.c

    # AY-3-8910 PSG (digital-sound-antiques/emu2149, MIT)
    ${CMAKE_CURRENT_LIST_DIR}/emu2149/emu2149.c

    # USB host (reused from PB-1000 project)
    ${CMAKE_CURRENT_LIST_DIR}/../modusb_host.c
)

target_include_directories(msx_lib INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..
)

# ============================================================
# 1b) Onboard DVI (Waveshare RP2350-PiZero only) — vendored libdvi (see
#     src/msx/libdvi/PROVENANCE.md) + this project's own driver
#     (src/msx/display/disp_dvi.c). Not part of msx_lib's unconditional
#     source list above since a Pico 2 build must never try to compile
#     PIO/DVI code for hardware it doesn't have.
# ============================================================
if(MSX_IS_PIZERO)
    target_sources(msx_lib INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/libdvi/dvi.c
        ${CMAKE_CURRENT_LIST_DIR}/libdvi/dvi_serialiser.c
        ${CMAKE_CURRENT_LIST_DIR}/libdvi/dvi_timing.c
        ${CMAKE_CURRENT_LIST_DIR}/libdvi/tmds_encode.c
        ${CMAKE_CURRENT_LIST_DIR}/libdvi/tmds_encode.S
        ${CMAKE_CURRENT_LIST_DIR}/display/disp_dvi.c
    )
    target_include_directories(msx_lib INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/libdvi
        ${CMAKE_CURRENT_LIST_DIR}/display
    )
    # dvi_serialiser.c needs dvi_serialiser.pio.h, normally produced from
    # dvi_serialiser.pio by pico_generate_pio_header() at build time — but
    # MicroPython's separate QSTR-extraction preprocessing pass (part of
    # its frozen-module build step) runs a raw preprocessor invocation
    # over every USER_C_MODULES source file BEFORE that custom command's
    # dependency graph guarantees the header has been generated, so
    # dvi_serialiser.c's #include fails with "No such file or directory"
    # during that pass specifically (real build failure hit 2026-09-22).
    # Worked around by pre-generating the header once with pioasm (the
    # .pio source rarely changes) and committing it as a plain vendored
    # file instead of a build-time generated one — see libdvi/
    # dvi_serialiser.pio.h's own header comment ("autogenerated by
    # pioasm... do not edit"). Regenerate with:
    #   <pico-sdk build>/pioasm-install/pioasm/pioasm -o c-sdk \
    #     libdvi/dvi_serialiser.pio libdvi/dvi_serialiser.pio.h
    # if dvi_serialiser.pio is ever changed.

    # libdvi's own dependencies beyond what msx_lib/usermod already link
    # (hardware_dma, below) — matches its upstream CMakeLists.txt.
    target_link_libraries(usermod INTERFACE
        hardware_pio
        hardware_interp
        hardware_pwm
        hardware_vreg
        pico_multicore
        pico_util
    )
endif()

# vrEmuTms9918 needs to know it is being linked statically
target_compile_definitions(msx_lib INTERFACE
    VR_EMU_TMS9918_STATIC
)

# vrEmuTms9918.c gates its RAM-resident (__time_critical_func) hot-path
# placement behind PICO_BUILD, which was never defined for this project —
# that optimization was silently inert. Enable it here.
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/tms9918/vrEmuTms9918.c
    PROPERTIES COMPILE_DEFINITIONS PICO_BUILD=1
)

# The overall MicroPython build defaults to CMAKE_BUILD_TYPE=MinSizeRel
# (-Os), which is right for flash-constrained MicroPython core code but
# leaves real performance on the table for our hot emulation loop. Force
# speed optimization for just these four files (Z80 core, VDP scanline
# render, PSG, and the frame loop that drives them) without bloating the
# rest of the firmware image.
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/msx_core.c
    ${CMAKE_CURRENT_LIST_DIR}/z80/z80.c
    ${CMAKE_CURRENT_LIST_DIR}/tms9918/vrEmuTms9918.c
    ${CMAKE_CURRENT_LIST_DIR}/emu2149/emu2149.c
    PROPERTIES COMPILE_OPTIONS "-O3"
)

# ============================================================
# 2) USB host core (STATIC, isolated from MicroPython)
#
#    Pico 2's USB-host port is wired to RP2350's native USB host
#    controller (hcd_rp2040.c). The Waveshare RP2350-PiZero's USB-host
#    port is NOT — it's a PIO-USB port (GPIO28/29) — so pizero instead
#    builds sekigon-gonnoc/Pico-PIO-USB (vendored in src/usb_host/pio_usb/,
#    see its PROVENANCE.md) plus this project's own HCD glue for it
#    (src/usb_host/hcd_pio_usb_pizero.c, Phase 4,
#    調査用/RP2350-PiZero_USBキーボード対応調査.md). usb_host_core.c/
#    modusb_host.c (HID report handling, Python bindings) and TinyUSB's
#    own board-agnostic host stack (usbh.c/hub.c/hid_host.c) are the same
#    for both boards — only the HCD layer underneath differs.
# ============================================================
add_library(msx_usb_host_core_lib STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../usb_host_core.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/host/usbh.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/host/hub.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/hid/hid_host.c
)

if(MSX_IS_PIZERO)
    target_sources(msx_usb_host_core_lib PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../usb_host/hcd_pio_usb_pizero.c
        ${CMAKE_CURRENT_LIST_DIR}/../usb_host/pio_usb/pio_usb.c
        ${CMAKE_CURRENT_LIST_DIR}/../usb_host/pio_usb/pio_usb_host.c
        ${CMAKE_CURRENT_LIST_DIR}/../usb_host/pio_usb/usb_crc.c
        # pio_usb_device.c (upstream's DEVICE-mode implementation)
        # deliberately NOT built — this project only ever uses pio_usb as
        # a HOST (keyboard); nothing in pio_usb.c/pio_usb_host.c/
        # hcd_pio_usb_pizero.c calls anything from it (confirmed by grep),
        # so it's dead weight (flash + a little .bss) if compiled in.
    )
    target_include_directories(msx_usb_host_core_lib PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../usb_host/pio_usb
    )
    # usb_tx.pio.h/usb_rx.pio.h are upstream's own pre-generated (pioasm)
    # headers, vendored as plain static files — same
    # pico_generate_pio_header()-at-QSTR-prescan-time problem as
    # dvi_serialiser.pio.h (see micropython_msx.cmake's libdvi comment and
    # src/usb_host/pio_usb/PROVENANCE.md), same fix (don't generate, just
    # include the committed header).
else()
    target_sources(msx_usb_host_core_lib PRIVATE
        ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/rp2040/hcd_rp2040.c
    )
endif()

target_link_libraries(msx_usb_host_core_lib PRIVATE
    pico_stdlib
    pico_rand
)

target_include_directories(msx_usb_host_core_lib PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/..
    ${CMAKE_CURRENT_LIST_DIR}/../usb_host
    ${PICO_SDK_PATH}/lib/tinyusb/src
    ${PICO_SDK_PATH}/lib/tinyusb/src/common
    ${PICO_SDK_PATH}/lib/tinyusb/hw
    ${PICO_SDK_PATH}/src/rp2_common/hardware_flash/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_base/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_pio/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_dma/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_spi/include
    ${CMAKE_SOURCE_DIR}/../..
    ${CMAKE_SOURCE_DIR}/boards/RPI_PICO2
    ${CMAKE_SOURCE_DIR}/boards/${BOARD}
    ${CMAKE_SOURCE_DIR}/boards/${PICO_BOARD}
    ${CMAKE_SOURCE_DIR}/build-RPI_PICO2
    ${CMAKE_SOURCE_DIR}
)

target_compile_options(msx_usb_host_core_lib PRIVATE
    "-include${CMAKE_CURRENT_LIST_DIR}/../usb_host/malloc_override.h"
    "-include${CMAKE_CURRENT_LIST_DIR}/../usb_host/tusb_config.h"
)

target_compile_definitions(msx_usb_host_core_lib PRIVATE
    malloc=tu_malloc
    free=tu_free
    calloc=tu_calloc
    realloc=tu_realloc
    PICO_MALLOC_PANIC=0
    CFG_TUH_HID_EP_BUFSIZE=64
    CFG_TUH_HID=4
    CFG_TUH_HID_EPIN_BUFSIZE=64
    CFG_TUH_HID_EPOUT_BUFSIZE=64
)

if(MSX_IS_PIZERO)
    # Not routed through CFLAGS_EXTRA/bldfrm_msx.sh like MSX_BOARD_PIZERO
    # (DVI) had to be — that workaround was only needed because MicroPython's
    # QSTR-extraction pre-pass doesn't see add_compile_definitions() from
    # this file, and only matters for defines that gate new MP_QSTR_*
    # identifiers. msx_usb_host_core_lib is a separate STATIC library, not
    # part of USER_C_MODULES' QSTR-scanned source list, and this project's
    # usb_host Python API surface (modusb_host.c) is identical on both
    # boards — no new QSTR symbols depend on this flag.
    target_compile_definitions(msx_usb_host_core_lib PRIVATE
        CFG_TUH_RPI_PIO_USB=1
    )
endif()

# ============================================================
# 3) Link everything into the MicroPython usermod
# ============================================================
target_link_libraries(usermod INTERFACE
    msx_lib
    msx_usb_host_core_lib
    hardware_dma
    hardware_pwm
    hardware_clocks
    pico_time
)
