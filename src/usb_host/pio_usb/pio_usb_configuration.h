
#pragma once

typedef enum {
  PIO_USB_PINOUT_DPDM = 0,  // DM = DP+1
  PIO_USB_PINOUT_DMDP,      // DM = DP-1
} PIO_USB_PINOUT;

typedef struct {
    uint8_t pin_dp;
    uint8_t pio_tx_num;
    uint8_t sm_tx;
    uint8_t tx_ch;
    uint8_t pio_rx_num;
    uint8_t sm_rx;
    uint8_t sm_eop;
    void* alarm_pool;
    int8_t debug_pin_rx;
    int8_t debug_pin_eop;
    bool skip_alarm_pool;
    PIO_USB_PINOUT pinout;
} pio_usb_configuration_t;

#ifndef PIO_USB_DP_PIN_DEFAULT
#define PIO_USB_DP_PIN_DEFAULT 0
#endif

#define PIO_USB_TX_DEFAULT 0
#define PIO_SM_USB_TX_DEFAULT 0
#define PIO_USB_DMA_TX_DEFAULT 0

#define PIO_USB_RX_DEFAULT 0
#define PIO_SM_USB_RX_DEFAULT 1
#define PIO_SM_USB_EOP_DEFAULT 2

#define PIO_USB_DEBUG_PIN_NONE (-1)

#define PIO_USB_DEFAULT_CONFIG                                             \
  {                                                                        \
    PIO_USB_DP_PIN_DEFAULT, PIO_USB_TX_DEFAULT, PIO_SM_USB_TX_DEFAULT,     \
        PIO_USB_DMA_TX_DEFAULT, PIO_USB_RX_DEFAULT, PIO_SM_USB_RX_DEFAULT, \
        PIO_SM_USB_EOP_DEFAULT, NULL, PIO_USB_DEBUG_PIN_NONE,              \
        PIO_USB_DEBUG_PIN_NONE, false, PIO_USB_PINOUT_DPDM                 \
  }

/* 2026-09-27 deliberately reduced from upstream's default of 32: this
 * project only ever has a single HID keyboard on this port (a control
 * endpoint pair + one interrupt-IN report endpoint, plus slack for a
 * device behind a hub) — see
 * 調査用/RP2350-PiZero_USBキーボード対応調査.md and this directory's
 * PROVENANCE.md ("deliberate patches" note). Saves ~24 * sizeof(endpoint_t)
 * (~4.5KB) of .bss, which mattered for fitting under the GC heap's
 * required minimum on real hardware. */
#define PIO_USB_EP_POOL_CNT 8
#define PIO_USB_DEV_EP_CNT 16
#define PIO_USB_DEVICE_CNT 4
#define PIO_USB_HUB_PORT_CNT 8
#define PIO_USB_ROOT_PORT_CNT 2

#define PIO_USB_EP_SIZE 64
