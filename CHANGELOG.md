# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

### Added

- **Z80 CALL/RST hook extension mechanism**: drop a module in `/sd/msx/ext/` or the onboard flash's `/ext/` and it's auto-loaded at boot (`mp/msx_ext.py`), no `main.py` edits required. Same design as the sibling PB-1000 emulator's loader.
- Debug helper APIs rounding out the hook mechanism: `msx.debug_set_cpu()` (write-back counterpart to `msx.debug_cpu()`, which now also returns `bc`/`de`/`hl`/`ix`/`iy`), `msx.debug_poke()`, and `msx.debug_get_slot()`/`msx.debug_set_slot()`. See `doc/extension_api.md` / `doc/extension_api_en.md`.
- Documented a real hardware-tested caveat: hooking an interrupt vector (e.g. `0x0038`) permanently disables interrupts, since the hook mechanism skips the handler's own `EI` along with everything else it replaces.

### Fixed

- **Runtime menu (GUI+F7) crash on real hardware** (`OSError: [Errno 5] EIO`): the main loop starts a frame's LCD DMA transfer and polls the keyboard while it's still in flight (deliberate pipelining for speed). If the menu hotkey landed in that window, the menu's own first draw would reconfigure and restart the same DMA channel/SPI peripheral without ever waiting for the previous transfer to finish — a race independent of the HDMI bridge, which earlier investigation had focused on. Fixed by draining any in-flight transfer (`msx.wait_display()`) before the menu can touch the shared SPI1 bus.
- **Cartridge-swap failures** (`cart_alloc() failed`) exposed once the crash above was fixed: `MICROPY_C_HEAP_SIZE`'s non-compacting allocator couldn't satisfy a 32KB allocation after freeing a smaller cart's block, even with nominal free space available elsewhere on the heap. `msx_cart_alloc()` now reuses one capacity-tracked buffer across cart swaps on the same slot instead of `free()`ing and `malloc()`ing on every load — no extra RAM budget needed (a bigger C heap, or a static buffer, was tried first and both shrank the separate MicroPython GC heap by the same amount, breaking boot-time module imports instead).
- **`config.txt` read failures silently falling back to defaults**: a transient SD read error while opening `config.txt` was indistinguishable from "no config file exists," producing a confusing "BIOS not found" error even when `bios=` was set correctly. `load_config()` now retries a few times and logs a warning if it still can't read the file.
- **SD read/write speed accidentally pinned at 400kHz**: the SD card driver's per-transfer baud rate was reusing the constant meant only for the initial low-speed handshake, so every block read/write ran at 400kHz regardless of the configured LCD/SPI speed. Split into a separate `SD_DATA_BAUD` (4MHz) — both a reliability and a throughput fix.
- **Menu crashes ending the game session**: the runtime menu now catches an unexpected exception, logs the full traceback to `/crashlog.txt` on internal flash (not SD — logging to the card that just failed would be circular), attempts to reinitialize the LCD if it was left frozen, and resumes gameplay instead of crashing the whole `main.py` loop.
- Menu status/error messages (e.g. a full `OSError` message) are now also printed to the serial console — the LCD's 31-character line was silently truncating them.
