#include "usb_host_core.h"
#include "py/runtime.h"
#include "py/mpprint.h"
#include "py/gc.h"
#include "tusb.h"
#include "host/hcd.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include <string.h>

#ifdef MSX_BOARD_PIZERO
#include "hardware/pio.h"
#include "hardware/timer.h"
#include "pico/time.h"
#include "pio_usb.h"
#include "pio_usb_ll.h"
#endif

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

// Custom allocator for TinyUSB (isolates it from SDK heap limits). A pure
// bump allocator (tu_free() below is a no-op) — usage only ever grows,
// driven by one-time device/HID enumeration buffers (descriptors, hub
// state) for however many devices CFG_TUH_DEVICE_MAX/CFG_TUH_HUB allow
// (1 device, 1 hub layer, for the single keyboard this project supports).
//
// 2026-09-27 (Phase 4, PIO-USB bring-up): investigated shrinking this for
// pizero's tighter RAM budget, but confirmed via nm that neither
// usbh.c/hub.c/hid_host.c (the only callers of malloc()/tu_malloc() in
// this static library) actually reference malloc anywhere in this
// project's configuration — this buffer is fully dead-code-eliminated
// regardless of its declared size, on both boards. Left at 64KB
// unconditionally rather than leave a misleading "this helped" comment;
// see bldfrm_msx.sh's MICROPY_C_HEAP_SIZE_VAL for where pizero's real
// Phase 4 RAM budget fix actually lives.
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

#ifdef MSX_BOARD_PIZERO
/* Drives pio_usb's 1ms SOF/keepalive/transfer tick ourselves — see the
 * skip_alarm_pool comment in usb_host_core_init_pizero() for why this
 * exists instead of letting the library create its own internal timer.
 *
 * 2026-09-27/28 real-hardware finding: this was originally driven via
 * pico_time's alarm_pool API (add_repeating_timer_us(), and before that
 * pio_usb's own internal alarm_pool_create()) — every alarm_pool_
 * create*() call, direct or lazily-created default, unconditionally
 * does `pool->lock = spin_lock_instance(next_striped_spin_lock_num())`
 * (pico_time/time.c), a lock drawn from the same shared 8-wide striped
 * pool used by unrelated subsystems firmware-wide (the same class of
 * hazard as disp_dvi.c's own striped-spinlock finding). Bypassing
 * alarm_pool entirely and driving this from the lower-level
 * hardware_alarm_* API instead (hardware/timer.h) — a self-rearming
 * one-shot alarm, claimed via hardware_alarm_claim_unused(), a one-time
 * claim-bitmap check with no runtime spinlock at all — removes that
 * hazard category outright, a genuine hardening kept regardless of what
 * turned out to actually be causing the crash chased during this same
 * investigation (see usb_host_core_init_pizero()'s own closing note).
 *
 * This handler deliberately does ONLY pio_usb_host_frame() (the one
 * thing pio_usb's own docs say must happen every 1ms, see pio_usb.h) —
 * NOT tuh_task(), which is polled instead from ordinary (non-interrupt)
 * context, once per game frame, via mp/main.py's poll_keyboard() /
 * usb_host.task(). Keeping tuh_task() (actual enumeration/control-
 * transfer processing) out of a 1kHz ISR is both safer and cheaper on
 * CPU budget than polling it that often when ~60Hz is plenty for
 * keyboard responsiveness. */
static volatile uint32_t pio_usb_sof_tick_count = 0;
static int pio_usb_sof_alarm_num = -1;

static void pio_usb_sof_alarm_handler(uint alarm_num) {
  pio_usb_sof_tick_count++;
  pio_usb_host_frame();
  hardware_alarm_set_target(alarm_num, delayed_by_us(get_absolute_time(), 1000));
}

/* Waveshare RP2350-PiZero: the USB-host connector (GPIO28/29) is wired
 * as a PIO-USB port, not RP2350's native USB host controller — see
 * board_config.py's USB_HOST_DP_PIN comment and
 * src/usb_host/pio_usb/PROVENANCE.md (Phase 4,
 * 調査用/RP2350-PiZero_USBキーボード対応調査.md). Deliberately does
 * NOT call set_sys_clock_khz() here, unlike the native-controller path
 * below: clk_sys must stay at whatever onboard DVI set it to (252MHz,
 * disp_dvi.c) — pio_usb_host_init() (src/usb_host/pio_usb/pio_usb_host.c)
 * computes its own PIO clock dividers dynamically from clock_get_hz
 * (clk_sys) at call time rather than requiring a specific fixed
 * frequency — confirmed working on real hardware (keyboard input
 * enumerates and delivers HID reports correctly at 252MHz).
 *
 * 2026-09-28 postscript: the striped-spinlock hardening above (hardware_
 * alarm_* instead of alarm_pool) and the stdio_uart_init() removal are
 * both genuine fixes for real hazards, kept for that reason — but the
 * reproducible "Hard assert" that motivated a full day chasing both of
 * them turned out to be neither: it was ports/rp2/uart.c's mp_uart_init()
 * (MICROPY_HW_ENABLE_UART_REPL's own UART0 IRQ handler) colliding with
 * machine.UART(0, ...) construction in mp/main.py's init_usb() —
 * irq_set_exclusive_handler()'s own hard_assert on a genuinely already-
 * claimed IRQ, num=33 (UART0_IRQ), traced via a temporary override of
 * hard_assertion_failure() printing __builtin_return_address(). The
 * actual reason it took so long to find: real-hardware testing that day
 * was against a stale, never-refreshed on-device copy of this project's
 * mp/main.py (deployed under the name main_msx.py, with no automated
 * sync step between editing the source and re-testing) — every C-side
 * fix was correctly deployed and tested each time (a fresh .uf2 flash
 * each time), but Python-side changes to main.py's pizero branches
 * (skipping boost_peri_clock()/the UART re-sync/start_bg_timer() on
 * pizero) never reached the device until that was noticed and fixed. */
