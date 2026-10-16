/*
 * modmsx_ui.c — C port of (part of) mp/msx_menu.py's UI foundation.
 *
 * 2026-10-16: background and motivation. This project's Python UI layer
 * (mp's .py files — menu/ROM-browser/save-slots/display-settings/
 * runtime-menu) is
 * NOT lazily imported in msx_menu.py's own case — main.py imports it
 * eagerly at EVERY boot (see main.py's `from msx_menu import (...)` right
 * after `from msx_ext import load_extensions`), unlike the leaf screens
 * (msx_rom_browser.py/msx_runtime_menu.py/msx_save_slots.py/
 * msx_display_settings.py), which are already lazily imported specifically
 * to spread their compile cost out. msx_menu.py's own ~929 lines of
 * bytecode compilation is therefore paid on every single boot regardless
 * of what the user ever does — doc/memory_usage.md documents this project
 * repeatedly hitting real-hardware MemoryError crashes from exactly this
 * class of cost (compiling a large lazily-imported .py module at the
 * moment some OTHER feature's static/C-heap growth has shrunk the GC
 * heap further than expected — see that doc's §3.1/§3.2/§4.1 for three
 * separate real-hardware incidents of this same shape).
 *
 * The user's own framing (this session): the goal is NOT execution speed
 * (the menu/UI is already fast enough) but eliminating this *chronic,
 * structural* heap pressure. Moving msx_menu.py's foundation into a C
 * module compiled directly into firmware removes its compile-time GC-heap
 * cost entirely (there is no "compiling the .py file" step for code that
 * was never .py to begin with), while a hand-rolled C UI avoids the
 * runtime GC-heap churn too (no framebuf.FrameBuffer object, no Python
 * list/string churn for color constants, etc.). Per the user's explicit
 * intent, the CALL/RST subroutine-hook mechanism (msx.set_call_hook(),
 * mp/msx_ext.py/msx_fdd.py) stays Python — this module doesn't touch
 * that at all.
 *
 * Scope of THIS first phase (deliberately narrow — "段階的に" per the
 * user's own request): only the parts of msx_menu.py that are pure
 * in-memory logic with NO file/VFS access at all — MenuCanvas (draw
 * primitives straight onto msx_state.framebuf, no framebuf module
 * dependency), color constants, HID keycode constants + key polling,
 * display-suspend state (hdmi_suspend()/lcd_suspend()/set_display_state()),
 * and the small message-echo/word-wrap helpers. This is roughly 350 of
 * msx_menu.py's 929 lines (about a third) — the biggest chunk that can be
 * converted with the least risk, since none of it touches the SD/VFS layer
 * or the real-hardware-tuned cartridge-loading/flash-cache heuristics
 * (load_cart_smart() and friends), which stay in Python for a later phase.
 *
 * mp/msx_menu.py becomes a thin compatibility shim (`from msx_ui import
 * ... as ...`) re-exporting every name this module provides under its
 * EXISTING Python names, so mp/msx_rom_browser.py, mp/msx_runtime_menu.py,
 * mp/msx_save_slots.py, mp/msx_display_settings.py, mp/msx_mode_switch.py
 * and mp/main.py all keep working completely unchanged — none of them
 * import from msx_ui directly, only from msx_menu as before.
 *
 * Drawing is done directly against msx_state.framebuf[framebuf_ready_idx]
 * (uint16_t[MSX_SCREEN_W*MSX_SCREEN_H], RGB565 already byte-swapped for
 * SPI — see msx_core.c's rgb888_to_565be()) instead of going through
 * MicroPython's framebuf module, so the color constants below are
 * precomputed with that exact same byte-swap formula (verified to match
 * rgb888_to_565be()'s output bit-for-bit). Text rendering reuses
 * MicroPython's own built-in 8x8 font table (extmod/font_petme128_8x8.h,
 * the same one framebuf.FrameBuffer.text() uses) so on-screen text looks
 * identical to the Python version it replaces.
 */

#include <string.h>
#include <stdlib.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/mphal.h"
#include "py/builtin.h"
#include "py/stream.h"
#include "py/misc.h"
#include "extmod/vfs.h"

#include "msx_core.h"
#include "usb_host_core.h"
#include "extmod/font_petme128_8x8.h"

/* ---------------------------------------------------------------------
 * Color constants — RGB565, byte-swapped for SPI (see file header).
 * Precomputed to match mp/msx_menu.py's rgb()/msx_core.c's
 * rgb888_to_565be() bit-for-bit (verified by hand, both formulas are
 * identical: ((r>>3)<<11)|((g>>2)<<5)|(b>>3), then byte-swapped).
 * ------------------------------------------------------------------- */
#define UI_C_BLACK  0x0000
#define UI_C_WHITE  0xFFFF
#define UI_C_GREEN  0xE006
#define UI_C_YELLOW 0xE0FF
#define UI_C_GRAY   0x1084
#define UI_C_CYAN   0xFB06
#define UI_C_RED    0x00D8

/* HID keycodes used for navigation — same values as mp/msx_menu.py. */
#define UI_HID_UP    0x52
#define UI_HID_DOWN  0x51
#define UI_HID_LEFT  0x50
#define UI_HID_RIGHT 0x4F
#define UI_HID_ENTER 0x28
#define UI_HID_ESC   0x29

/* ---------------------------------------------------------------------
 * rgb(r, g, b) -> int  — general-purpose helper, kept for API parity
 * with mp/msx_menu.py even though nothing else in this project currently
 * calls it directly (the C_* constants above are precomputed instead).
 * ------------------------------------------------------------------- */
static mp_obj_t ui_rgb(mp_obj_t r_obj, mp_obj_t g_obj, mp_obj_t b_obj) {
    int r = mp_obj_get_int(r_obj);
    int g = mp_obj_get_int(g_obj);
    int b = mp_obj_get_int(b_obj);
    uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    uint16_t swapped = (uint16_t)((v >> 8) | (v << 8));
    return MP_OBJ_NEW_SMALL_INT(swapped);
}
static MP_DEFINE_CONST_FUN_OBJ_3(ui_rgb_obj, ui_rgb);

/* ---------------------------------------------------------------------
 * Display-state globals — mirrors mp/msx_menu.py's _display_mode/
 * _hdmi_suspended/_lcd_suspended module-level state exactly (same
 * semantics, same callers: main.py's boot sequence and the Display
 * Settings menu call set_display_state() once per change; hdmi_suspend()/
 * lcd_suspend() bracket SD-heavy operations — see mp/msx_menu.py's own
 * long comment on *why* for the full real-hardware rationale, unchanged
 * here).
 * ------------------------------------------------------------------- */
typedef enum { UI_DISPLAY_LCD = 0, UI_DISPLAY_HDMI = 1, UI_DISPLAY_DVI = 2 } ui_display_mode_t;

static ui_display_mode_t ui_display_mode = UI_DISPLAY_LCD;
static bool ui_hdmi_suspended = false;
static bool ui_lcd_suspended = false;

