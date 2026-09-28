/*
 * hcd_pio_usb_pizero.c — TinyUSB host-controller-driver glue for the
 * Waveshare RP2350-PiZero's PIO-USB port.
 *
 * 2026-09-27, Phase 4 of the RP2350-PiZero port (see
 * 調査用/RP2350-PiZero_USBキーボード対応調査.md and
 * src/usb_host/pio_usb/PROVENANCE.md).
 *
 * This board's USB-host connector (GPIO28/29) is not wired to RP2350's
 * native USB host controller the way Pico 2's is — it only works via
 * PIO-bitbanged USB (sekigon-gonnoc/Pico-PIO-USB, vendored in
 * src/usb_host/pio_usb/). TinyUSB's generic host stack (usbh.c, hub.c,
 * hid_host.c — already built for Pico 2 in msx_usb_host_core_lib) talks
 * to *some* "HCD" (Host Controller Driver) through a small, fixed set of
 * hcd_* functions; this file IS that HCD for pizero, implemented against
 * pio_usb's public API (pio_usb.h/pio_usb_ll.h), so the rest of the host
 * stack — and this project's own usb_host_core.c/modusb_host.c HID
 * report handling — needs no board-specific changes at all.
 *
 * Written fresh against pio_usb 0.7.2's actual current API (every
 * function call below was individually confirmed to exist with this
 * exact signature in the vendored src/usb_host/pio_usb/*.c before being
 * used here — see PROVENANCE.md). This project already had an untested,
 * abandoned attempt at this exact glue (src/usb_host/hcd_pio_usb_custom.c,
 * a copy of TinyUSB's own upstream example) from very early in the
 * project; per the investigation doc's 2026-09-27 addendum, that attempt
 * was never actually built against a vendored pio_usb checkout and its
 * failure was never diagnosed, so it is treated here only as background,
 * not as a source to copy from. Ending up structurally similar to it is
 * expected — this IS what a correct TinyUSB<->pio_usb bridge looks like —
 * but every call here was independently verified, not copied.
 *
 * Key facts this file relies on (see PROVENANCE.md for the full detail):
 *   - PIO-USB's host mode is entirely self-contained once
 *     pio_usb_host_init() returns: it registers its own 1ms repeating
 *     hardware-alarm timer internally (pio_usb_host.c's sof_timer() ->
 *     pio_usb_host_frame()), which does the actual bit-banged transfer
 *     work AND calls pio_usb_host_irq_handler() (a *weak* symbol in
 *     pio_usb_host.c) whenever a root port's `ints` are set. This file's
 *     own strong, non-weak definition of pio_usb_host_irq_handler()
 *     below overrides that weak default at link time — this is the only
 *     hook TinyUSB-side event delivery needs; nothing here has to touch
 *     interrupts, register a PIO IRQ, or run a task loop by hand.
 *   - Because of the above, hcd_int_enable()/hcd_int_disable() are true
 *     no-ops (matches upstream's own pio-usb HCD example) and there is no
 *     hcd_task()-equivalent to call — usb_host_core.c's existing
 *     repeating-timer-driven tuh_task() call is only for TinyUSB's own
 *     higher-level event queue (attach/detach/xfer-complete), which is
 *     fed by hcd_event_handler() calls from this file's IRQ handler.
 *   - Deliberately runs on **core0** (called from usb_host_core_init(),
 *     not core1) — see PROVENANCE.md's "Board-specific facts" section
 *     for why core1 (fully committed to onboard DVI) is intentionally
 *     left alone.
 */

#include "tusb_option.h"

#if CFG_TUH_ENABLED && CFG_TUH_RPI_PIO_USB

#include <string.h>

#include "pico.h"
#include "pio_usb.h"
#include "pio_usb_ll.h"

#include "host/hcd.h"
#include "host/usbh.h"

/* TinyUSB reserves rhport 0 for whichever "native" controller mode a
 * board might also use; pio_usb's single root port is addressed as
 * rhport 1 here (tuh_init(1) at the call site) so both could coexist on
 * a board with both, matching pio_usb's own upstream convention (its
 * example glue uses the same offset). pizero only ever uses rhport 1. */
