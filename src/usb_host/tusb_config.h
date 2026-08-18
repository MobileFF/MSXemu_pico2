#ifndef _TUSB_HOST_CONFIG_H_
#define _TUSB_HOST_CONFIG_H_

// Fallback definitions for symbols if TinyUSB headers haven't defined them yet
#ifndef OPT_MCU_RP2040
#define OPT_MCU_RP2040 100
#endif
#ifndef OPT_OS_PICO
#define OPT_OS_PICO 200
#endif
#ifndef OPT_MODE_HOST
#define OPT_MODE_HOST 0x01
#endif
#ifndef OPT_MODE_FULL_SPEED
#define OPT_MODE_FULL_SPEED 0x00
#endif

// Guard attribute macros to prevent "expected ';' before void" errors
#ifndef TU_ATTR_FAST_FUNC
#define TU_ATTR_FAST_FUNC
#endif
#ifndef TU_ATTR_SECTION
#define TU_ATTR_SECTION(sec_name)
#endif

// MCU and OS
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

// Use rhport0 as host (Native USB)
#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
#endif

// Debug
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif
#ifndef CFG_TUH_LOG_LEVEL
#define CFG_TUH_LOG_LEVEL 0
#endif

// by default use standard printf for debug output; routing through
// mp_printf causes build errors when this header is included from
// non-MicroPython source files (mp_printf and mp_plat_print are not
// visible there).
#ifndef CFG_TUSB_DEBUG_PRINTF
#define CFG_TUSB_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#endif

// Memory alignment and section (used directly in struct definitions)
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))
#endif
#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif

// DCache
#ifndef CFG_TUSB_MEM_DCACHE_LINE_SIZE
#define CFG_TUSB_MEM_DCACHE_LINE_SIZE 32
#endif
#ifndef CFG_TUH_MEM_DCACHE_ENABLE
#define CFG_TUH_MEM_DCACHE_ENABLE 0
#endif
#ifndef CFG_TUH_MEM_DCACHE_LINE_SIZE
#define CFG_TUH_MEM_DCACHE_LINE_SIZE 32
#endif

// Speed
#ifndef CFG_TUH_MAX_SPEED
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED
#endif
#ifndef TUP_RHPORT_HIGHSPEED
#define TUP_RHPORT_HIGHSPEED 0
#endif
#ifndef TUH_OPT_HIGH_SPEED
#define TUH_OPT_HIGH_SPEED 0
#endif

// Host config
#ifndef CFG_TUH_ENABLED
#define CFG_TUH_ENABLED 1
#endif
#ifndef CFG_TUH_RPI_PIO_USB
#define CFG_TUH_RPI_PIO_USB 0
#endif

// Force HID host config to resolve missing members in hid_host.c
#undef CFG_TUH_HID
#define CFG_TUH_HID 4

#ifndef CFG_TUH_HUB
#define CFG_TUH_HUB 1
#endif
#ifndef CFG_TUH_DEVICE_MAX
#define CFG_TUH_DEVICE_MAX 1
#endif
#ifndef CFG_TUH_ENUMERATION_BUFSIZE
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#endif
#ifndef CFG_TUH_API_EDPT_XFER
#define CFG_TUH_API_EDPT_XFER 0
#endif

#undef CFG_TUH_HID_EP_BUFSIZE
#define CFG_TUH_HID_EP_BUFSIZE 64

#undef CFG_TUH_HID_EPIN_BUFSIZE
#define CFG_TUH_HID_EPIN_BUFSIZE 64

#undef CFG_TUH_HID_EPOUT_BUFSIZE
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

// PIO-USB pin
#ifndef PIO_USB_HOST_DP_PIN
#define PIO_USB_HOST_DP_PIN 2
#endif

// Disable Device mode in this compilation unit
#ifndef CFG_TUD_ENABLED
#define CFG_TUD_ENABLED 0
#endif
#ifndef CFG_TUD_MEM_ALIGN
#define CFG_TUD_MEM_ALIGN __attribute__((aligned(4)))
#endif
#ifndef CFG_TUD_MEM_SECTION
#define CFG_TUD_MEM_SECTION
#endif
#ifndef CFG_TUD_MEM_DCACHE_ENABLE
#define CFG_TUD_MEM_DCACHE_ENABLE 0
#endif
#ifndef CFG_TUD_MEM_DCACHE_LINE_SIZE
#define CFG_TUD_MEM_DCACHE_LINE_SIZE 32
#endif
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif
#ifndef CFG_TUD_ENDPPOINT_MAX
#define CFG_TUD_ENDPPOINT_MAX 0
#endif
#ifndef CFG_TUD_INTERFACE_MAX
#define CFG_TUD_INTERFACE_MAX 0
#endif

#endif
