"""
MSX1 keyboard matrix mapping
USB HID keycode → (row, col_bit) in the MSX 11×8 matrix.

Matrix layout (active-low: bit=0 means key pressed):
  Row  Col:  7      6      5      4      3      2      1      0
   0         7      6      5      4      3      2      1      0
   1         ;      ]      [      \      =      -      9      8
   2         B      A      ~      `      /      .      ,      M
   3         J      I      H      G      F      E      D      C
   4         R      Q      P      O      N      M      L      K
   5         Z      Y      X      W      V      U      T      S
   6         F3     F2     F1    CODE   CAPS   GRAPH  CTRL  SHIFT
   7         RET    SEL    BS    STOP   TAB    ESC    F5     F4
   8        RIGHT  DOWN    UP   LEFT   DEL    INS   HOME   SPACE
   9        NUM4   NUM3   NUM2  NUM1   NUM0    /      +      *
  10         .      ,      -      9      8      7      6      5
"""

# HID keycodes exported for special handling in main.py
HID_F5 = 0x3E   # save state
HID_F7 = 0x40   # GUI+F7: runtime emulator menu
HID_F8 = 0x41   # load state

# USB HID keycode → (row, col_bit)
# col_bit is the bit mask (e.g. 0x01 = column 0, 0x02 = column 1, ...)
# row: 0–10, col_bit: 0x01–0x80
HID_TO_MSX = {
    # Row 0: 0–7
    0x27: (0, 0x01),  # 0
    0x1E: (0, 0x02),  # 1
    0x1F: (0, 0x04),  # 2
    0x20: (0, 0x08),  # 3
    0x21: (0, 0x10),  # 4
    0x22: (0, 0x20),  # 5
    0x23: (0, 0x40),  # 6
    0x24: (0, 0x80),  # 7

    # Row 1: 8, 9, -, =, [, ], \, ;
    0x25: (1, 0x01),  # 8
    0x26: (1, 0x02),  # 9
    0x2D: (1, 0x04),  # -
    0x2E: (1, 0x08),  # =
    0x2F: (1, 0x10),  # [
    0x30: (1, 0x20),  # ]
    0x31: (1, 0x40),  # \ (backslash)
    0x33: (1, 0x80),  # ;

    # Row 2: M, comma, period, /, ` (grave), ~(shift+`), A, B
    0x10: (2, 0x01),  # M
    0x36: (2, 0x02),  # ,
    0x37: (2, 0x04),  # .
    0x38: (2, 0x08),  # /
    0x35: (2, 0x10),  # ` (grave / tilde)
    # 0x35 with shift → ~ but HID only sees 0x35 + shift modifier
    0x04: (2, 0x40),  # A
    0x05: (2, 0x80),  # B

    # Row 3: C, D, E, F, G, H, I, J
    0x06: (3, 0x01),  # C
    0x07: (3, 0x02),  # D
    0x08: (3, 0x04),  # E
    0x09: (3, 0x08),  # F
    0x0A: (3, 0x10),  # G
    0x0B: (3, 0x20),  # H
    0x0C: (3, 0x40),  # I
    0x0D: (3, 0x80),  # J

    # Row 4: K, L, M(dup), N, O, P, Q, R
    0x0E: (4, 0x01),  # K
    0x0F: (4, 0x02),  # L
    # 0x10 M already in row2
    0x11: (4, 0x08),  # N
    0x12: (4, 0x10),  # O
    0x13: (4, 0x20),  # P
    0x14: (4, 0x40),  # Q
    0x15: (4, 0x80),  # R

    # Row 5: S, T, U, V, W, X, Y, Z
    0x16: (5, 0x01),  # S
    0x17: (5, 0x02),  # T
    0x18: (5, 0x04),  # U
    0x19: (5, 0x08),  # V
    0x1A: (5, 0x10),  # W
    0x1B: (5, 0x20),  # X
    0x1C: (5, 0x40),  # Y
    0x1D: (5, 0x80),  # Z

    # Row 6: SHIFT, CTRL, GRAPH, CAPS, CODE, F1, F2, F3
    0xE1: (6, 0x01),  # Left SHIFT
    0xE5: (6, 0x01),  # Right SHIFT
    0xE0: (6, 0x02),  # Left CTRL
    0xE4: (6, 0x02),  # Right CTRL
    0xE2: (6, 0x04),  # Left ALT → GRAPH
    0xE6: (6, 0x04),  # Right ALT → GRAPH
    0x39: (6, 0x08),  # CAPS LOCK
    # CODE: no standard HID key → map to Right GUI (Windows key)
    0xE7: (6, 0x10),  # Right GUI → CODE
    0x3A: (6, 0x20),  # F1
    0x3B: (6, 0x40),  # F2
    0x3C: (6, 0x80),  # F3

    # Row 7: F4, F5, ESC, TAB, STOP, BS, SELECT, ENTER
    0x3D: (7, 0x01),  # F4
    0x3E: (7, 0x02),  # F5
    0x29: (7, 0x04),  # ESC
    0x2B: (7, 0x08),  # TAB
    # STOP: Scroll Lock or Pause
    0x47: (7, 0x10),  # Scroll Lock → STOP
    0x48: (7, 0x10),  # Pause/Break → STOP
    0x2A: (7, 0x20),  # Backspace → BS
    # SELECT: End key
    0x4D: (7, 0x40),  # End → SELECT
    0x28: (7, 0x80),  # Enter → RETURN

    # Row 8: SPACE, HOME, INS, DEL, LEFT, UP, DOWN, RIGHT
    0x2C: (8, 0x01),  # Space
    0x4A: (8, 0x02),  # Home
    0x49: (8, 0x04),  # Insert
    0x4C: (8, 0x08),  # Delete
    0x50: (8, 0x10),  # Left arrow
    0x52: (8, 0x20),  # Up arrow
    0x51: (8, 0x40),  # Down arrow
    0x4F: (8, 0x80),  # Right arrow

    # Row 9: numpad * + / 0 1 2 3 4
    0x55: (9, 0x01),  # Numpad *
    0x57: (9, 0x02),  # Numpad +
    0x54: (9, 0x04),  # Numpad /
    0x62: (9, 0x08),  # Numpad 0
    0x59: (9, 0x10),  # Numpad 1
    0x5A: (9, 0x20),  # Numpad 2
    0x5B: (9, 0x40),  # Numpad 3
    0x5C: (9, 0x80),  # Numpad 4

    # Row 10: numpad 5 6 7 8 9 - , .
    0x5D: (10, 0x01), # Numpad 5
    0x5E: (10, 0x02), # Numpad 6
    0x5F: (10, 0x04), # Numpad 7
    0x60: (10, 0x08), # Numpad 8
    0x61: (10, 0x10), # Numpad 9
    0x56: (10, 0x20), # Numpad -
    0x85: (10, 0x40), # Numpad , (some keyboards)
    0x63: (10, 0x80), # Numpad .
}

