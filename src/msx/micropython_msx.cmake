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

# 2026-10-11: Pico 2's optional PIO-USB build variant (bldfrm_msx.sh's
# "pico2_piousb" target) — same MICROPY_BOARD ("RPI_PICO2") as the
# default native-USB-host pico2 build, so it can't be distinguished via
# MICROPY_BOARD like MSX_IS_PIZERO above.
#
# Aligned with the sibling PB-1000 emulator project's own established
# native-vs-PIO-USB coexistence convention (per its own session's
# explicit write-up, shared 2026-10-11): a single `option()`
# (USE_PIO_USB, unprefixed — matches that project's own cmake option
# name exactly, for easy cross-project comparison) that bldfrm_msx.sh
# sets via a plain `-DUSE_PIO_USB=ON` CMAKE_ARGS entry (ports/rp2's own
# Makefile already forwards CMAKE_ARGS to its cmake invocation — the
# same mechanism PB-1000 itself uses), rather than a project-specific env
# var as an earlier version of this file did. Internally still tracked
# as MSX_USE_PIO_USB (this project's existing naming convention for its
# own cmake-level board/variant switches, e.g. MSX_IS_PIZERO above) to
# avoid renaming every use site below.
#
# The separate MSX_USE_PIO_USB_HOST compiler macro (-DMSX_USE_PIO_USB_HOST=1,
# set by bldfrm_msx.sh via CFLAGS_EXTRA — see usb_host_core.c/
# modusb_host.c) is DELIBERATELY a different name from this cmake option,
# matching PB-1000's own explicit reasoning for using two different names
# for its cmake option (USE_PIO_USB) vs. its C-side #ifdef macro
# (PB1000_USE_PIO_USB): purely a naming-role distinction (build-selection
# name vs. C-guard name), not a functional one. It still needs the
# CFLAGS_EXTRA route rather than USE_PIO_USB itself, because it gates a
# new MP_QSTR_* registration (usb_host.debug()) that MUST reach
# MicroPython's QSTR-extraction pre-pass — see the MSX_IS_PIZERO comment
# above for why a plain cmake option()/add_compile_definitions() value
# does NOT reach that pass in THIS project's build (a real, previously-
# hit build failure, 2026-09-22) — PB-1000's own project does not have
# this particular constraint, which is why its single option()+macro
# pair is sufficient there but not here for that one QSTR-gated symbol.
option(USE_PIO_USB "Pico 2: use PIO-USB (GP4/GP5) for the keyboard host instead of the native USB host controller, freeing native USB for a CDC REPL" OFF)
set(MSX_USE_PIO_USB ${USE_PIO_USB})

# 2026-10-02: native USB CDC REPL — pico2-only restriction, now scoped
# per board instead of the previous unconditional MICROPY_HW_USB_CDC=0.
# pico2's native RP2350 USB controller is normally dedicated to keyboard
# host mode (usb_host_core.c's usb_host_core_init() -> tuh_init(0) — the
# chip has only one native USB controller, and it can't be host and CDC
# device at the same time), so CDC must stay off there. pizero's keyboard
# instead goes through PIO-USB on a separate virtual port (tuh_init(1),
# bit-banged on GPIO28/29 — see usb_host_core_init_pizero()), leaving
# pizero's native USB controller (the same Type-C port used for
# BOOTSEL/programming) completely unused — safe to enable CDC there.
# Requested explicitly: the keyboard being on a physically separate port
# makes native-USB REPL more convenient than the UART REPL (GP0/1,
# bldfrm_msx.sh's MICROPY_HW_ENABLE_UART_REPL) pizero has used until now.
#
# 2026-10-11: pico2's own optional "pico2_piousb" build variant
# (MSX_USE_PIO_USB, set above) applies this exact same reasoning to pico2
# itself — its keyboard moves to a second, separately-wired PIO-USB port
# (GP4/GP5, usb_host_core_init_pico2_piousb()), freeing the native USB
# controller for CDC the same way pizero's already does.
if(MSX_IS_PIZERO OR MSX_USE_PIO_USB)
    add_compile_definitions(MICROPY_HW_USB_CDC=1)