static mp_obj_t ui_set_display_state(mp_obj_t mode_obj) {
    const char *s = mp_obj_str_get_str(mode_obj);
    if (strcmp(s, "hdmi") == 0) {
        ui_display_mode = UI_DISPLAY_HDMI;
    } else if (strcmp(s, "dvi") == 0) {
        ui_display_mode = UI_DISPLAY_DVI;
    } else {
        ui_display_mode = UI_DISPLAY_LCD;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_set_display_state_obj, ui_set_display_state);

static mp_obj_t ui_hdmi_suspend(void) {
    bool prev = ui_hdmi_suspended;
    ui_hdmi_suspended = true;
    if (!prev) {
        mp_hal_delay_ms(20);
    }
    return mp_obj_new_bool(prev);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ui_hdmi_suspend_obj, ui_hdmi_suspend);

static mp_obj_t ui_hdmi_resume(mp_obj_t prev_obj) {
    ui_hdmi_suspended = mp_obj_is_true(prev_obj);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_hdmi_resume_obj, ui_hdmi_resume);

static mp_obj_t ui_lcd_suspend(void) {
    bool prev = ui_lcd_suspended;
    ui_lcd_suspended = true;
    if (!prev) {
        mp_hal_delay_ms(20);
    }
    return mp_obj_new_bool(prev);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ui_lcd_suspend_obj, ui_lcd_suspend);

static mp_obj_t ui_lcd_resume(mp_obj_t prev_obj) {
    ui_lcd_suspended = mp_obj_is_true(prev_obj);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_lcd_resume_obj, ui_lcd_resume);

/* ---------------------------------------------------------------------
 * Keyboard polling — mirrors mp/msx_menu.py's _get_key()/
 * _wait_key_release() exactly, but calls usb_host_core_task()/
 * usb_host_core_get_hid_report() (src/usb_host_core.h) directly instead
 * of going through the usb_host Python module object — no Python-level
 * method call at all, not even a bound-method lookup. The Python
 * versions took a `usb_host_mod` argument that was either the usb_host
 * module object or None (no USB host present); callers pass the exact
 * same thing here unchanged, we just only check it for None-ness.
 * ------------------------------------------------------------------- */
static int ui_get_key_c(mp_obj_t usb_host_obj) {
    if (usb_host_obj == mp_const_none) {
        return 0;
    }
    usb_host_core_task();
    const uint8_t *r = usb_host_core_get_hid_report();
    for (int i = 2; i < 8; i++) {
        if (r[i] != 0) {
            return r[i];
        }
    }
    return 0;
}

static mp_obj_t ui_get_key(mp_obj_t usb_host_obj) {
    return MP_OBJ_NEW_SMALL_INT(ui_get_key_c(usb_host_obj));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_get_key_obj, ui_get_key);

static mp_obj_t ui_wait_key_release(mp_obj_t usb_host_obj) {
    mp_hal_delay_ms(80);
    while (ui_get_key_c(usb_host_obj) != 0) {
        mp_hal_delay_ms(20);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_wait_key_release_obj, ui_wait_key_release);

/* ---------------------------------------------------------------------
 * Message echo/word-wrap helpers — mirrors mp/msx_menu.py's _echo_msg()/
 * _wrap_msg() exactly (same "only print when the message actually
 * changed" dedup, same word-wrap algorithm, hard-split included). Both
 * callers (mp/msx_display_settings.py, mp/msx_runtime_menu.py) always
 * call _wrap_msg(msg) with its default width=31/max_lines=2 — hardcoded
 * here rather than exposed as parameters, same simplification applied to
 * MenuCanvas's methods below (see their own comments).
 * ------------------------------------------------------------------- */
#define UI_WRAP_WIDTH     31
#define UI_WRAP_MAX_LINES 2
#define UI_LAST_MSG_BUF   160
#define UI_WRAP_LINE_BUF  64

static char ui_last_msg[UI_LAST_MSG_BUF];
static bool ui_have_last_msg = false;

/* Plain-C core shared by ui_echo_msg() and draw_message() below (which
 * needs the same "echo if changed" dedup for its own combined line1+
 * line2 message, without going through an mp_obj_t string). */
static void ui_echo_msg_c(const char *msg) {
    if (msg[0] != '\0' && (!ui_have_last_msg || strcmp(msg, ui_last_msg) != 0)) {
        mp_printf(&mp_plat_print, "MENU: %s\n", msg);
    }
    strncpy(ui_last_msg, msg, UI_LAST_MSG_BUF - 1);
    ui_last_msg[UI_LAST_MSG_BUF - 1] = '\0';
    ui_have_last_msg = true;
}

static mp_obj_t ui_echo_msg(mp_obj_t msg_obj) {
    ui_echo_msg_c(mp_obj_str_get_str(msg_obj));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_echo_msg_obj, ui_echo_msg);

/* Appends one wrapped line (len bytes starting at `text`) to `result`,
 * truncating to UI_WRAP_LINE_BUF-1 bytes if somehow longer (shouldn't
 * happen in practice: callers only ever pass <=UI_WRAP_WIDTH-byte
 * chunks here, this is just a safety net). */
static void ui_wrap_append(mp_obj_t result, const char *text, size_t len) {
    if (len >= UI_WRAP_LINE_BUF) {
        len = UI_WRAP_LINE_BUF - 1;
    }
    mp_obj_list_append(result, mp_obj_new_str(text, len));
}

static mp_obj_t ui_wrap_msg(mp_obj_t msg_obj) {
    const char *msg = mp_obj_str_get_str(msg_obj);
    mp_obj_t result = mp_obj_new_list(0, NULL);

    char cur[UI_WRAP_LINE_BUF];
    size_t cur_len = 0;
    int n_lines = 0;

    const char *p = msg;
    while (*p && n_lines < UI_WRAP_MAX_LINES) {
        const char *word = p;
        while (*p && *p != ' ') {
            p++;
        }
        size_t word_len = (size_t)(p - word);
        if (*p == ' ') {
            p++;
        }

        /* Hard-split words longer than UI_WRAP_WIDTH, same as the
         * Python `while len(word) > width:` loop. */
        while (word_len > UI_WRAP_WIDTH && n_lines < UI_WRAP_MAX_LINES) {
            if (cur_len > 0) {
                ui_wrap_append(result, cur, cur_len);
                n_lines++;
                cur_len = 0;
                if (n_lines >= UI_WRAP_MAX_LINES) {
                    break;
                }
            }
            ui_wrap_append(result, word, UI_WRAP_WIDTH);
            n_lines++;
            word += UI_WRAP_WIDTH;
            word_len -= UI_WRAP_WIDTH;
        }
        if (n_lines >= UI_WRAP_MAX_LINES) {
            break;
        }

        if (cur_len == 0) {
            size_t take = word_len < (UI_WRAP_LINE_BUF - 1) ? word_len : (UI_WRAP_LINE_BUF - 1);
            memcpy(cur, word, take);
            cur_len = take;
        } else if (cur_len + 1 + word_len <= UI_WRAP_WIDTH && cur_len + 1 + word_len < UI_WRAP_LINE_BUF) {
            cur[cur_len++] = ' ';
            memcpy(cur + cur_len, word, word_len);
            cur_len += word_len;
        } else {
            ui_wrap_append(result, cur, cur_len);
            n_lines++;
            cur_len = 0;
            if (n_lines < UI_WRAP_MAX_LINES) {
                size_t take = word_len < (UI_WRAP_LINE_BUF - 1) ? word_len : (UI_WRAP_LINE_BUF - 1);
                memcpy(cur, word, take);
                cur_len = take;
            }
        }
    }
    if (cur_len > 0 && n_lines < UI_WRAP_MAX_LINES) {
        ui_wrap_append(result, cur, cur_len);
    }
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_wrap_msg_obj, ui_wrap_msg);

/* ---------------------------------------------------------------------
 * MenuCanvas — direct draw primitives onto msx_state.framebuf, replacing
 * mp/msx_menu.py's MenuCanvas (which wrapped framebuf.FrameBuffer). The
 * pixel buffer pointer is captured once at construction time (the
 * "ready" half, msx_state.framebuf[framebuf_ready_idx]) exactly like the
 * Python version's self._buf = msx_module.get_framebuf() did — menus run
 * with gameplay paused, so framebuf_ready_idx doesn't flip for the
 * lifetime of a MenuCanvas instance (same assumption the Python version
 * relied on).
 *
 * Every draw method has a plain-C `mc_*()` core plus a thin mp_obj_t
 * wrapper, so draw_message() below (which needs to clear/fill-rect/text
 * a canvas internally) can call the exact same drawing code directly
 * instead of re-entering the MicroPython calling convention.
 * ------------------------------------------------------------------- */
typedef struct _menucanvas_obj_t {
    mp_obj_base_t base;
    uint16_t *buf; /* MSX_SCREEN_W * MSX_SCREEN_H pixels */
} menucanvas_obj_t;

extern const mp_obj_type_t menucanvas_type;

static inline void mc_setpixel(menucanvas_obj_t *self, int x, int y, uint16_t color) {
    if (x >= 0 && x < MSX_SCREEN_W && y >= 0 && y < MSX_SCREEN_H) {
        self->buf[y * MSX_SCREEN_W + x] = color;
    }
}

static void mc_clear(menucanvas_obj_t *self, uint16_t color) {
    for (int i = 0; i < MSX_SCREEN_W * MSX_SCREEN_H; i++) {
        self->buf[i] = color;
    }
}

static void mc_hline(menucanvas_obj_t *self, int x, int y, int w, uint16_t color) {
    for (int i = 0; i < w; i++) {
        mc_setpixel(self, x + i, y, color);
    }
}

static void mc_rect(menucanvas_obj_t *self, int x, int y, int w, int h, uint16_t color, bool fill) {
    if (fill) {
        for (int yy = y; yy < y + h; yy++) {
            for (int xx = x; xx < x + w; xx++) {
                mc_setpixel(self, xx, yy, color);
            }
        }
    } else {
        for (int xx = x; xx < x + w; xx++) {
            mc_setpixel(self, xx, y, color);
            mc_setpixel(self, xx, y + h - 1, color);
        }
        for (int yy = y; yy < y + h; yy++) {
            mc_setpixel(self, x, yy, color);
            mc_setpixel(self, x + w - 1, yy, color);
        }
    }
}

/* Renders via MicroPython's own built-in 8x8 font (font_petme128_8x8.h)
 * using the exact same per-column-byte, LSB-at-top bit layout as
 * extmod/modframebuf.c's framebuf_text() — on-screen text is pixel-
 * identical to the Python version it replaces. */
static void mc_text(menucanvas_obj_t *self, const char *str, mp_int_t x0, mp_int_t y0, uint16_t color) {
    for (; *str; ++str) {
        int chr = *(const uint8_t *)str;
        if (chr < 32 || chr > 127) {
            chr = 127;
        }
        const uint8_t *chr_data = &font_petme128_8x8[(chr - 32) * 8];
        for (int j = 0; j < 8; j++, x0++) {
            if (x0 >= 0 && x0 < MSX_SCREEN_W) {
                unsigned vline_data = chr_data[j];
                int y = (int)y0;
                for (; vline_data; vline_data >>= 1, y++) {
                    if (vline_data & 1) {
                        mc_setpixel(self, (int)x0, y, color);
                    }
                }
            }
        }
    }
}

/* flush() core — mirrors mp/msx_menu.py's MenuCanvas.flush() exactly:
 * dispatch on the display-mode/suspend globals above, calling the
 * underlying C render functions directly (no Python call at all, whereas
 * the version this replaces called msx_module.render_*() Python-bound
 * methods). */
static void mc_flush(void) {
    msx_state_t *msx = msx_get_state();
    bool use_hdmi = (ui_display_mode == UI_DISPLAY_HDMI) && !ui_hdmi_suspended;
    bool use_lcd  = (ui_display_mode == UI_DISPLAY_LCD)  && !ui_lcd_suspended;
    if (use_lcd) {
        msx_render_to_display_1to1(msx);
        msx_wait_display(msx);
    }
#ifdef MSX_BOARD_PIZERO
    /* msx_render_to_display_dvi() only exists on the pizero build (no
     * DVI/core1 on pico2 — see msx_core.h's own #ifdef around its
     * declaration) — guard the call the same way, not just the dead
     * branch, or a pico2 build fails with an implicit-declaration error
     * (real build failure hit while bringing this module up). */
    if (ui_display_mode == UI_DISPLAY_DVI) {
        msx_render_to_display_dvi(msx);
    }
#endif
    if (use_hdmi) {
        msx_render_to_hdmi_raw332(msx);
    }
}

/* ---- MicroPython bindings for MenuCanvas ------------------------------ */

static mp_obj_t menucanvas_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    /* Accepts (and ignores) the same single positional arg the Python
     * call sites pass (MenuCanvas(msx_module)/MenuCanvas(msx)) — kept for
     * zero call-site changes; we reach msx_state directly via
     * msx_get_state() instead of needing the module object. */
    (void)args;
    mp_arg_check_num(n_args, n_kw, 0, 1, false);

    menucanvas_obj_t *self = mp_obj_malloc(menucanvas_obj_t, &menucanvas_type);
    msx_state_t *msx = msx_get_state();
    self->buf = msx->framebuf[msx->framebuf_ready_idx];
    return MP_OBJ_FROM_PTR(self);
}

/* clear(color=C_BLACK) — the only draw method any call site invokes with
 * a default argument (mp/main.py's _show_error(): `c.clear()`), so this
 * one (and only this one) supports the 0-arg form. */
static mp_obj_t menucanvas_clear(size_t n_args, const mp_obj_t *args) {
    menucanvas_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint16_t color = (n_args >= 2) ? (uint16_t)mp_obj_get_int(args[1]) : UI_C_BLACK;
    mc_clear(self, color);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(menucanvas_clear_obj, 1, 2, menucanvas_clear);

/* text(s, x, y, color) — every call site in this project passes all 4
 * args explicitly, so no default-argument handling is needed. */
static mp_obj_t menucanvas_text(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    menucanvas_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    const char *str = mp_obj_str_get_str(args[1]);
    mp_int_t x0 = mp_obj_get_int(args[2]);
    mp_int_t y0 = mp_obj_get_int(args[3]);
    uint16_t color = (uint16_t)mp_obj_get_int(args[4]);
    mc_text(self, str, x0, y0, color);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(menucanvas_text_obj, 5, 5, menucanvas_text);

/* hline(x, y, w, color) — always called with all 4 args. */
static mp_obj_t menucanvas_hline(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    menucanvas_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    int x = mp_obj_get_int(args[1]);
    int y = mp_obj_get_int(args[2]);
    int w = mp_obj_get_int(args[3]);
    uint16_t color = (uint16_t)mp_obj_get_int(args[4]);
    mc_hline(self, x, y, w, color);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(menucanvas_hline_obj, 5, 5, menucanvas_hline);

/* rect(x, y, w, h, color, fill=False) — `fill` is always passed as an
 * explicit keyword arg at every call site in this project (`fill=True`),
 * but parsed properly (with its real False default) via mp_arg_parse_all
 * rather than assuming that, in case a future caller omits it. */
static mp_obj_t menucanvas_rect(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_x, ARG_y, ARG_w, ARG_h, ARG_color, ARG_fill };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_x,     MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_y,     MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_w,     MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_h,     MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_color, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_fill,  MP_ARG_BOOL,                  {.u_bool = false} },
    };
    menucanvas_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, vals);

    mc_rect(self, vals[ARG_x].u_int, vals[ARG_y].u_int, vals[ARG_w].u_int, vals[ARG_h].u_int,
            (uint16_t)vals[ARG_color].u_int, vals[ARG_fill].u_bool);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(menucanvas_rect_obj, 1, menucanvas_rect);

static mp_obj_t menucanvas_flush(mp_obj_t self_in) {
    (void)self_in;
    mc_flush();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(menucanvas_flush_obj, menucanvas_flush);

static const mp_rom_map_elem_t menucanvas_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_W),     MP_ROM_INT(MSX_SCREEN_W) },
    { MP_ROM_QSTR(MP_QSTR_H),     MP_ROM_INT(MSX_SCREEN_H) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&menucanvas_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_text),  MP_ROM_PTR(&menucanvas_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_hline), MP_ROM_PTR(&menucanvas_hline_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect),  MP_ROM_PTR(&menucanvas_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&menucanvas_flush_obj) },
};
static MP_DEFINE_CONST_DICT(menucanvas_locals_dict, menucanvas_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    menucanvas_type,
    MP_QSTR_MenuCanvas,
    MP_TYPE_FLAG_NONE,
    make_new, menucanvas_make_new,
    locals_dict, &menucanvas_locals_dict
    );

