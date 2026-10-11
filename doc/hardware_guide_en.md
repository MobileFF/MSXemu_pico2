# Hardware Guide

This guide covers the hardware components and wiring needed to build the MSX1 emulator (Raspberry Pi Pico 2 + MicroPython).

## Bill of Materials (BOM)

- **Microcontroller**: Raspberry Pi Pico 2 (RP2350). Required for both USB host support and performance — the original RP2040 Pico is not supported.
- **Display**: one of the following (selectable at runtime via `config.txt`'s `lcd=` key; wiring and init sequence are identical for both)
  - **ST7796 480×320 TFT LCD** (a.k.a. MSP4021) — default. MSX's native 256×192 screen is centered on the panel, 1:1.
  - **ILI9341 320×240 TFT LCD** (a.k.a. MSP2402)
- **Storage**: Micro SD card module (SPI interface). Holds the BIOS/cartridge ROMs, config file, and save states.
- **USB host**: USB OTG adapter (Micro-USB male to USB-A female) for a keyboard. Uses the Pico 2's native USB port (USB CDC is disabled in this firmware, so a separate UART is needed for a debug/REPL serial console).
- **Audio output**: a passive piezo buzzer, or an RC low-pass filter feeding earphones/a speaker (see below).
- **Joystick** (optional): standard ATARI/MSX 9-pin joystick port equivalent, wired directly to GPIO.
- **Power**: USB 5V supply.
- **Misc**: breadboard, jumper wires.

## Pin Assignments

Aside from the console UART and USB, the LCD and SD card share the same SPI bus (SPI1).

### 1. LCD Display (SPI1)

| LCD pin | Pico pin | Function | Notes |
| :--- | :--- | :--- | :--- |
| VCC | 3.3V | Power | |
| GND | GND | Ground | |
| CS | GP9 | Chip select | |
| DC / RS | GP8 | Data/command | |
| RST | GP7 | Reset | |
| SDI (MOSI) | GP11 | SPI1 TX | Shared with SD |
| SCK | GP10 | SPI1 SCK | Shared with SD |
| SDO (MISO) | GP12 | SPI1 RX | Shared with SD (wire it even if unused) |
| LED (BL) | GP22 | Backlight | |

SPI baud rate is 62.5MHz (`SPI_BAUD` in `main.py`) — the highest rate confirmed reliable on this board's wiring; 125MHz visibly corrupted the display in real-hardware testing.

Set `lcd=ST7796` or `lcd=ILI9341` in `config.txt` to select the panel size (defaults to ST7796, 480×320 if omitted). MSX's native 256×192 resolution is rendered 1:1 (no scaling) centered on the panel — scaling was tried but roughly doubled the transfer time and made gameplay feel like slow motion, so it isn't used.

Set `rotate=180` in `config.txt` to flip the panel 180° (for an upside-down mounted panel).

### 2. SD Card Module (SPI1)

| SD pin | Pico pin | Function | Notes |
| :--- | :--- | :--- | :--- |
| VCC | 3.3V | Power | |
| GND | GND | Ground | |
| CS | GP15 | Chip select | |
| MOSI | GP11 | SPI1 TX | Shared with LCD |
| SCK | GP10 | SPI1 SCK | Shared with LCD |
| MISO | GP12 | SPI1 RX | Shared with LCD. Pull-up required (see below) |

> **Important**: SPI1_RX (GP12) doubles as the SD card's MISO. When the SD card is deselected (CS high) it tri-states MISO; **without a pull-up (an internal one is fine), a floating MISO line can read as 0x00, causing the SD init command sequence to falsely appear to succeed and then time out.** The firmware sets `gpio_pull_up(12)` automatically, but an external pull-up (~10kΩ) works too.

The Pico's onboard flash (QSPI, a separate bus from SPI1) has roughly 3MB free, used as a scratch cache for Mega ROM (bank-switched, large cartridge) support. The SD card is only read once, at cart-selection time — never during gameplay (see `usage_guide_en.md` for details).

### 3. UART Console

| Function | Pico pin | Notes |
| :--- | :--- | :--- |
| UART0 TX | GP0 | Debug / REPL output |
| UART0 RX | GP1 | REPL input |

Native USB CDC is disabled in this firmware (the native USB port is dedicated to USB host mode), so a separate USB-UART adapter (e.g. FTDI) connected to UART0 is needed for a development/debug serial connection (e.g. via `mpremote`).

### 4. Audio Output (PWM)

| Function | Pico pin | Notes |
| :--- | :--- | :--- |
| PSG audio output | **GP14** | PWM output (10-bit, ~234kHz carrier) |

#### Direct connection to a passive piezo buzzer (simple)

Connecting GP14 through a ~100Ω resistor straight to a buzzer produces sound, but the PWM's raw high-frequency content drives the buzzer's diaphragm directly, giving a somewhat harsh tone. Adjust `volume=` (default 256, 0-256) and `audio_filter=` (default 0, 0-8, a digital low-pass filter strength) in `config.txt` to taste — both are also live-adjustable from the runtime menu's Audio Settings screen.

#### Earphone/speaker output (recommended, needs extra hardware)

Feeding the PWM output straight into earphones has DC-offset, carrier-noise, and excessive-volume problems. A simple RC low-pass + DC-blocking circuit like the following is recommended:

```
GP14 ──[R1: 1kΩ]──┬──[R2: 1kΩ]──┬─── (DC block) ──[C2: 10µF]── earphone jack (TIP/RING tied)
                   │             │
                 [C1: 4.7nF]   [C1b: 4.7nF]
                   │             │
                  GND           GND
                                              earphone jack GND ─── GND
```

- Two RC low-pass stages (R1+C1, R2+C1b, ~34kHz cutoff each) sufficiently attenuate the 234kHz PWM carrier
- C2 (~10µF) blocks the DC component before reaching the earphone
- Tie TIP and RING to the same signal node through independent resistors (e.g. 220Ω each) to get mono output on both channels

### 5. Joystick (optional)

A standard ATARI/MSX 9-pin joystick port's signals, wired directly to GPIO (using the Pico's internal PULL_UP, active-low). Only JOY1 (one port) is supported.