else()
    add_compile_definitions(MICROPY_HW_USB_CDC=0)
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

# 2026-10-11: pico2's own optional PIO-USB build variant needs
# hardware_pio too (pio_usb.c/pio_usb_host.c, same as pizero's PIO-USB
# use above) but none of libdvi's other dependencies (no DVI/core1 on
# this variant) — linked separately here rather than folding into the
# MSX_IS_PIZERO block above, so a plain pico2 build's link line stays
# byte-for-byte unchanged (this only applies when MSX_USE_PIO_USB is
# actually set — see that variable's own comment near the top of this
# file). hardware_dma/pico_time are already linked to usermod
# unconditionally below (section 3) regardless of board/variant.
if(MSX_USE_PIO_USB)
    target_link_libraries(usermod INTERFACE
        hardware_pio
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
#    Pico 2's USB-host port is, by default, wired to RP2350's native USB
#    host controller (hcd_rp2040.c). The Waveshare RP2350-PiZero's
#    USB-host port is NOT — it's a PIO-USB port (GPIO28/29) — so pizero
#    instead builds sekigon-gonnoc/Pico-PIO-USB (vendored in
#    src/usb_host/pio_usb/, see its PROVENANCE.md) plus this project's
#    own HCD glue for it (src/usb_host/hcd_pio_usb_pizero.c, Phase 4,
#    調査用/RP2350-PiZero_USBキーボード対応調査.md). pico2 ALSO has an
#    optional PIO-USB build variant (MSX_USE_PIO_USB, "pico2_piousb"
#    target — see that same investigation doc's 2026-10-11 addendum) for
#    boards wired with a second USB connector on GP4/GP5, which frees the
#    native controller for a CDC REPL exactly like pizero's does.
#    usb_host_core.c/modusb_host.c (HID report handling, Python bindings)
#    and TinyUSB's own board-agnostic host stack (usbh.c/hub.c/
#    hid_host.c) are the same across all three configurations — only the
#    HCD layer underneath differs.
# ============================================================
add_library(msx_usb_host_core_lib STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../usb_host_core.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/host/usbh.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/host/hub.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/hid/hid_host.c
)

if(MSX_IS_PIZERO OR MSX_USE_PIO_USB)
    target_sources(msx_usb_host_core_lib PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../usb_host/pio_usb/pio_usb.c
        ${CMAKE_CURRENT_LIST_DIR}/../usb_host/pio_usb/pio_usb_host.c
        ${CMAKE_CURRENT_LIST_DIR}/../usb_host/pio_usb/usb_crc.c
        # pio_usb_device.c (upstream's DEVICE-mode implementation)
        # deliberately NOT built — this project only ever uses pio_usb as
        # a HOST (keyboard); nothing in pio_usb.c/pio_usb_host.c/
        # hcd_pio_usb_*.c calls anything from it (confirmed by grep), so
        # it's dead weight (flash + a little .bss) if compiled in.
    )
    if(MSX_IS_PIZERO)
        target_sources(msx_usb_host_core_lib PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/../usb_host/hcd_pio_usb_pizero.c
        )
    else()
        # 2026-10-11: pico2's own optional PIO-USB build variant (see
        # MICROPY_HW_USB_CDC's comment above) — hcd_pio_usb_pico2.c is a
        # deliberate content-duplicate of hcd_pio_usb_pizero.c (the HCD
        # glue layer itself has no board-specific code at all; see that
        # file's own header comment), kept as a separate per-variant file
        # rather than shared so that this addition can never risk
        # pizero's existing, real-hardware-confirmed build.
        target_sources(msx_usb_host_core_lib PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/../usb_host/hcd_pio_usb_pico2.c
        )
    endif()
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

if(MSX_IS_PIZERO OR MSX_USE_PIO_USB)
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