/* ---------------------------------------------------------------------
 * draw_message(canvas, title, line1, line2, color) — mirrors
 * mp/msx_menu.py's _draw_message(). Its only caller
 * (mp/msx_rom_browser.py) always passes all 5 args explicitly, so no
 * default-argument handling is needed here either. Calls the plain-C
 * mc_*() drawing cores directly (not the mp_obj_t-wrapped methods above)
 * since it already has a `menucanvas_obj_t *` in hand.
 * ------------------------------------------------------------------- */
static mp_obj_t ui_draw_message(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    menucanvas_obj_t *canvas = MP_OBJ_TO_PTR(args[0]);
    const char *title = mp_obj_str_get_str(args[1]);
    const char *line1 = mp_obj_str_get_str(args[2]);
    const char *line2 = mp_obj_str_get_str(args[3]);
    uint16_t color = (uint16_t)mp_obj_get_int(args[4]);

    /* _echo_msg(f"{line1} {line2}".strip() if line2 else line1) — built
     * by hand (no snprintf(): not confirmed available/linked in this
     * embedded build, unlike the libc string.h functions already used
     * throughout this project's C modules). */
    if (line2[0] != '\0') {
        char combined[UI_LAST_MSG_BUF];
        size_t l1 = strlen(line1);
        if (l1 > sizeof(combined) - 2) {
            l1 = sizeof(combined) - 2;
        }
        memcpy(combined, line1, l1);
        combined[l1] = ' ';
        size_t remaining = sizeof(combined) - 1 - (l1 + 1);
        size_t l2 = strlen(line2);
        if (l2 > remaining) {
            l2 = remaining;
        }
        memcpy(combined + l1 + 1, line2, l2);
        combined[l1 + 1 + l2] = '\0';
        ui_echo_msg_c(combined);
    } else {
        ui_echo_msg_c(line1);
    }

    mc_clear(canvas, UI_C_BLACK);
    mc_rect(canvas, 0, 0, MSX_SCREEN_W, 12, UI_C_CYAN, true);

    size_t title_len = strlen(title);
    if (title_len > 31) {
        title_len = 31;
    }
    char title_buf[32];
    memcpy(title_buf, title, title_len);
    title_buf[title_len] = '\0';
    mc_text(canvas, title_buf, 2, 2, UI_C_BLACK);

    size_t line1_len = strlen(line1);
    if (line1_len > 31) {
        line1_len = 31;
    }
    char line1_buf[32];
    memcpy(line1_buf, line1, line1_len);
    line1_buf[line1_len] = '\0';
    mc_text(canvas, line1_buf, 2, 20, color);

    if (line2[0] != '\0') {
        size_t line2_len = strlen(line2);
        if (line2_len > 31) {
            line2_len = 31;
        }
        char line2_buf[32];
        memcpy(line2_buf, line2, line2_len);
        line2_buf[line2_len] = '\0';
        mc_text(canvas, line2_buf, 2, 32, UI_C_GRAY);
    }

    mc_flush();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_draw_message_obj, 5, 5, ui_draw_message);

