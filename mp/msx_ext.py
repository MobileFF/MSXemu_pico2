"""
msx_ext.py — startup extension auto-loader ("plugin" mechanism)

Scans /sd/msx/ext/ and /ext/ for extension modules and imports each one
that's found; if the module defines register(msx), it's called
automatically. Extensions use this to register their own CALL/RST hooks
via msx.set_call_hook() (see doc/extension_api.md) without main.py itself
needing to know anything about them.

Same design, same priority rule, and (deliberately) most of the same code
as this project's sibling PB-1000 emulator's _ext_load_modules(): a module
present in both directories is loaded only from /sd/msx/ext/ (SD card
takes priority over onboard flash); other than that, PB-1000 also has a
per-"RAM profile" ext/ tier ahead of both, which doesn't apply here since
this emulator has no equivalent concept of a profile.

Writing an extension (/sd/msx/ext/myext.py):

    CALL_ADDR = 0xC100   # whatever BIOS/game address you want to replace

    def register(msx):
        def _hook():
            pc, sp, a, f, bc, de, hl, ix, iy, cyc, halted, iff1, im = msx.debug_cpu()
            ...
        msx.set_call_hook(CALL_ADDR, _hook)
        print("myext: registered")
"""
import os, sys, gc

EXT_DIRS = ["/sd/msx/ext", "/ext"]


def load_extensions(msx):
    mod_sources = {}  # mod_name -> ext_dir (first one found wins = higher priority)

    # Insert directories into sys.path in reverse priority order — each
    # insert(0, ...) pushes the previous ones back, so the last one
    # inserted (highest priority) ends up at sys.path[0]. This makes
    # __import__()'s own resolution naturally prefer the higher-priority
    # directory when the same module name exists in both.
    for ext_dir in reversed(EXT_DIRS):
        if ext_dir not in sys.path:
            sys.path.insert(0, ext_dir)

    for ext_dir in EXT_DIRS:
        try:
            files = os.listdir(ext_dir)
        except OSError:
            continue
        for fname in sorted(files):
            if fname.startswith("_"):
                continue
            if fname.endswith(".py"):
                mod_name = fname[:-3]
            elif fname.endswith(".mpy"):
                mod_name = fname[:-4]
            else:
                continue
            if mod_name not in mod_sources:
                mod_sources[mod_name] = ext_dir

    for mod_name in sorted(mod_sources):
        ext_dir = mod_sources[mod_name]
        try:
            # Importing several extensions back-to-back without collecting
            # in between: compiling each one's source needs a transient
            # contiguous allocation, and heap fragmentation left over from
            # an earlier import can make a later, smaller one fail with
            # MemoryError even though total free memory looks sufficient —
            # the same GC-heap-fragmentation issue this project hit (and
            # fixed) for BIOS/cartridge loading, see doc/sdcard_spi_mode_bug.md
            # neighbourhood in project history for the general shape of it.
            gc.collect()
            mod = __import__(mod_name)
            if hasattr(mod, "register"):
                mod.register(msx)
                print(f"EXT: loaded {mod_name} from {ext_dir}")
            else:
                print(f"EXT: {mod_name} has no register(), skipped")
        except Exception as e:
            print(f"EXT: {mod_name} load error: {e}")
            sys.print_exception(e)
