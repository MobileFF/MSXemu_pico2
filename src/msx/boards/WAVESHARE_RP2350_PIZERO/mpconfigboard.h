// Board and hardware specific configuration for the Waveshare RP2350-PiZero.
// See mpconfigboard.cmake for provenance notes.

#define MICROPY_HW_BOARD_NAME "Waveshare RP2350-PiZero"
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)

// PSRAM pad exists on the PCB but is not populated on stock boards (see
// xroar-waveshare-rp2350-pizero's README) -- unlike WAVESHARE_RP2350B_CORE,
// do NOT enable it here. If a specific board has been hand-populated, this
// can be flipped on real hardware after confirming with a continuity check.
#define MICROPY_HW_ENABLE_PSRAM (0)

// Override machine_uart.c defaults (not currently referenced by
// machine_uart.c per WAVESHARE_RP2350B_CORE's own comment, kept here for
// parity/documentation).
#define MICROPY_HW_UART0_TX     (0)
#define MICROPY_HW_UART0_RX     (1)
#define MICROPY_HW_UART0_CTS    (-1)
#define MICROPY_HW_UART0_RTS    (-1)