/* ---------------------------------------------------------------------
 * Phase 2 (2026-10-16): save-state I/O + the shared chunked-file-I/O
 * helpers, ported from mp/msx_menu.py. Unlike Phase 1 above, this phase
 * DOES touch the filesystem (open()/read()/write() via MicroPython's own
 * stream protocol, extmod/vfs.h's mp_vfs_remove()/mp_vfs_rename()/
 * mp_vfs_stat()) — still deliberately NOT the cart-loading/flash-cache
 * heuristics (load_cart_smart() and friends stay Python for a later
 * phase; those are real-hardware-tuned and higher-risk to touch in the
 * same pass) or msx.ini config I/O (load_config()/save_config() stay
 * Python too — save_config() in particular rewrites the user's actual
 * config file in place, and getting that line-preserving logic
 * byte-for-byte right in C isn't worth the risk for the modest eager-
 * compile savings it would buy).
 *
 * readinto_chunked()/write_chunked() are used not just by save-state
 * here but also by mp/main.py's load_bios_file() and msx_menu.py's own
 * (still-Python) load_cart_smart() — moving just these two generic
 * helpers benefits all three call sites.
 * ------------------------------------------------------------------- */

#define UI_CHUNK_SIZE 4096
#define UI_MAX_SAVE_SLOTS_DEFAULT 10

/* Plain-C cores (no mp_obj_t file-closing/exception bookkeeping here —
 * callers that need that, like ui_save_state_to()/ui_load_state_from()
 * below, wrap these themselves). Semantics deliberately match
 * mp/msx_menu.py's readinto_chunked()/write_chunked() exactly: each
 * iteration issues AT MOST ONE underlying stream read()/write() call
 * (MP_STREAM_RW_ONCE for reads) bounded to UI_CHUNK_SIZE bytes — this is
 * the real-hardware-tuned SD-reliability behavior those functions exist
 * for (see their own long comments in msx_menu.py's history), not
 * something this port should change even incidentally. */
static mp_uint_t ui_readinto_chunked_c(mp_obj_t f_obj, uint8_t *dst, mp_uint_t size) {
    mp_uint_t pos = 0;
    while (pos < size) {
        mp_uint_t remaining = size - pos;
        mp_uint_t want = remaining > UI_CHUNK_SIZE ? UI_CHUNK_SIZE : remaining;
        int errcode = 0;
        mp_uint_t n = mp_stream_rw(f_obj, dst + pos, want, &errcode, MP_STREAM_RW_READ | MP_STREAM_RW_ONCE);
        if (errcode != 0) {
            mp_raise_OSError(errcode);
        }
        if (n == 0) {
            break; /* EOF */
        }
        pos += n;
    }
    return pos;
}

static void ui_write_chunked_c(mp_obj_t f_obj, const uint8_t *src, mp_uint_t size) {
    mp_uint_t pos = 0;
    while (pos < size) {
        mp_uint_t remaining = size - pos;
        mp_uint_t chunk = remaining > UI_CHUNK_SIZE ? UI_CHUNK_SIZE : remaining;
        int errcode = 0;
        mp_stream_write_exactly(f_obj, src + pos, chunk, &errcode);
        if (errcode != 0) {
            mp_raise_OSError(errcode);
        }
        pos += chunk;
    }
}

/* readinto_chunked(f, buf, size) -> int */
static mp_obj_t ui_readinto_chunked(mp_obj_t f_obj, mp_obj_t buf_obj, mp_obj_t size_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_obj, &bufinfo, MP_BUFFER_WRITE);
    mp_uint_t size = (mp_uint_t)mp_obj_get_int(size_obj);
    if (size > (mp_uint_t)bufinfo.len) {
        size = (mp_uint_t)bufinfo.len; /* safety net: never write past buf */
    }
    mp_uint_t n = ui_readinto_chunked_c(f_obj, (uint8_t *)bufinfo.buf, size);
    return mp_obj_new_int_from_uint(n);
}
static MP_DEFINE_CONST_FUN_OBJ_3(ui_readinto_chunked_obj, ui_readinto_chunked);

/* write_chunked(f, buf) -> None */
static mp_obj_t ui_write_chunked(mp_obj_t f_obj, mp_obj_t buf_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_obj, &bufinfo, MP_BUFFER_READ);
    ui_write_chunked_c(f_obj, (const uint8_t *)bufinfo.buf, (mp_uint_t)bufinfo.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ui_write_chunked_obj, ui_write_chunked);

static mp_obj_t ui_open_file(mp_obj_t path_obj, qstr mode) {
    mp_obj_t open_args[2] = { path_obj, MP_OBJ_NEW_QSTR(mode) };
    return mp_builtin_open(2, open_args, (mp_map_t *)&mp_const_empty_map);
}

/* save_state_to(msx_module, path) — msx_module is accepted (for zero
 * call-site changes, same as MenuCanvas's constructor arg in Phase 1)
 * but unused: this file reaches msx_state directly via msx_get_state()
 * instead. Writes header+RAM+VRAM as separate pieces (never one big
 * ~80KB contiguous buffer) — same zero-large-allocation rationale as
 * the Python version this replaces. */
static mp_obj_t ui_save_state_to(mp_obj_t msx_module_obj, mp_obj_t path_obj) {
    (void)msx_module_obj;
    msx_state_t *msx = msx_get_state();
    uint8_t header[MSX_SAVE_HDR_SZ];
    if (!msx_save_state_header(msx, header, sizeof(header))) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("get_state_header() failed"));
    }

    mp_obj_t f = ui_open_file(path_obj, MP_QSTR_wb);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        int errcode = 0;
        mp_stream_write_exactly(f, header, sizeof(header), &errcode);
        if (errcode != 0) {
            mp_raise_OSError(errcode);
        }
        ui_write_chunked_c(f, msx->ram, MSX_RAM_SIZE);
        uint8_t *vram = msx_get_vram_ptr(msx);
        ui_write_chunked_c(f, vram, MSX_VRAM_SIZE);
        nlr_pop();
    } else {
        mp_stream_close(f);
        nlr_jump(nlr.ret_val);
    }
    mp_stream_close(f);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ui_save_state_to_obj, ui_save_state_to);

