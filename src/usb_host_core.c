#include "usb_host_core.h"
#include "py/runtime.h"
#include "py/mpprint.h"
#include "py/gc.h"
#include "tusb.h"
#include "host/hcd.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include <string.h>

/* Raw 8-byte HID keyboard report, updated on every received report.
 * Exposed via usb_host_core_get_hid_report() for Python polling. */
static uint8_t last_hid_report[8] = {0};

/* Background timer for tuh_task() */
static struct repeating_timer usb_bg_timer;
static bool usb_bg_timer_active = false;

static bool usb_bg_timer_callback(struct repeating_timer *t) {
  (void)t;
  tuh_task();
  return true; /* keep repeating */
}

// TinyUSB debug string helpers
char const *const tu_str_speed[] = {"Full", "Low", "High", "Unknown"};
char const *const tu_str_xfer_result[] = {"OK", "FAIL", "STALL", "ERROR"};
char const *const tu_str_std_request[] = {
    "GET_STATUS",        "CLEAR_FEATURE",  "Reserved",
    "SET_FEATURE",       "Reserved",       "SET_ADDRESS",
    "GET_DESCRIPTOR",    "SET_DESCRIPTOR", "GET_CONFIGURATION",
    "SET_CONFIGURATION", "GET_INTERFACE",  "SET_INTERFACE",
    "SYNCH_FRAME"};

void tu_print_mem(void const *buf, uint32_t count, uint8_t indent) {
  uint8_t const *p = (uint8_t const *)buf;
  for (uint32_t i = 0; i < count; i++) {
    if (i % 16 == 0) {
      if (i > 0) mp_printf(&mp_plat_print, "\n");
      for (uint8_t j = 0; j < indent; j++) mp_printf(&mp_plat_print, " ");
    }
    mp_printf(&mp_plat_print, "%02X ", p[i]);
  }
  mp_printf(&mp_plat_print, "\n");
}

// Helpers for logging to MicroPython REPL
#define TRACE(str) mp_printf(&mp_plat_print, str "\n")
#define DEBUG_PRINTF(...) mp_printf(&mp_plat_print, __VA_ARGS__)

// Custom allocator for TinyUSB (isolates it from SDK heap limits)
#define USB_HOST_HEAP_SIZE (64 * 1024)
static uint8_t usb_host_heap[USB_HOST_HEAP_SIZE] __attribute__((aligned(8)));
static size_t usb_host_heap_pos = 0;

static void *tu_malloc(size_t size) {
  size = (size + 7) & ~7;
  if (usb_host_heap_pos + size > USB_HOST_HEAP_SIZE) return NULL;
  void *p = &usb_host_heap[usb_host_heap_pos];
  usb_host_heap_pos += size;
  return p;
}

static void *tu_calloc(size_t nmemb, size_t size) {
  size_t total = nmemb * size;
  void *p = tu_malloc(total);
  if (p) memset(p, 0, total);
  return p;
}

static void *tu_realloc(void *ptr, size_t size) {
  if (!ptr) return tu_malloc(size);
  void *q = tu_malloc(size);
  if (q) memcpy(q, ptr, size);
  return q;
}

static void tu_free(void *ptr) { (void)ptr; }


// TinyUSB Event Hooks
void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
  const char *name = "?";
  switch (eventid) {
    case HCD_EVENT_DEVICE_ATTACH: name = "ATTACH"; break;
    case HCD_EVENT_DEVICE_REMOVE: name = "REMOVE"; break;
    case HCD_EVENT_XFER_COMPLETE: name = "XFER_COMPLETE"; break;
  }
//  DEBUG_PRINTF("[USB Host] event rhport=%u id=%u(%s) in_isr=%d\n",
//               rhport, (unsigned)eventid, name, (int)in_isr);
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
  (void)desc_report; (void)desc_len;
//  DEBUG_PRINTF("[USB Host] HID mount: dev=%u inst=%u\n", dev_addr, instance);
  tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
//  DEBUG_PRINTF("[USB Host] HID unmount: dev=%u inst=%u\n", dev_addr, instance);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
  if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD &&
      len >= 8) {
    /* Store raw report for Python-level polling (usb_host.get_hid_report()) */
    memcpy(last_hid_report, report, 8);
  }
  tuh_hid_receive_report(dev_addr, instance);
}

const uint8_t *usb_host_core_get_hid_report(void) {
  return last_hid_report;
}

void usb_host_core_init(void) {
  /* clk_sys must be a clean multiple of 12 MHz for the USB SIE's
   * full-speed bit timing. 144 MHz (12*12) is a conservative common
   * choice, but caps CPU-bound emulation speed hard while USB host mode
   * is active (it's always active here, for the keyboard). 240 MHz
   * (12*20) is another standard, well-supported multiple that gets
   * closer to this board's normal 250 MHz operating clock. */
  set_sys_clock_khz(240000, true);
  sleep_ms(10);
  stdio_uart_init();

  gc_info_t gcstate;
  gc_info(&gcstate);
//  DEBUG_PRINTF("[USB Host] Native Init: free=%u used=%u sysclk=%u MHz\n",
//               (unsigned)gcstate.free, (unsigned)gcstate.used,
//               (unsigned)(clock_get_hz(clk_sys) / 1000000));

  if (!tuh_init(0)) {
    DEBUG_PRINTF("[USB Host] ERROR: tuh_init failed!\n");
    return;
  }
}

void usb_host_core_task(void) {
  tuh_task();
}

void usb_host_core_start_bg_timer(int interval_ms) {
  if (usb_bg_timer_active) return;
  if (interval_ms < 1) interval_ms = 8;
  add_repeating_timer_ms(-interval_ms, usb_bg_timer_callback, NULL, &usb_bg_timer);
  usb_bg_timer_active = true;
}

void usb_host_core_stop_bg_timer(void) {
  if (!usb_bg_timer_active) return;
  cancel_repeating_timer(&usb_bg_timer);
  usb_bg_timer_active = false;
}