# Modifier byte bit positions (in USB HID report byte 0)
MOD_LCTRL  = 0x01
MOD_LSHIFT = 0x02
MOD_LALT   = 0x04
MOD_LGUI   = 0x08
MOD_RCTRL  = 0x10
MOD_RSHIFT = 0x20
MOD_RALT   = 0x40
MOD_RGUI   = 0x80


def apply_hid_report(msx_module, modifier, keycodes):
    """
    Convert a USB HID keyboard report to MSX key matrix updates.

    msx_module : the `msx` C module
    modifier   : HID modifier byte (uint8)
    keycodes   : list/bytes of up to 6 active HID keycodes
    """
    # Start with all keys released
    matrix = [0xFF] * 11  # active-low, 0xFF = all released

    # Handle modifier keys
    if modifier & (MOD_LSHIFT | MOD_RSHIFT):
        matrix[6] &= ~0x01
    if modifier & (MOD_LCTRL | MOD_RCTRL):
        matrix[6] &= ~0x02
    if modifier & (MOD_LALT | MOD_RALT):
        matrix[6] &= ~0x04  # ALT → GRAPH

    # Handle regular keys
    for hid_code in keycodes:
        if hid_code == 0:
            continue
        entry = HID_TO_MSX.get(hid_code)
        if entry:
            row, col_bit = entry
            matrix[row] &= ~col_bit

    # Push to C module
    for row in range(11):
        msx_module.set_key_matrix(row, matrix[row])
