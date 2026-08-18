// Ensure TinyUSB/portable code uses the standard C allocator rather than
// any macro-based replacement provided by the MicroPython build.
// This file is force-included for every source in usb_host_core_lib.

#ifdef malloc
#undef malloc
#endif
#ifdef calloc
#undef calloc
#endif
#ifdef realloc
#undef realloc
#endif
#ifdef free
#undef free
#endif
