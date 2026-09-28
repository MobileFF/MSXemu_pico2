#ifndef USB_HOST_CORE_H
#define USB_HOST_CORE_H

#include <stdbool.h>
#include <stdint.h>


void usb_host_core_init(void);
void usb_host_core_task(void);
void usb_host_core_start_bg_timer(int interval_ms);
void usb_host_core_stop_bg_timer(void);

/* Returns pointer to the last received 8-byte HID keyboard report.
 * Layout: [modifier, reserved, keycode×6]  (USB HID boot protocol) */
const uint8_t *usb_host_core_get_hid_report(void);

#ifdef MSX_BOARD_PIZERO
/* Real-hardware bring-up diagnostic (Phase 4, PIO-USB) — see
 * usb_host_core.c's own comment. Exposed via modusb_host.c as
 * usb_host.debug() -> (tick_count, connected, suspended, ints). */
void usb_host_core_debug_pizero(uint32_t *tick_count, uint32_t *connected,
                                 uint32_t *suspended, uint32_t *ints);
#endif

#endif