/* load_state_from(msx_module, path) -> bool */
static mp_obj_t ui_load_state_from(mp_obj_t msx_module_obj, mp_obj_t path_obj) {
    (void)msx_module_obj;
    msx_state_t *msx = msx_get_state();
    mp_obj_t f = ui_open_file(path_obj, MP_QSTR_rb);
    bool ok = false;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        uint8_t header[MSX_SAVE_HDR_SZ];
        mp_uint_t n_header = ui_readinto_chunked_c(f, header, sizeof(header));
        if (!msx_load_state_header(msx, header, n_header)) {
            ok = false;
        } else {
            mp_uint_t n_ram = ui_readinto_chunked_c(f, msx->ram, MSX_RAM_SIZE);
            uint8_t *vram = msx_get_vram_ptr(msx);
            mp_uint_t n_vram = ui_readinto_chunked_c(f, vram, MSX_VRAM_SIZE);
            ok = (n_ram == MSX_RAM_SIZE) && (n_vram == MSX_VRAM_SIZE);
        }
        nlr_pop();
    } else {
        mp_stream_close(f);
        nlr_jump(nlr.ret_val);
    }
    mp_stream_close(f);
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ui_load_state_from_obj, ui_load_state_from);

/* save_slot_path(base, slot) -> str : f"{base}.{slot}.sav" */
static mp_obj_t ui_save_slot_path_c(mp_obj_t base_obj, mp_int_t slot) {
    vstr_t vstr;
    vstr_init(&vstr, 24);
    size_t base_len;
    const char *base = mp_obj_str_get_data(base_obj, &base_len);
    vstr_add_strn(&vstr, base, base_len);
    vstr_printf(&vstr, ".%d.sav", (int)slot);
    return mp_obj_new_str_from_vstr(&vstr);
}

static mp_obj_t ui_save_slot_path(mp_obj_t base_obj, mp_obj_t slot_obj) {
    return ui_save_slot_path_c(base_obj, mp_obj_get_int(slot_obj));
}
static MP_DEFINE_CONST_FUN_OBJ_2(ui_save_slot_path_obj, ui_save_slot_path);

/* save_base_for_cart(cart_path, fallback_base) -> str */
static mp_obj_t ui_save_base_for_cart(mp_obj_t cart_path_obj, mp_obj_t fallback_obj) {
    if (cart_path_obj == mp_const_none) {
        return fallback_obj;
    }
    size_t len;
    const char *s = mp_obj_str_get_data(cart_path_obj, &len);
    if (len == 0) {
        return fallback_obj;
    }
    if (len >= 4) {
        char ext[4];
        for (int i = 0; i < 4; i++) {
            char c = s[len - 4 + i];
            if (c >= 'A' && c <= 'Z') {
                c += 32;
            }
            ext[i] = c;
        }
        if (ext[0] == '.' && ext[1] == 'r' && ext[2] == 'o' && ext[3] == 'm') {
            return mp_obj_new_str(s, len - 4);
        }
    }
    return cart_path_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ui_save_base_for_cart_obj, ui_save_base_for_cart);

/* try_vfs_*: swallow any exception (matches `except OSError: pass`) —
 * os.remove()/os.rename()/os.stat() only ever raise OSError in practice
 * for a missing file, so catching everything here is a deliberate, safe
 * simplification (same reasoning as mp/msx_ext.py's load_extensions()
 * broad `except Exception`). */
static bool ui_try_vfs_remove(mp_obj_t path) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_vfs_remove(path);
        nlr_pop();
        return true;
    }
    return false;
}

static bool ui_try_vfs_rename(mp_obj_t old_path, mp_obj_t new_path) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_vfs_rename(old_path, new_path);
        nlr_pop();
        return true;
    }
    return false;
}

static bool ui_try_vfs_stat(mp_obj_t path) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_vfs_stat(path);
        nlr_pop();
        return true;
    }
    return false;
}

/* rotate_and_save_state(msx_module, base, max_slots=MAX_SAVE_SLOTS) */
static mp_obj_t ui_rotate_and_save_state(size_t n_args, const mp_obj_t *args) {
    mp_obj_t base_obj = args[1];
    mp_int_t max_slots = (n_args >= 3) ? mp_obj_get_int(args[2]) : UI_MAX_SAVE_SLOTS_DEFAULT;

    ui_try_vfs_remove(ui_save_slot_path_c(base_obj, max_slots - 1));
    for (mp_int_t i = max_slots - 2; i >= 0; i--) {
        ui_try_vfs_rename(ui_save_slot_path_c(base_obj, i), ui_save_slot_path_c(base_obj, i + 1));
    }
    ui_save_state_to(args[0], ui_save_slot_path_c(base_obj, 0));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_rotate_and_save_state_obj, 2, 3, ui_rotate_and_save_state);

/* list_save_slots(base, max_slots=MAX_SAVE_SLOTS) -> list[int] */
static mp_obj_t ui_list_save_slots(size_t n_args, const mp_obj_t *args) {
    mp_obj_t base_obj = args[0];
    mp_int_t max_slots = (n_args >= 2) ? mp_obj_get_int(args[1]) : UI_MAX_SAVE_SLOTS_DEFAULT;
    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (mp_int_t i = 0; i < max_slots; i++) {
        if (ui_try_vfs_stat(ui_save_slot_path_c(base_obj, i))) {
            mp_obj_list_append(result, MP_OBJ_NEW_SMALL_INT(i));
        }
    }
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_list_save_slots_obj, 1, 2, ui_list_save_slots);

/* ---------------------------------------------------------------------
 * Phase 3 (2026-10-16): msx.ini config I/O + cartridge loading, ported
 * from mp/msx_menu.py. This is the highest-risk phase so far (real-
 * hardware-tuned heuristics: the pizero Mega-ROM refusal, the KONAMI
 * mapper-detection fallback, the flash-cache invalidate-before-write
 * ordering) — ported as a careful line-by-line translation rather than
 * a redesign, specifically to preserve every one of those behaviors
 * exactly. One deliberate exception: msx.load_cart_paged() itself is
 * NOT reimplemented here — it's still called as a method on the passed-
 * in msx_module object (ui_call_method_n() below), because its Python
 * binding (modmsx.c's msx_py_load_cart_paged()) does its own GC-rooting
 * of the open file object across repeated page-fetch callbacks for the
 * life of the cart; reimplementing that lifetime/callback machinery in
 * C is out of scope for this phase and buys nothing (that function call
 * itself was never the compile-cost or heap-pressure problem — the
 * *orchestration* around it was).
 * ------------------------------------------------------------------- */

/* Convenience: a Python str object from a C string literal/constant —
 * used for the fixed paths below, which aren't valid MP_QSTR_*
 * identifiers (they contain '/' and '.'). */
static mp_obj_t ui_str(const char *s) {
    return mp_obj_new_str(s, strlen(s));
}

/* Call self_obj.method(*args) — the standard 2-slot MicroPython method-
 * call convention (mp_load_method() fills dest[0]/dest[1], actual args
 * go in dest[2..]). Capped at 4 args (ui_load_cart_smart()'s
 * load_cart_paged(slot, f, size, mapper) call is the only user). */
static mp_obj_t ui_call_method_n(mp_obj_t self_obj, qstr method, size_t n_args, const mp_obj_t *args) {
    mp_obj_t dest[2 + 4];
    mp_load_method(self_obj, method, dest);
    for (size_t i = 0; i < n_args; i++) {
        dest[2 + i] = args[i];
    }
    return mp_call_method_n_kw(n_args, 0, dest);
}

static bool ui_is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static void ui_strip_trim(const char *s, size_t len, size_t *out_start, size_t *out_len) {
    size_t a = 0, b = len;
    while (a < b && ui_is_ws(s[a])) {
        a++;
    }
    while (b > a && ui_is_ws(s[b - 1])) {
        b--;
    }
    *out_start = a;
    *out_len = b - a;
}

