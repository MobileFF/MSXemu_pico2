#include "py/mphal.h"
#include "py/runtime.h"
#include "usb_host_core.h"

// low‑level print helper (no buffering)
#define TRACE(str) mp_hal_stdout_tx_str(str "\n")

// Python API: usb_host.init()
static mp_obj_t mod_usb_host_init(size_t n_args, const mp_obj_t *args) {
  (void)n_args;
  (void)args;
//  TRACE("[USB Host] wrapper: before core_init");
//  mp_printf(&mp_plat_print, "[USB Host] wrapper: before core_init\n");
#ifdef DEBUG_SKIP_CORE_INIT
  TRACE("[USB Host] skipping core_init\n");
  mp_printf(&mp_plat_print, "[USB Host] skipping core_init\n");
#else
  usb_host_core_init();
#endif
  //  TRACE("[USB Host] wrapper: after core_init");
  //  mp_printf(&mp_plat_print, "[USB Host] wrapper: after core_init\n");
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_usb_host_init_obj, 0, 1,
                                           mod_usb_host_init);

// Python API: usb_host.probe() - simple call to test module linkage
static mp_obj_t mod_usb_host_probe(void) {
  TRACE("[USB Host] probe called");
  mp_printf(&mp_plat_print, "[USB Host] probe called\n");
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_usb_host_probe_obj, mod_usb_host_probe);

// Python API: usb_host.task()
// Should be called frequently in the main loop
static mp_obj_t mod_usb_host_task(void) {
  usb_host_core_task();
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_usb_host_task_obj, mod_usb_host_task);

// Python API: usb_host.start_bg_timer(interval_ms=8)
static mp_obj_t mod_usb_host_start_bg_timer(size_t n_args, const mp_obj_t *args) {
  int interval = (n_args > 0) ? mp_obj_get_int(args[0]) : 8;
  usb_host_core_start_bg_timer(interval);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_usb_host_start_bg_timer_obj,
                                            0, 1, mod_usb_host_start_bg_timer);

// Python API: usb_host.stop_bg_timer()
static mp_obj_t mod_usb_host_stop_bg_timer(void) {
  usb_host_core_stop_bg_timer();
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_usb_host_stop_bg_timer_obj,
                                  mod_usb_host_stop_bg_timer);

// Python API: usb_host.get_hid_report() -> bytes
// Returns the last received 8-byte USB HID keyboard report:
//   byte 0: modifier mask (bit0=LCtrl … bit7=RGui)
//   byte 1: reserved (always 0)
//   bytes 2-7: up to 6 simultaneous keycodes (HID Usage IDs)
static mp_obj_t mod_usb_host_get_hid_report(void) {
  const uint8_t *r = usb_host_core_get_hid_report();
  return mp_obj_new_bytes(r, 8);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_usb_host_get_hid_report_obj,
                                  mod_usb_host_get_hid_report);


static const mp_rom_map_elem_t usb_host_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_usb_host)},
    {MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&mod_usb_host_init_obj)},
    {MP_ROM_QSTR(MP_QSTR_probe), MP_ROM_PTR(&mod_usb_host_probe_obj)},
    {MP_ROM_QSTR(MP_QSTR_task), MP_ROM_PTR(&mod_usb_host_task_obj)},
    {MP_ROM_QSTR(MP_QSTR_start_bg_timer),
     MP_ROM_PTR(&mod_usb_host_start_bg_timer_obj)},
    {MP_ROM_QSTR(MP_QSTR_stop_bg_timer),
     MP_ROM_PTR(&mod_usb_host_stop_bg_timer_obj)},
    {MP_ROM_QSTR(MP_QSTR_get_hid_report),
     MP_ROM_PTR(&mod_usb_host_get_hid_report_obj)},
};
static MP_DEFINE_CONST_DICT(usb_host_module_globals,
                            usb_host_module_globals_table);

const mp_obj_module_t mp_module_usb_host = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&usb_host_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_usb_host, mp_module_usb_host);
