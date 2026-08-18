/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if CFG_TUH_ENABLED && (CFG_TUSB_MCU == OPT_MCU_RP2040) && CFG_TUH_RPI_PIO_USB

#include "pico.h"
#include "pio_usb.h"
#include "pio_usb_ll.h"

//--------------------------------------------------------------------+
// INCLUDE
//--------------------------------------------------------------------+
#include "osal/osal.h"

#include "host/hcd.h"
#include "host/usbh.h"

#define RHPORT_OFFSET 1
#define RHPORT_PIO(_x) ((_x) - RHPORT_OFFSET)

static pio_usb_configuration_t pio_host_cfg = PIO_USB_DEFAULT_CONFIG;

//--------------------------------------------------------------------+
// HCD API
//--------------------------------------------------------------------+
bool hcd_configure(uint8_t rhport, uint32_t cfg_id, const void *cfg_param) {
  (void)rhport;
  TU_VERIFY(cfg_id == TUH_CFGID_RPI_PIO_USB_CONFIGURATION);
  memcpy(&pio_host_cfg, cfg_param, sizeof(pio_usb_configuration_t));
  return true;
}

bool hcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
  (void)rhport;
  (void)rh_init;

  // To run USB SOF interrupt in core1, call this init in core1
  pio_usb_host_init(&pio_host_cfg);

  return true;
}

void hcd_port_reset(uint8_t rhport) {
  uint8_t const pio_rhport = RHPORT_PIO(rhport);
  pio_usb_host_port_reset_start(pio_rhport);
}

void hcd_port_reset_end(uint8_t rhport) {
  uint8_t const pio_rhport = RHPORT_PIO(rhport);
  pio_usb_host_port_reset_end(pio_rhport);
}

bool hcd_port_connect_status(uint8_t rhport) {
  uint8_t const pio_rhport = RHPORT_PIO(rhport);

  root_port_t *root = PIO_USB_ROOT_PORT(pio_rhport);
  port_pin_status_t line_state = pio_usb_bus_get_line_state(root);

  return line_state != PORT_PIN_SE0;
}

tusb_speed_t hcd_port_speed_get(uint8_t rhport) {
  // TODO determine link speed
  uint8_t const pio_rhport = RHPORT_PIO(rhport);
  tusb_speed_t speed = PIO_USB_ROOT_PORT(pio_rhport)->is_fullspeed
                           ? TUSB_SPEED_FULL
                           : TUSB_SPEED_LOW;
  TU_LOG1("[HCD] port_speed_get rhport=%u speed=%u\n", rhport, (unsigned)speed);
  return speed;
}

// Close all opened endpoint belong to this device
void hcd_device_close(uint8_t rhport, uint8_t dev_addr) {
  uint8_t const pio_rhport = RHPORT_PIO(rhport);
  pio_usb_host_close_device(pio_rhport, dev_addr);
}

uint32_t hcd_frame_number(uint8_t rhport) {
  (void)rhport;
  return pio_usb_host_get_frame_number();
}

void hcd_int_enable(uint8_t rhport) { (void)rhport; }

void hcd_int_disable(uint8_t rhport) { (void)rhport; }

//--------------------------------------------------------------------+
// Endpoint API
//--------------------------------------------------------------------+

bool hcd_edpt_open(uint8_t rhport, uint8_t dev_addr,
                   tusb_desc_endpoint_t const *desc_ep) {
  hcd_devtree_info_t dev_tree;
  hcd_devtree_get_info(dev_addr, &dev_tree);
  bool const need_pre = (dev_tree.hub_addr && dev_tree.speed == TUSB_SPEED_LOW);

  uint8_t const pio_rhport = RHPORT_PIO(rhport);
  return pio_usb_host_endpoint_open(pio_rhport, dev_addr,
                                    (uint8_t const *)desc_ep, need_pre);
}

bool hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr,
                   uint8_t *buffer, uint16_t buflen) {
  uint8_t const pio_rhport = RHPORT_PIO(rhport);
  return pio_usb_host_endpoint_transfer(pio_rhport, dev_addr, ep_addr, buffer,
                                        buflen);
}

bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  uint8_t const pio_rhport = RHPORT_PIO(rhport);
  return pio_usb_host_endpoint_abort_transfer(pio_rhport, dev_addr, ep_addr);
}

bool hcd_setup_send(uint8_t rhport, uint8_t dev_addr,
                    uint8_t const setup_packet[8]) {
  uint8_t const pio_rhport = RHPORT_PIO(rhport);
  return pio_usb_host_send_setup(pio_rhport, dev_addr, setup_packet);
}

bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void)rhport;
  (void)dev_addr;
  (void)ep_addr;

  return true;
}

static void __no_inline_not_in_flash_func(handle_endpoint_irq)(
    uint8_t tu_rhport, xfer_result_t result, volatile uint32_t *ep_reg) {
  const uint32_t ep_all = *ep_reg;

  for (uint8_t ep_idx = 0; ep_idx < PIO_USB_EP_POOL_CNT; ep_idx++) {
    uint32_t const mask = (1u << ep_idx);

    if (ep_all & mask) {
      endpoint_t *ep = PIO_USB_ENDPOINT(ep_idx);

      // Bypassing buggy hcd_event_xfer_complete() in hcd.h
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
  }

  // clear all
  (*ep_reg) &= ~ep_all;
}

// IRQ Handler
void __no_inline_not_in_flash_func(pio_usb_host_irq_handler)(uint8_t root_id) {
  uint8_t const tu_rhport = root_id + 1;
  root_port_t *rport = PIO_USB_ROOT_PORT(root_id);
  uint32_t const ints = rport->ints;

  if (ints & PIO_USB_INTS_ENDPOINT_COMPLETE_BITS) {
    handle_endpoint_irq(tu_rhport, XFER_RESULT_SUCCESS, &rport->ep_complete);
  }

  if (ints & PIO_USB_INTS_ENDPOINT_STALLED_BITS) {
    handle_endpoint_irq(tu_rhport, XFER_RESULT_STALLED, &rport->ep_stalled);
  }

  if (ints & PIO_USB_INTS_ENDPOINT_ERROR_BITS) {
    handle_endpoint_irq(tu_rhport, XFER_RESULT_FAILED, &rport->ep_error);
  }

  if (ints & PIO_USB_INTS_CONNECT_BITS) {
    hcd_event_t event = {
        .rhport = tu_rhport,
        .event_id = HCD_EVENT_DEVICE_ATTACH,
    };
    hcd_event_handler(&event, true);
  }

  if (ints & PIO_USB_INTS_DISCONNECT_BITS) {
    hcd_event_t event = {
        .rhport = tu_rhport,
        .event_id = HCD_EVENT_DEVICE_REMOVE,
    };
    hcd_event_handler(&event, true);
  }

  // clear all
  rport->ints &= ~ints;
}

#endif