| Signal | Pico pin | Notes |
| :--- | :--- | :--- |
| UP | GP18 | |
| DOWN | GP19 | |
| LEFT | GP20 | |
| RIGHT | GP21 | |
| TRIGGER A | GP26 | |
| TRIGGER B | GP27 | |

Each pin reads "pressed" when shorted to GND via a button/switch (active-low). On the software side, this state is exposed to the Z80 core through the exact same path real MSX hardware uses — the PSG's (AY-3-8910) I/O ports A/B, registers 14/15 — so unmodified real-hardware game joystick-reading routines work as-is.

Leaving the pins unconnected is fine (the internal pull-up keeps them HIGH, read as "nothing pressed").

### 6. USB Keyboard

- Connect a USB keyboard via a USB OTG adapter to the Pico 2's native Micro-USB port (GP24/25, using the TinyUSB host stack).
- USB host initialization forces `clk_sys` to 240MHz (needed for USB full-speed timing accuracy); the impact on emulation speed is minor.

#### Optional: PIO-USB keyboard + native USB CDC REPL (the `pico2_piousb` build)

For the `bldfrm_msx.sh pico2_piousb` target, wire the board as follows instead, to free the native USB port from the keyboard so it can be used as a CDC serial port for mpremote (real-hardware confirmed 2026-10-11: keyboard recognition, CDC REPL, and Mega ROM loading all working).

- Add a new USB connector (Micro-USB or USB-C female) and wire its D+/D- lines to the following GPIOs, each through a **22Ω series resistor**. No external pull-up/pull-down resistors are needed (RP2350's own internal pull-down is sufficient; the PIO-USB library drives it).
  | Signal | Pico 2 pin |
  | :--- | :--- |
  | D+ | GP4 |
  | D- | GP5 |
- Also wire GND and 5V (VBUS, to power the keyboard) to this connector.
- **Practical note (from a sibling project's own real-hardware bring-up)**: a temporary breadboard wiring combined with an external pull-down resistor was found to make SETUP packets (short control transfers) succeed while DATA-IN packets (longer transfers) frequently failed with CRC/receive errors. Prefer a solid connection (e.g. soldered to perfboard) over a breadboard for this wiring.
- Since the keyboard moves to PIO-USB on GP4/GP5 with this build, **do not flash it onto a Pico 2 that hasn't been wired this way** — the keyboard will simply not respond. Use the default `pico2` target (native USB host) for a normal Pico 2.

## Wiring Notes

- **SPI sharing**: the LCD and SD card's CS (chip select) pins must be independent GPIOs. Drive all CS pins HIGH at startup before initializing anything (the firmware does this automatically).
- **Power draw**: the LCD backlight draws significant current. If the Pico resets or the screen flickers, consider powering the display from a dedicated external 3.3V/5V regulator.
- **Logic levels**: all pins are 3.3V logic. Never connect a 5V signal directly to a Pico pin.
- **The LCD and SD card share an SPI bus, so a wiring fault on one can appear as a failure on the other too.** If the screen is black AND the SD card isn't recognized, first test a plain MicroPython SD mount (without the `msx` module at all) to isolate whether the problem is wiring or software.

## Related Source Files

- `src/msx/msx_core.c`'s `msx_init_display_hardware()` — LCD init and GPIO setup
- `mp/main.py`'s header comment and pin constants — the authoritative source for which GPIOs are actually in use