/* Best-effort whole-file text read: true (and `out` filled) on success;
 * false (open or read failed for any reason -- missing file, OSError,
 * ...) otherwise, `out` left as whatever was appended so far. Mirrors
 * the various `except OSError: ...`/`try: ... except: lines = []`
 * fallbacks this logic has throughout msx_menu.py. */
static bool ui_try_read_whole_text(mp_obj_t path_obj, vstr_t *out) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t f = ui_open_file(path_obj, MP_QSTR_r);
        nlr_buf_t nlr2;
        if (nlr_push(&nlr2) == 0) {
            char chunk[192];
            for (;;) {
                int errcode = 0;
                mp_uint_t n = mp_stream_rw(f, chunk, sizeof(chunk), &errcode, MP_STREAM_RW_READ | MP_STREAM_RW_ONCE);
                if (errcode != 0) {
                    mp_raise_OSError(errcode);
                }
                if (n == 0) {
                    break;
                }
                vstr_add_strn(out, chunk, n);
            }
            nlr_pop();
        } else {
            mp_stream_close(f);
            nlr_jump(nlr2.ret_val);
        }
        mp_stream_close(f);
        nlr_pop();
        return true;
    }
    return false;
}

/* ---- msx.ini config I/O ---- */

static void ui_parse_ini_lines(const char *buf, size_t len, mp_obj_t cfg) {
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        while (i < len && buf[i] != '\n') {
            i++;
        }
        size_t line_len = i - line_start;
        if (i < len) {
            i++;
        }

        size_t s, l;
        ui_strip_trim(buf + line_start, line_len, &s, &l);
        const char *line = buf + line_start + s;
        if (l == 0 || line[0] == '#') {
            continue;
        }

        size_t eq = (size_t)-1;
        for (size_t j = 0; j < l; j++) {
            if (line[j] == '=') {
                eq = j;
                break;
            }
        }
        if (eq == (size_t)-1) {
            continue;
        }

        size_t ks, kl;
        ui_strip_trim(line, eq, &ks, &kl);
        size_t vs, vl;
        ui_strip_trim(line + eq + 1, l - eq - 1, &vs, &vl);

        char keybuf[64];
        size_t use_kl = kl >= sizeof(keybuf) ? sizeof(keybuf) - 1 : kl;
        for (size_t j = 0; j < use_kl; j++) {
            char c = line[ks + j];
            if (c >= 'A' && c <= 'Z') {
                c += 32;
            }
            keybuf[j] = c;
        }
        mp_obj_t key = mp_obj_new_str(keybuf, use_kl);
        mp_obj_t val = mp_obj_new_str(line + eq + 1 + vs, vl);
        mp_obj_dict_store(cfg, key, val);
    }
}

/* load_config(config_path) -> dict */
static mp_obj_t ui_load_config(mp_obj_t path_obj) {
    mp_obj_t cfg = mp_const_none;
    bool success = false;
    for (int attempt = 0; attempt < 3 && !success; attempt++) {
        if (attempt > 0) {
            mp_hal_delay_ms(50);
        }
        vstr_t content;
        vstr_init(&content, 256);
        if (ui_try_read_whole_text(path_obj, &content)) {
            cfg = mp_obj_new_dict(0);
            ui_parse_ini_lines(content.buf, content.len, cfg);
            success = true;
        }
        vstr_clear(&content);
    }
    if (!success) {
        mp_printf(&mp_plat_print, "WARNING: could not read %s after 3 attempts -- using defaults\n",
                  mp_obj_str_get_str(path_obj));
        return mp_obj_new_dict(0);
    }
    return cfg;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_load_config_obj, ui_load_config);

#define UI_SAVE_CONFIG_MAX_KEYS 32

/* save_config(config_path, updates) -> None */
static mp_obj_t ui_save_config(mp_obj_t path_obj, mp_obj_t updates_obj) {
    vstr_t content;
    vstr_init(&content, 256);
    if (!ui_try_read_whole_text(path_obj, &content)) {
        content.len = 0; /* no existing file / read failed -- matches `lines = []` */
    }

    mp_obj_dict_t *updates = MP_OBJ_TO_PTR(updates_obj);
    mp_map_t *umap = &updates->map;
    mp_obj_t upd_keys[UI_SAVE_CONFIG_MAX_KEYS];
    mp_obj_t upd_vals[UI_SAVE_CONFIG_MAX_KEYS];
    bool upd_consumed[UI_SAVE_CONFIG_MAX_KEYS];
    size_t n_upd = 0;
    for (size_t pos = 0; pos < umap->alloc && n_upd < UI_SAVE_CONFIG_MAX_KEYS; pos++) {
        if (mp_map_slot_is_filled(umap, pos)) {
            upd_keys[n_upd] = umap->table[pos].key;
            upd_vals[n_upd] = umap->table[pos].value;
            upd_consumed[n_upd] = false;
            n_upd++;
        }
    }

    vstr_t out;
    vstr_init(&out, content.len + 64);
    mp_print_t print = { &out, (mp_print_strn_t)vstr_add_strn };

    size_t i = 0;
    while (i < content.len) {
        size_t line_start = i;
        while (i < content.len && content.buf[i] != '\n') {
            i++;
        }
        if (i < content.len) {
            i++;
        }
        size_t line_total_len = i - line_start; /* includes trailing \n if present */

        size_t s, l;
        ui_strip_trim(content.buf + line_start, line_total_len, &s, &l);
        const char *line = content.buf + line_start + s;

        bool replaced = false;
        if (l > 0 && line[0] != '#') {
            size_t eq = (size_t)-1;
            for (size_t j = 0; j < l; j++) {
                if (line[j] == '=') {
                    eq = j;
                    break;
                }
            }
            if (eq != (size_t)-1) {
                size_t ks, kl;
                ui_strip_trim(line, eq, &ks, &kl);
                char keybuf[64];
                size_t use_kl = kl >= sizeof(keybuf) ? sizeof(keybuf) - 1 : kl;
                for (size_t j = 0; j < use_kl; j++) {
                    char c = line[ks + j];
                    if (c >= 'A' && c <= 'Z') {
                        c += 32;
                    }
                    keybuf[j] = c;
                }
                for (size_t u = 0; u < n_upd; u++) {
                    if (upd_consumed[u]) {
                        continue;
                    }
                    size_t ulen;
                    const char *ukey = mp_obj_str_get_data(upd_keys[u], &ulen);
                    if (ulen == use_kl && memcmp(ukey, keybuf, use_kl) == 0) {
                        vstr_add_strn(&out, keybuf, use_kl);
                        vstr_add_char(&out, '=');
                        mp_obj_print_helper(&print, upd_vals[u], PRINT_STR);
                        vstr_add_char(&out, '\n');
                        upd_consumed[u] = true;
                        replaced = true;
                        break;
                    }
                }
            }
        }
        if (!replaced) {
            vstr_add_strn(&out, content.buf + line_start, line_total_len);
        }
    }

    for (size_t u = 0; u < n_upd; u++) {
        if (upd_consumed[u]) {
            continue;
        }
        size_t klen;
        const char *kdata = mp_obj_str_get_data(upd_keys[u], &klen);
        vstr_add_strn(&out, kdata, klen);
        vstr_add_char(&out, '=');
        mp_obj_print_helper(&print, upd_vals[u], PRINT_STR);
        vstr_add_char(&out, '\n');
    }
    vstr_clear(&content);

    mp_obj_t f = ui_open_file(path_obj, MP_QSTR_w);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        int errcode = 0;
        mp_stream_write_exactly(f, out.buf, out.len, &errcode);
        if (errcode != 0) {
            mp_raise_OSError(errcode);
        }
        nlr_pop();
    } else {
        mp_stream_close(f);
        vstr_clear(&out);
        nlr_jump(nlr.ret_val);
    }
    mp_stream_close(f);
    vstr_clear(&out);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ui_save_config_obj, ui_save_config);

/* ---- Cartridge loading ---- */

