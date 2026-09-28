/*
 * Board header for the Waveshare RP2350-PiZero, modeled on pico-sdk's
 * weact_studio_rp2350b_core.h / MicroPython's waveshare_rp2350b_core.h
 * (same "no official pico-sdk support" pattern -- see mpconfigboard.cmake).
 *
 * -----------------------------------------------------
 * NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
 *       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
 * -----------------------------------------------------
 *
 * Pin facts below are confirmed against the vendor schematic by a working
 * reference port (github.com/ugufru/xroar-waveshare-rp2350-pizero, README
 * "Target hardware" pinout table, 2026-09 snapshot) -- this project's own
 * DVI/SD/USB/joystick pin choices live in mp/board_config.py's "pizero"
 * branch and src/msx/display/disp_dvi.c, not here; this file only carries
 * the handful of pico-sdk PICO_DEFAULT_* peripherals that MicroPython's
 * core (non-MSX) code may reference.
 */

#ifndef _BOARDS_WAVESHARE_RP2350_PIZERO_H
#define _BOARDS_WAVESHARE_RP2350_PIZERO_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define WAVESHARE_RP2350_PIZERO

// --- UART --- (confirmed: UART0 TX=GPIO0, RX=GPIO1)
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- I2C --- (confirmed: I2C0 SDA=GPIO6, SCL=GPIO7)
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 6
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 7
#endif

// --- LED --- no simple GPIO LED: status indicator is a WS2812 (NeoPixel)
// on GPIO2, which needs a PIO driver, not a plain digital pin. Leave
// PICO_DEFAULT_LED_PIN undefined rather than pointing it at GPIO2 and
// having something toggle it as if it were a normal LED.

// --- Stack size --- 2026-09-21: PICO_STACK_SIZE (pico/platform.h) is NOT
// actually consulted by MicroPython's RP2350 linker script
// (ports/rp2/memmap_mp_rp2350.ld) -- it computes the C stack region from
// a separate, hardcoded __micropy_extra_stack__ linker symbol instead
// (ports/rp2/CMakeLists.txt). A #define here would silently do nothing.
// See src/msx/micropython_msx.cmake's MSX_IS_PIZERO block for the actual
// fix and the real-hardware symptom it addresses.

// --- SPI --- MicroPython's ports/rp2/machine_spi.c requires these
// (unlike PICO_DEFAULT_I2C_*/UART_*, they are not individually #ifndef-
// guarded there) to define machine.SPI(0)'s power-on defaults, but this
// project's own SD/DVI code always passes explicit pins and never
// constructs machine.SPI(0) with no arguments -- these values are only
// load-bearing for the build to succeed, not for anything this project
// actually uses. Reused from WAVESHARE_RP2350B_CORE (same vendor, same
// RP2350B chip, same "no official pico-sdk support" situation) as an
// arbitrary-but-known-safe default rather than guessing fresh pins.
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 0
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 18
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 19
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 16
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 17
#endif

// Flash: 16MB QSPI NOR, chip part number not yet confirmed on this specific
// board -- W25Q080's boot2 stage is the same generic-compatible choice
// WAVESHARE_RP2350B_CORE (same vendor, same chip family) uses; revisit if
// real-hardware boot fails at the boot2 stage.
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

#endif
