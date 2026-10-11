/*
 * hcd_pio_usb_pico2.c — TinyUSB host-controller-driver glue for Pico 2's
 * OPTIONAL PIO-USB build variant (bldfrm_msx.sh's "pico2_piousb" target).
 *
 * 2026-10-11. Pico 2's USB-host port is normally wired to RP2350's
 * native USB host controller (the default pico2 build, hcd_rp2040.c —
 * see micropython_msx.cmake). This variant is for a pico2 board that has
 * instead been wired with a SECOND USB connector on a free GPIO pair
 * (GP4=D+/GP5=D-, see usb_host_core.c's usb_host_core_init_pico2_piousb()),
 * freeing the native USB controller entirely for a CDC REPL (mpremote
 * over USB-C directly, instead of the external USB-UART adapter this
 * project otherwise relies on) — see 調査用/RP2350-PiZero_USB
 * キーボード対応調査.md's 2026-10-11 addendum for the cross-project
 * background.
 *
 * This file is content-identical to this project's own
 * src/usb_host/hcd_pio_usb_pizero.c — the HCD glue layer between
 * TinyUSB's generic host stack and pio_usb's public API
 * (pio_usb.h/pio_usb_ll.h) has no board-specific code at all; every
 * board/pin/PIO-instance difference lives in usb_host_core.c's
 * usb_host_core_init_*() functions, not here. Kept as a separate file
 * per board/variant (matching this project's existing naming
 * convention, hcd_pio_usb_pizero.c) rather than shared/renamed, so that
 * touching the pico2 variant can never risk pizero's already
 * real-hardware-confirmed build — see that file's own header comment
 * for the full rationale this one inherits unchanged.
 *
 * See hcd_pio_usb_pizero.c's header comment for the detailed mechanism
 * notes (pio_usb_host_irq_handler() weak-symbol override, why
 * hcd_int_enable/disable() are no-ops, why there is no hcd_task()
 * equivalent here) — not repeated.
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
 * rhport 1 here (tuh_init(1) at the call site), matching pio_usb's own
 * upstream convention and this project's pizero variant. */
#define PIO_USB_RHPORT 1
#define PIO_USB_ROOT_IDX(rhport) ((rhport) - PIO_USB_RHPORT)

static pio_usb_configuration_t pio_host_cfg = PIO_USB_DEFAULT_CONFIG;

/* -----------------------------------------------------------------------
 * hcd_configure / hcd_init
 *
 * usb_host_core.c calls tuh_configure(PIO_USB_RHPORT,
 * TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg) with pin_dp/pio_tx_num/
 * pio_rx_num/tx_ch already filled in (see
 * usb_host_core_init_pico2_piousb()) before tuh_init(PIO_USB_RHPORT);
 * TinyUSB forwards that configuration struct here verbatim via
 * hcd_configure(), and hcd_init() is where pio_usb_host_init() actually
 * brings the port up.
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

/* No real interrupt to mask/unmask — see hcd_pio_usb_pizero.c's own
 * header comment ("hcd_int_enable()/hcd_int_disable() are true
 * no-ops"). */
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
 * in pio_usb_host.c. Called synchronously from inside
 * pio_usb_host_frame(), itself called once per millisecond from
 * usb_host_core.c's own hardware_alarm_*-driven SOF tick — NOT from a
 * real hardware PIO/DMA interrupt, but the naming is pio_usb's own and
 * kept as-is since this function's signature/linkage (the weak-symbol
 * override) is fixed by the library, not a naming choice made here.
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