#define UI_CART_INRAM_MAX 0x8000
#define UI_PREFIX_BUF_SIZE 4096
#define UI_FLASH_CACHE_PATH "/megarom_cache.rom"
#define UI_FLASH_CACHE_META "/megarom_cache_src.txt"
#define UI_FLASH_COPY_CHUNK 4096

/* get_rom_load_buf() -> bytearray : zero-copy view of modmsx.c's shared
 * 4KB C-heap scratch buffer (msx_get_scratch_buf()) — same buffer
 * msx.get_scratch_view() hands out, just reached without a Python call.
 * Used below for the Mega ROM mapper-detection prefix read and the
 * flash-cache copy chunks; also called once by mp/main.py at boot as a
 * warm-up (see its own comment) to trigger the lazy malloc() while the
 * heap is freshest. */
static mp_obj_t ui_get_rom_load_buf(void) {
    return mp_obj_new_bytearray_by_ref(4096, msx_get_scratch_buf());
}
static MP_DEFINE_CONST_FUN_OBJ_0(ui_get_rom_load_buf_obj, ui_get_rom_load_buf);

/* True if UI_FLASH_CACHE_PATH already holds a copy of src_path/src_size. */
static bool ui_flash_cache_valid(mp_obj_t src_path_obj, mp_int_t src_size) {
    vstr_t content;
    vstr_init(&content, 64);
    bool read_ok = ui_try_read_whole_text(ui_str(UI_FLASH_CACHE_META), &content);
    if (!read_ok) {
        vstr_clear(&content);
        return false;
    }

    size_t i = 0;
    while (i < content.len && content.buf[i] != '\n') {
        i++;
    }
    size_t l1len = i;
    if (i < content.len) {
        i++;
    }
    size_t l2s = i;
    while (i < content.len && content.buf[i] != '\n') {
        i++;
    }
    size_t l2len = i - l2s;

    size_t ts, tl;
    ui_strip_trim(content.buf, l1len, &ts, &tl);
    size_t src_path_len;
    const char *src_path_s = mp_obj_str_get_data(src_path_obj, &src_path_len);
    bool path_match = (tl == src_path_len) && (tl == 0 || memcmp(content.buf + ts, src_path_s, tl) == 0);

    size_t ts2, tl2;
    ui_strip_trim(content.buf + l2s, l2len, &ts2, &tl2);
    mp_int_t saved_size = 0;
    bool size_ok = tl2 > 0;
    for (size_t j = 0; j < tl2 && size_ok; j++) {
        char c = content.buf[l2s + ts2 + j];
        if (c < '0' || c > '9') {
            size_ok = false;
            break;
        }
        saved_size = saved_size * 10 + (c - '0');
    }
    vstr_clear(&content);

    if (!path_match || !size_ok || saved_size != src_size) {
        return false;
    }

    bool stat_ok = false;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t st = mp_vfs_stat(ui_str(UI_FLASH_CACHE_PATH));
        size_t n_items;
        mp_obj_t *items;
        mp_obj_get_array(st, &n_items, &items);
        stat_ok = (n_items > 6) && (mp_obj_get_int(items[6]) == src_size);
        nlr_pop();
    }
    return stat_ok;
}

/* Stream src_path (on SD) into UI_FLASH_CACHE_PATH (onboard flash) in
 * small chunks via the shared scratch buffer, invalidating the meta
 * file BEFORE writing any data (so an interrupted copy's stale/
 * incomplete .rom never looks valid on the next boot) — same ordering
 * and rationale as msx_menu.py's _copy_to_flash_cache(). */
static void ui_copy_to_flash_cache(mp_obj_t src_path_obj, mp_int_t size) {
    ui_try_vfs_remove(ui_str(UI_FLASH_CACHE_META));

    uint8_t *buf = msx_get_scratch_buf(); /* may raise MemoryError -- propagates, matches the Python original */

    mp_obj_t src = ui_open_file(src_path_obj, MP_QSTR_rb);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t dst = ui_open_file(ui_str(UI_FLASH_CACHE_PATH), MP_QSTR_wb);
        nlr_buf_t nlr2;
        if (nlr_push(&nlr2) == 0) {
            mp_int_t remaining = size;
            while (remaining > 0) {
                mp_uint_t chunk = (mp_uint_t)(remaining > UI_FLASH_COPY_CHUNK ? UI_FLASH_COPY_CHUNK : remaining);
                int errcode = 0;
                mp_uint_t n = mp_stream_rw(src, buf, chunk, &errcode, MP_STREAM_RW_READ | MP_STREAM_RW_ONCE);
                if (errcode != 0) {
                    mp_raise_OSError(errcode);
                }
                if (n == 0) {
                    break;
                }
                mp_stream_write_exactly(dst, buf, n, &errcode);
                if (errcode != 0) {
                    mp_raise_OSError(errcode);
                }
                remaining -= (mp_int_t)n;
            }
            nlr_pop();
        } else {
            mp_stream_close(dst);
            mp_stream_close(src);
            nlr_jump(nlr2.ret_val);
        }
        mp_stream_close(dst);
        nlr_pop();
    } else {
        mp_stream_close(src);
        nlr_jump(nlr.ret_val);
    }
    mp_stream_close(src);

    mp_obj_t mf = ui_open_file(ui_str(UI_FLASH_CACHE_META), MP_QSTR_w);
    nlr_buf_t nlr3;
    if (nlr_push(&nlr3) == 0) {
        size_t path_len;
        const char *path_s = mp_obj_str_get_data(src_path_obj, &path_len);
        int errcode = 0;
        mp_stream_write_exactly(mf, path_s, path_len, &errcode);
        if (errcode == 0) {
            mp_stream_write_exactly(mf, "\n", 1, &errcode);
        }
        if (errcode == 0) {
            vstr_t sz;
            vstr_init(&sz, 16);
            vstr_printf(&sz, "%d", (int)size);
            mp_stream_write_exactly(mf, sz.buf, sz.len, &errcode);
            vstr_clear(&sz);
        }
        if (errcode == 0) {
            mp_stream_write_exactly(mf, "\n", 1, &errcode);
        }
        if (errcode != 0) {
            mp_raise_OSError(errcode);
        }
        nlr_pop();
    } else {
        mp_stream_close(mf);
        nlr_jump(nlr3.ret_val);
    }
    mp_stream_close(mf);
}

/* load_cart_smart(msx_module, slot, path) -> bool */
static mp_obj_t ui_load_cart_smart(mp_obj_t msx_module_obj, mp_obj_t slot_obj, mp_obj_t path_obj) {
    msx_state_t *msx = msx_get_state();
    mp_int_t slot = mp_obj_get_int(slot_obj);

    mp_obj_t stat_result = mp_vfs_stat(path_obj);
    size_t stat_len;
    mp_obj_t *stat_items;
    mp_obj_get_array(stat_result, &stat_len, &stat_items);
    mp_int_t size = mp_obj_get_int(stat_items[6]);

#ifdef MSX_BOARD_PIZERO
    /* 2026-10-05: Mega ROM (paged) loading disabled on pizero -- real-
     * hardware finding, see msx_menu.py's own long comment history for
     * the full writeup (core1/DVI crash, root cause not isolated). */
    if (size > UI_CART_INRAM_MAX) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("Mega ROM %dKB not supported on PiZero"), (int)(size / 1024));
    }