#define PIO_USB_RHPORT 1
#define PIO_USB_ROOT_IDX(rhport) ((rhport) - PIO_USB_RHPORT)

static pio_usb_configuration_t pio_host_cfg = PIO_USB_DEFAULT_CONFIG;

/* -----------------------------------------------------------------------
 * hcd_configure / hcd_init
 *
 * usb_host_core.c calls tuh_configure(PIO_USB_RHPORT,
 * TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg) with pin_dp/pio_tx_num/
 * pio_rx_num/tx_ch already filled in (board-specific — see that file)
 * before tuh_init(PIO_USB_RHPORT); TinyUSB forwards that configuration
 * struct here verbatim via hcd_configure(), and hcd_init() is where
 * pio_usb_host_init() actually brings the port up.
 * ----------------------------------------------------------------------- */
bool hcd_configure(uint8_t rhport, uint32_t cfg_id, const void *cfg_param) {
  (void)rhport;
  TU_VERIFY(cfg_id == TUH_CFGID_RPI_PIO_USB_CONFIGURATION);
  memcpy(&pio_host_cfg, cfg_param, sizeof(pio_usb_configuration_t));
  return true;
}

bool hcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
  (void)rhport;
  (void)rh_init;
  pio_usb_host_init(&pio_host_cfg);
  return true;
}

void hcd_port_reset(uint8_t rhport) {
  pio_usb_host_port_reset_start(PIO_USB_ROOT_IDX(rhport));
}

void hcd_port_reset_end(uint8_t rhport) {
  pio_usb_host_port_reset_end(PIO_USB_ROOT_IDX(rhport));
}

bool hcd_port_connect_status(uint8_t rhport) {
  root_port_t *root = PIO_USB_ROOT_PORT(PIO_USB_ROOT_IDX(rhport));
  return pio_usb_bus_get_line_state(root) != PORT_PIN_SE0;
}

tusb_speed_t hcd_port_speed_get(uint8_t rhport) {
  root_port_t *root = PIO_USB_ROOT_PORT(PIO_USB_ROOT_IDX(rhport));
  return root->is_fullspeed ? TUSB_SPEED_FULL : TUSB_SPEED_LOW;
}

void hcd_device_close(uint8_t rhport, uint8_t dev_addr) {
  pio_usb_host_close_device(PIO_USB_ROOT_IDX(rhport), dev_addr);
}

uint32_t hcd_frame_number(uint8_t rhport) {
  (void)rhport;
  return pio_usb_host_get_frame_number();
}

/* No real interrupt to mask/unmask — see this file's header comment
 * ("hcd_int_enable()/hcd_int_disable() are true no-ops"). */
void hcd_int_enable(uint8_t rhport) { (void)rhport; }
void hcd_int_disable(uint8_t rhport) { (void)rhport; }

/* -----------------------------------------------------------------------
 * Endpoint API — thin forwarders to pio_usb_host_*, each verified against
 * src/usb_host/pio_usb/pio_usb_host.c's actual current signatures.
 * ----------------------------------------------------------------------- */
bool hcd_edpt_open(uint8_t rhport, uint8_t dev_addr,
                    tusb_desc_endpoint_t const *desc_ep) {
  /* Low-speed devices behind a full-speed hub need PRE (preamble)
   * packets — hcd_devtree_get_info() (TinyUSB's own device-tree walker,
   * already built for Pico 2 in usbh.c/hub.c, unchanged here) reports
   * whether that applies to this device. */
  hcd_devtree_info_t dev_tree;
  hcd_devtree_get_info(dev_addr, &dev_tree);
  bool const need_pre = (dev_tree.hub_addr != 0 &&
                         dev_tree.speed == TUSB_SPEED_LOW);

  return pio_usb_host_endpoint_open(PIO_USB_ROOT_IDX(rhport), dev_addr,
                                    (uint8_t const *)desc_ep, need_pre);
}