static void usb_host_core_init_pizero(void) {
  /* 2026-09-27 real-hardware finding: stdio_uart_init() (pico-sdk's own
   * low-level UART setup, called here in the native-controller path
   * below because THAT path also reinitializes clk_sys and needs the
   * UART's baud divisor recomputed to match) reinitializes the same
   * physical UART0 hardware pizero's REPL is already actively using —
   * but pizero's REPL is driven entirely through MicroPython's OWN UART
   * driver (MICROPY_HW_ENABLE_UART_REPL). Calling pico-sdk's
   * stdio_uart_init() on top of that, mid-connection, corrupts whatever
   * is currently in flight over the SAME physical UART — confirmed on
   * real hardware. This project never changes clk_sys for USB on
   * pizero (unlike the native-controller path), so there is no baud
   * divisor to fix up here in the first place — just don't call it. */

  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = 28;     /* USB_HOST_DP_PIN, board_config.py; D- = +1 = GP29 */
  pio_cfg.pio_tx_num = 1;  /* pio1 — pio0 is fully committed to onboard DVI,
                            * see disp_dvi.c; this library needs a whole PIO
                            * block (3 of 4 state machines) to itself. */
  pio_cfg.sm_tx = 0;
  pio_cfg.pio_rx_num = 1;
  pio_cfg.sm_rx = 1;
  pio_cfg.sm_eop = 2;
  /* Highest DMA channel (RP2350 has 16, 0-15), claimed via
   * dma_claim_mask() inside pio_usb.c — deliberately far from DVI's own
   * dma_claim_unused_channel(true) calls (which hand out the lowest free
   * channels first, currently 6 of them) so the two can never collide
   * regardless of which subsystem happens to initialize first. */
  pio_cfg.tx_ch = 15;

  /* 2026-09-27 real-hardware finding #1: pio_usb_host_init() (pio_usb_
   * host.c) unconditionally does `alarm_pool_create(2, 1)` for its own
   * internal 1ms SOF timer whenever pio_cfg.alarm_pool is left NULL (the
   * default — see PIO_USB_DEFAULT_CONFIG) — hardware alarm #2, hardcoded.
   * MicroPython's rp2 port ALSO permanently claims hardware alarm #2 at
   * boot for its own internal soft-timer/scheduler
   * (MICROPY_HW_SOFT_TIMER_ALARM_NUM, ports/rp2/mpconfigport.h) —
   * unconditionally, before any Python code runs. The two claims
   * collided: hw_claim_or_assert's "already claimed" panic, seen on real
   * hardware as a bare "Hard assert" with no further text (this port's
   * panic handler doesn't surface hard_assert()'s formatted message,
   * only the literal string passed to the top-level panic() — the real
   * cause needed source inspection, not the crash output).
   *
   * 2026-09-27/28 real-hardware findings #2 and #3: supplying our own
   * pool, then later driving pio_usb_host_frame() via pico_time's
   * add_repeating_timer_us()/_ms() instead, each independently ran into
   * a same-core self-deadlock/hard_assert once real hardware exercised
   * them (see pio_usb_sof_alarm_handler()'s own comment above — every
   * alarm_pool_create*() call, direct or lazily-created default, draws
   * its lock from the same shared striped spinlock pool, which is what
   * actually causes this class of hazard, not which hardware alarm
   * number gets used). Fixed for good by bypassing alarm_pool entirely
   * — see that comment for the real mechanism (hardware_alarm_* direct,
   * claimed below). */
  pio_cfg.skip_alarm_pool = true;

  if (!tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg)) {
    DEBUG_PRINTF("[USB Host] ERROR: tuh_configure (PIO-USB) failed!\n");
    return;
  }
  if (!tuh_init(1)) {
    DEBUG_PRINTF("[USB Host] ERROR: tuh_init failed!\n");
    return;
  }

  pio_usb_sof_alarm_num = hardware_alarm_claim_unused(true);
  hardware_alarm_set_callback(pio_usb_sof_alarm_num, pio_usb_sof_alarm_handler);
  hardware_alarm_set_target(pio_usb_sof_alarm_num, delayed_by_us(get_absolute_time(), 1000));
}

/* Real-hardware bring-up diagnostic — lets Python poll core0/PIO-USB
 * root-port liveness (msx.dvi_debug()'s own sibling, same rationale —
 * see disp_dvi.c). Exposed as usb_host.debug(). Kept as a permanent,
 * low-cost tool rather than removed, matching this project's established
 * pattern for this class of hard-won bring-up diagnostics. */
void usb_host_core_debug_pizero(uint32_t *tick_count, uint32_t *connected,
                                 uint32_t *suspended, uint32_t *ints) {
  *tick_count = pio_usb_sof_tick_count;
  root_port_t *root = PIO_USB_ROOT_PORT(0);
  *connected = root->connected;
  *suspended = root->suspended;
  *ints = root->ints;
}
#endif /* MSX_BOARD_PIZERO */

void usb_host_core_init(void) {
#ifdef MSX_BOARD_PIZERO
  usb_host_core_init_pizero();
  return;
#else
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
#endif
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