#endif

    if (size <= UI_CART_INRAM_MAX) {
        uint8_t *view = msx_cart_alloc(msx, (uint8_t)slot, (uint32_t)size);
        if (view == NULL) {
            mp_raise_msg_varg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("cart_alloc(%d bytes) failed (C heap exhausted/fragmented?)"), (int)size);
        }
        mp_obj_t f = ui_open_file(path_obj, MP_QSTR_rb);
        mp_uint_t n = 0;
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            n = ui_readinto_chunked_c(f, view, (mp_uint_t)size);
            nlr_pop();
        } else {
            mp_stream_close(f);
            nlr_jump(nlr.ret_val);
        }
        mp_stream_close(f);
        if ((mp_int_t)n != size) {
            mp_raise_msg_varg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("short read: got %d/%d bytes"), (int)n, (int)size);
        }
        return mp_obj_new_bool(msx_cart_finalize(msx, (uint8_t)slot, MSX_MAPPER_PLAIN));
    }

    if (!ui_flash_cache_valid(path_obj, size)) {
        ui_copy_to_flash_cache(path_obj, size);
    }

    mp_obj_t f = ui_open_file(ui_str(UI_FLASH_CACHE_PATH), MP_QSTR_rb);
    bool ok = false;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        uint8_t mapper = MSX_MAPPER_KONAMI;
        {
            nlr_buf_t nlr_mem;
            if (nlr_push(&nlr_mem) == 0) {
                uint8_t *buf = msx_get_scratch_buf();
                mp_uint_t prefix_len = (mp_uint_t)(size < UI_PREFIX_BUF_SIZE ? size : UI_PREFIX_BUF_SIZE);
                mp_uint_t n = ui_readinto_chunked_c(f, buf, prefix_len);
                uint8_t detected = msx_detect_mapper(buf, (uint32_t)n);
                if (detected != MSX_MAPPER_PLAIN) {
                    mapper = detected;
                }
                nlr_pop();
            } else {
                /* Only MemoryError is tolerated (keep the KONAMI
                 * fallback) -- matches `except MemoryError: pass`. */
                mp_obj_t exc = MP_OBJ_FROM_PTR(nlr_mem.ret_val);
                if (!mp_obj_exception_match(exc, MP_OBJ_FROM_PTR(&mp_type_MemoryError))) {
                    nlr_jump(nlr_mem.ret_val);
                }
            }
        }

        const char *mapper_name = "?";
        switch (mapper) {
            case MSX_MAPPER_PLAIN:   mapper_name = "PLAIN";   break;
            case MSX_MAPPER_ASCII8:  mapper_name = "ASCII8";  break;
            case MSX_MAPPER_ASCII16: mapper_name = "ASCII16"; break;
            case MSX_MAPPER_KONAMI:  mapper_name = "KONAMI";  break;
        }
        mp_printf(&mp_plat_print, "MAPPER DIAG: %s -> %s\n", mp_obj_str_get_str(path_obj), mapper_name);

        mp_obj_t call_args[4] = { slot_obj, f, mp_obj_new_int(size), mp_obj_new_int(mapper) };
        mp_obj_t result = ui_call_method_n(msx_module_obj, MP_QSTR_load_cart_paged, 4, call_args);
        ok = mp_obj_is_true(result);
        nlr_pop();
    } else {
        /* matches `except Exception: f.close(); raise` -- load_cart_paged()
         * only takes ownership of f on success, so any exception here
         * (including one load_cart_paged() itself raises) means we must
         * close it ourselves. */
        mp_stream_close(f);
        nlr_jump(nlr.ret_val);
    }
    if (!ok) {
        mp_stream_close(f);
    }
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_3(ui_load_cart_smart_obj, ui_load_cart_smart);

/* ---------------------------------------------------------------------
 * Module table
 * ------------------------------------------------------------------- */
static const mp_rom_map_elem_t msx_ui_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),          MP_ROM_QSTR(MP_QSTR_msx_ui) },

    { MP_ROM_QSTR(MP_QSTR_MenuCanvas),        MP_ROM_PTR(&menucanvas_type) },

    { MP_ROM_QSTR(MP_QSTR_C_BLACK),           MP_ROM_INT(UI_C_BLACK) },
    { MP_ROM_QSTR(MP_QSTR_C_WHITE),           MP_ROM_INT(UI_C_WHITE) },
    { MP_ROM_QSTR(MP_QSTR_C_GREEN),           MP_ROM_INT(UI_C_GREEN) },
    { MP_ROM_QSTR(MP_QSTR_C_YELLOW),          MP_ROM_INT(UI_C_YELLOW) },
    { MP_ROM_QSTR(MP_QSTR_C_GRAY),            MP_ROM_INT(UI_C_GRAY) },
    { MP_ROM_QSTR(MP_QSTR_C_CYAN),            MP_ROM_INT(UI_C_CYAN) },
    { MP_ROM_QSTR(MP_QSTR_C_RED),             MP_ROM_INT(UI_C_RED) },
    { MP_ROM_QSTR(MP_QSTR_rgb),               MP_ROM_PTR(&ui_rgb_obj) },

    { MP_ROM_QSTR(MP_QSTR_HID_UP),            MP_ROM_INT(UI_HID_UP) },
    { MP_ROM_QSTR(MP_QSTR_HID_DOWN),          MP_ROM_INT(UI_HID_DOWN) },
    { MP_ROM_QSTR(MP_QSTR_HID_LEFT),          MP_ROM_INT(UI_HID_LEFT) },
    { MP_ROM_QSTR(MP_QSTR_HID_RIGHT),         MP_ROM_INT(UI_HID_RIGHT) },
    { MP_ROM_QSTR(MP_QSTR_HID_ENTER),         MP_ROM_INT(UI_HID_ENTER) },
    { MP_ROM_QSTR(MP_QSTR_HID_ESC),           MP_ROM_INT(UI_HID_ESC) },

    { MP_ROM_QSTR(MP_QSTR_set_display_state), MP_ROM_PTR(&ui_set_display_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_hdmi_suspend),      MP_ROM_PTR(&ui_hdmi_suspend_obj) },
    { MP_ROM_QSTR(MP_QSTR_hdmi_resume),       MP_ROM_PTR(&ui_hdmi_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_lcd_suspend),       MP_ROM_PTR(&ui_lcd_suspend_obj) },
    { MP_ROM_QSTR(MP_QSTR_lcd_resume),        MP_ROM_PTR(&ui_lcd_resume_obj) },

    { MP_ROM_QSTR(MP_QSTR_get_key),           MP_ROM_PTR(&ui_get_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_key_release),  MP_ROM_PTR(&ui_wait_key_release_obj) },

    { MP_ROM_QSTR(MP_QSTR_echo_msg),          MP_ROM_PTR(&ui_echo_msg_obj) },
    { MP_ROM_QSTR(MP_QSTR_wrap_msg),          MP_ROM_PTR(&ui_wrap_msg_obj) },
    { MP_ROM_QSTR(MP_QSTR_draw_message),      MP_ROM_PTR(&ui_draw_message_obj) },

    { MP_ROM_QSTR(MP_QSTR_SAVE_HEADER_SIZE),   MP_ROM_INT(MSX_SAVE_HDR_SZ) },
    { MP_ROM_QSTR(MP_QSTR_MAX_SAVE_SLOTS),     MP_ROM_INT(UI_MAX_SAVE_SLOTS_DEFAULT) },
    { MP_ROM_QSTR(MP_QSTR_readinto_chunked),   MP_ROM_PTR(&ui_readinto_chunked_obj) },
    { MP_ROM_QSTR(MP_QSTR_write_chunked),      MP_ROM_PTR(&ui_write_chunked_obj) },
    { MP_ROM_QSTR(MP_QSTR_save_state_to),      MP_ROM_PTR(&ui_save_state_to_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_state_from),    MP_ROM_PTR(&ui_load_state_from_obj) },
    { MP_ROM_QSTR(MP_QSTR_save_slot_path),     MP_ROM_PTR(&ui_save_slot_path_obj) },
    { MP_ROM_QSTR(MP_QSTR_save_base_for_cart), MP_ROM_PTR(&ui_save_base_for_cart_obj) },
    { MP_ROM_QSTR(MP_QSTR_rotate_and_save_state), MP_ROM_PTR(&ui_rotate_and_save_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_list_save_slots),    MP_ROM_PTR(&ui_list_save_slots_obj) },

    { MP_ROM_QSTR(MP_QSTR_load_config),        MP_ROM_PTR(&ui_load_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_save_config),        MP_ROM_PTR(&ui_save_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_rom_load_buf),   MP_ROM_PTR(&ui_get_rom_load_buf_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_cart_smart),    MP_ROM_PTR(&ui_load_cart_smart_obj) },
};
static MP_DEFINE_CONST_DICT(msx_ui_module_globals, msx_ui_module_globals_table);

const mp_obj_module_t mp_module_msx_ui = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&msx_ui_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_msx_ui, mp_module_msx_ui);