bool hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr,
                    uint8_t *buffer, uint16_t buflen) {
  return pio_usb_host_endpoint_transfer(PIO_USB_ROOT_IDX(rhport), dev_addr,
                                       ep_addr, buffer, buflen);
}

bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  return pio_usb_host_endpoint_abort_transfer(PIO_USB_ROOT_IDX(rhport),
                                              dev_addr, ep_addr);
}

bool hcd_setup_send(uint8_t rhport, uint8_t dev_addr,
                     uint8_t const setup_packet[8]) {
  return pio_usb_host_send_setup(PIO_USB_ROOT_IDX(rhport), dev_addr,
                                 setup_packet);
}

bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  /* pio_usb has no separate stall-clear primitive (a keyboard's plain
   * interrupt-IN endpoint never stalls in practice); matches upstream's
   * own pio-usb HCD example, which also just reports success here. */
  (void)rhport; (void)dev_addr; (void)ep_addr;
  return true;
}

/* -----------------------------------------------------------------------
 * pio_usb_host_irq_handler — overrides the __attribute__((weak)) default
 * in pio_usb_host.c (see this file's header comment). Called
 * synchronously from inside pio_usb_host_frame(), itself called once per
 * millisecond from pio_usb's own internal repeating-timer callback — NOT
 * from a real hardware PIO/DMA interrupt, but the naming is pio_usb's
 * own and kept as-is since this function's signature/linkage (the weak-
 * symbol override) is fixed by the library, not a naming choice made
 * here.
 * ----------------------------------------------------------------------- */
static void handle_endpoint_irq(uint8_t tu_rhport, xfer_result_t result,
                                volatile uint32_t *ep_reg) {
  uint32_t const ep_all = *ep_reg;

  for (uint8_t ep_idx = 0; ep_idx < PIO_USB_EP_POOL_CNT; ep_idx++) {
    uint32_t const mask = (1u << ep_idx);
    if (!(ep_all & mask)) {
      continue;
    }

    endpoint_t *ep = PIO_USB_ENDPOINT(ep_idx);
    hcd_event_t event = {
        .rhport = tu_rhport,
        .event_id = HCD_EVENT_XFER_COMPLETE,
        .dev_addr = ep->dev_addr,
    };
    event.xfer_complete.ep_addr = ep->ep_num;
    event.xfer_complete.result = result;
    event.xfer_complete.len = ep->actual_len;
    hcd_event_handler(&event, true);
  }

  *ep_reg &= ~ep_all;
}

void pio_usb_host_irq_handler(uint8_t root_id) {
  uint8_t const tu_rhport = root_id + PIO_USB_RHPORT;
  root_port_t *root = PIO_USB_ROOT_PORT(root_id);
  uint32_t const ints = root->ints;

  if (ints & PIO_USB_INTS_ENDPOINT_COMPLETE_BITS) {
    handle_endpoint_irq(tu_rhport, XFER_RESULT_SUCCESS, &root->ep_complete);
  }
  if (ints & PIO_USB_INTS_ENDPOINT_STALLED_BITS) {
    handle_endpoint_irq(tu_rhport, XFER_RESULT_STALLED, &root->ep_stalled);
  }
  if (ints & PIO_USB_INTS_ENDPOINT_ERROR_BITS) {
    handle_endpoint_irq(tu_rhport, XFER_RESULT_FAILED, &root->ep_error);
  }
  if (ints & PIO_USB_INTS_CONNECT_BITS) {
    hcd_event_t event = {.rhport = tu_rhport, .event_id = HCD_EVENT_DEVICE_ATTACH};
    hcd_event_handler(&event, true);
  }
  if (ints & PIO_USB_INTS_DISCONNECT_BITS) {
    hcd_event_t event = {.rhport = tu_rhport, .event_id = HCD_EVENT_DEVICE_REMOVE};
    hcd_event_handler(&event, true);
  }

  root->ints &= ~ints;
}

#endif /* CFG_TUH_ENABLED && CFG_TUH_RPI_PIO_USB */
