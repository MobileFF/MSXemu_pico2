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
# 1) MSX emulator core + third-party libraries (INTERFACE)
# ============================================================
add_library(msx_lib INTERFACE)

target_sources(msx_lib INTERFACE
    # MSX system
    ${CMAKE_CURRENT_LIST_DIR}/msx_core.c
    ${CMAKE_CURRENT_LIST_DIR}/modmsx.c

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
# ============================================================
add_library(msx_usb_host_core_lib STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../usb_host_core.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/host/usbh.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/host/hub.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/hid/hid_host.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/rp2040/hcd_rp2040.c
)

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
