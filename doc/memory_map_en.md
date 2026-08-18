# MSX1 Emulator Memory Map

## Overview

This emulates MSX1's standard slot/page memory architecture. The Z80's 16-bit address space (0x0000-0xFFFF) is split into four 16KB pages, each of which independently selects one of four "slots."

```
0x0000 ┌─────────────────┐
       │ Page 0 (16KB)    │ <- slot_select bits[1:0]
0x4000 ├─────────────────┤
       │ Page 1 (16KB)    │ <- slot_select bits[3:2]
0x8000 ├─────────────────┤
       │ Page 2 (16KB)    │ <- slot_select bits[5:4]
0xC000 ├─────────────────┤
       │ Page 3 (16KB)    │ <- slot_select bits[7:6]
0xFFFF └─────────────────┘
```

Each 2-bit field of `slot_select` (PPI port A, I/O port 0xA8) selects the slot number (0-3) for the corresponding page.

| Slot | Contents | Implementation |
| :--- | :--- | :--- |
| 0 | BIOS/BASIC ROM (32KB) | `msx->bios[]` (fixed; occupies 0x0000-0x7FFF's worth, reads as 0xFF past 0x8000) |
| 1 | Cartridge slot 1 | `msx->cart[0]`, or `msx->cart_cache[0]` in paged mode |
| 2 | Cartridge slot 2 | `msx->cart[1]`, or `msx->cart_cache[1]` in paged mode (largely unused/unverified so far) |
| 3 | RAM (64KB) | `msx->ram[]` (fixed) |

Immediately after boot (right after `msx_reset()`), `slot_select = 0x00` — every page points at slot 0 (BIOS). The BIOS's own boot routine performs the actual slot switching by writing to port 0xA8.

---

## Bank Switching Inside the Cartridge Region (Addresses 0x4000-0xBFFF)

A non-bank-switched cartridge (`MSX_MAPPER_PLAIN`) just maps the ROM linearly starting at 0x4000. A bank-switched mapper instead splits 0x4000-0xBFFF into four 8KB windows, each independently selecting any 8KB page from the ROM.

| Window | Address range | ASCII-8 bank register write | ASCII-16 | KONAMI |
| :--- | :--- | :--- | :--- | :--- |
| 0 | 0x4000-0x5FFF | 0x6000-0x67FF | 0x6000-0x67FF (16KB granularity, selects win0+1 together) | Fixed (always ROM page 0) |
| 1 | 0x6000-0x7FFF | 0x6800-0x6FFF | ditto | 0x6000-0x6FFF |
| 2 | 0x8000-0x9FFF | 0x7000-0x77FF | 0x7000-0x77FF (16KB granularity, selects win2+3 together) | 0x8000-0x8FFF |
| 3 | 0xA000-0xBFFF | 0x7800-0x7FFF | ditto | 0xA000-0xAFFF |

- **ASCII-8**: each of the 4 windows independently gets an 8KB page number (0-255) written to it.
- **ASCII-16**: two 16KB registers (the 0x6000-range and 0x7000-range ones) each switch two windows at once (16KB = 8KB × 2).
- **KONAMI** (no SCC): window 0 is always fixed to ROM page 0 (not switchable). Only windows 1-3 can be switched.

See `cart_page_ptr()` (the read path) and `cart_mapper_write()` (bank-switch write detection) in `src/msx/msx_core.c`.

### Caching in Paged (Mega ROM) Mode

A large cartridge loaded via `msx_load_cart_paged()` keeps only a fixed 32KB cache (`msx->cart_cache[]`, 8KB × 4 windows) resident, matching the 4 windows above. A bank-switch triggers a fetch of just that window's 8KB via a registered callback. This 32KB footprint is constant regardless of the ROM's actual size (up to roughly 1MB is expected to work).

---

## I/O Port Map

| Port | Direction | Function |
| :--- | :--- | :--- |
| 0x98 | R/W | VDP data port (VRAM read/write) |
| 0x99 | R/W | VDP status/address register port |
| 0xA0 | W | PSG register select (low 4 bits = register number) |
| 0xA1 | W | PSG data write |
| 0xA2 | R | PSG data read (value of the currently selected register) |
| 0xA8 | R/W | PPI port A — slot select (`slot_select`, 2 bits per page) |
| 0xA9 | R | PPI port B — keyboard matrix read (the row selected by `key_row`, active-low) |
| 0xAA | R/W | PPI port C — bits[3:0] select the keyboard row (`key_row`); the full written value is mirrored into `ppi_c` |
| 0xAB | W | PPI control word (mode set). Accepted but ignored |

### Joystick Input via PSG Registers 14/15

As on real MSX hardware, joystick input is emulated through the PSG's I/O ports (register 14 = port A, register 15 = port B).

- Bit 6 of a write to register 15 toggles between JOY1 (0) and JOY2 (1) (`msx->joy_select`).
- Reading register 14 (via port 0xA2, when `psg_reg == 14`) returns `msx->joy_state[port]` for the currently selected port. Bit layout: bit0=Up, bit1=Down, bit2=Left, bit3=Right, bit4=TriggerA, bit5=TriggerB (all active-low, 0=pressed).
- On the Python side, `msx.set_joystick(port, state)` updates this once per frame.

---

## Key Fields of `msx_state_t` (`src/msx/msx_core.h`)

```c
typedef struct {
    z80 cpu;                    // Z80 CPU core (superzazu/z80)
    VrEmuTms9918 *vdp;           // VDP core (heap-allocated, holds 16KB VRAM internally)
    PSG *psg;                    // PSG core (heap-allocated)

    uint8_t bios[0x8000];        // Slot 0 (32KB)
    uint8_t ram[0x10000];        // Slot 3 (64KB)
    uint8_t *cart[2];            // Slots 1/2 (heap-allocated, full-in-RAM mode)
    uint32_t cart_size[2];
    uint8_t cart_type[2];        // MSX_MAPPER_*
    uint8_t cart_bank[2][4];     // Currently selected bank per window

    bool cart_paged[2];          // true = Mega ROM (paged) mode
    uint8_t *cart_cache[2];      // 4 x 8KB, heap-allocated only in paged mode
    int32_t cart_cache_page[2][4]; // ROM page currently cached per window

    uint8_t slot_select;         // PPI port A
    uint8_t key_matrix[11];      // Keyboard matrix (11 rows, active-low)
    uint8_t key_row;             // Currently selected row (PPI port C low 4 bits)
    uint8_t psg_reg;             // Currently selected PSG register

    uint16_t framebuf[2][256*192]; // Display framebuffer (double-buffered)
    uint8_t framebuf_ready_idx;

    int16_t audio_buf[512];      // One frame's worth of PSG audio samples

    uint16_t lcd_w, lcd_h;       // Panel size selected at runtime
    bool rotate_180;             // Screen rotation setting

    uint8_t joy_state[2];        // Joystick state (JOY1/JOY2)
    uint8_t joy_select;
} msx_state_t;
```

---

## Save-State Header (fixed 64 bytes)

`msx_save_t` (`msx_core.h`) is a fixed 64-byte struct bundling CPU registers, VDP registers, mapper banks, and I/O state. RAM (64KB) and VRAM (16KB) are read/written separately from the header, zero-copy (`msx.get_ram_view()` / `msx.get_vram_view()`).

| Offset | Size | Field | Contents |
| :--- | :--- | :--- | :--- |
| 0 | 8 | `magic` | `"MSX1SAV1"` |
| 8 | 2 | `pc` | Program counter |
| 10 | 2 | `sp` | Stack pointer |
| 12 | 2 | `ix` | Index register IX |
| 14 | 2 | `iy` | Index register IY |
| 16 | 1 | `a` | Accumulator |
| 17 | 1 | `f` | Flags (packed: S Z Y H X P N C) |
| 18 | 1 | `b` | Register B |
| 19 | 1 | `c` | Register C |
| 20 | 1 | `d` | Register D |
| 21 | 1 | `e` | Register E |
| 22 | 1 | `h` | Register H |
| 23 | 1 | `l` | Register L |
| 24 | 1 | `a_` | Shadow register A' |
| 25 | 1 | `f_` | Shadow register F' |
| 26 | 1 | `b_` | Shadow register B' |
| 27 | 1 | `c_` | Shadow register C' |
| 28 | 1 | `d_` | Shadow register D' |
| 29 | 1 | `e_` | Shadow register E' |
| 30 | 1 | `h_` | Shadow register H' |
| 31 | 1 | `l_` | Shadow register L' |
| 32 | 1 | `i` | Interrupt register I |
| 33 | 1 | `r` | Refresh register R |
| 34 | 1 | `iff1` | Interrupt flip-flop 1 |
| 35 | 1 | `iff2` | Interrupt flip-flop 2 |
| 36 | 1 | `int_mode` | Interrupt mode (0/1/2) |
| 37 | 1 | `halted` | HALT state flag |
| 38 | 1 | `slot_select` | PPI port A (slot select) |
| 39 | 1 | `psg_reg` | Currently selected PSG register |
| 40 | 1 | `key_row` | Currently selected keyboard row |
| 41 | 1 | `ppi_c` | PPI port C mirror |
| 42 | 8 | `vdp_regs[8]` | VDP registers R0-R7 |
| 50 | 8 | `cart_bank[2][4]` | Bank number per slot per window |
| 58 | 6 | `_pad` | Reserved (padding to 80 bytes, unused) |

Save file format (`save_state_to()` in `msx_menu.py`): a ~80.06KB binary consisting of the 64-byte header + 64KB RAM + 16KB VRAM concatenated. The header and RAM/VRAM are written via separate `f.write()` calls, never allocating a large temporary buffer.

---

## Related Python-Side Constants

`mp/msx_keymap.py`:

- MSX keyboard row indices (`MSX_KEY_ROW_*`, matching the definitions in `msx_core.h`)
- The USB HID keycode -> (row, bitmask) lookup table

`mp/main.py`:

- GPIO pin assignment constants (see `hardware_guide_en.md`)
- `LCD_SIZES` — resolution per panel model

---

## Related Source Files

- `src/msx/msx_core.h` — the complete definitions of `msx_state_t` / `msx_save_t`
- `src/msx/msx_core.c` — `msx_mem_read()` / `msx_mem_write()` (address decoding), `msx_port_read()` / `msx_port_write()` (I/O ports), `cart_page_ptr()` / `cart_mapper_write()` (bank switching)
- `src/msx/modmsx.c` — the Python-side save-state bindings (`get_state_header` / `set_state_header` / `get_ram_view` / `get_vram_view`)
