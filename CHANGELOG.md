# Changelog

All notable changes to this project are documented in this file. Japanese version: [CHANGELOG_ja.md](CHANGELOG_ja.md).

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

### Added (2026-09-04/05)

- **HDMI Display Settings menu (GUI+F7)**: LCD panel model, rotation, HDMI baud rate, and a new `boot_exclusive` option, editable from the runtime menu alongside the existing Audio/HDMI Settings screens. All four are restart-only (read once at boot by `msx.init_display_hardware()`/`msx.init_hdmi_output()`) — the screen says so, and ENTER writes them to `msx.ini` without applying them live.
- **`boot_exclusive` config option**: combined with `display=lcd` or `display=hdmi` (not `both`), skips the unused side's hardware initialization entirely at boot — `display=hdmi` skips the LCD panel reset/init sequence outright (real boot-time saving, and the LCD need not be physically present); `display=lcd` skips `msx.init_hdmi_output()` even if `hdmi=1`. Either can still be turned on live from the menu afterward.
- **ROM selector folder navigation**: `select_rom()` now browses from the SD card root (was hardcoded to `/sd/msx`) and can navigate into subfolders — UP/DOWN move, ENTER opens a folder or picks a `.ROM`, ESC goes up a level (or cancels at the root). Falls back to a recursive first-match search when there's no keyboard attached.
- **HDMI send is now DMA-driven** (`msx_render_to_hdmi()`): the whole frame is built into a static buffer, then sent as a single DMA transfer left in-flight (same pattern already used by the LCD path), instead of 192 separate blocking `spi_write_blocking()` calls. Real-hardware FPS with `hdmi=1` improved substantially over the old fully-blocking send (~40ms/frame at 10MHz).
- **hdmi_bridge_receiver**: the top-left reception-confirmation marker is now hidden (painted black) during actual gameplay frames and only shown for menu/UI screens (ROM selector, runtime menu) — uses the existing per-frame `bpp` header field (4=game, 8=menu) already sent by the sender, no protocol change needed.

### Changed (2026-09-04/05)

- **Config file renamed and relocated**: `/sd/msx/config.txt` → `/sd/msx.ini` (SD card root). Existing cards need their config file moved/renamed.
- **Default HDMI baud rate lowered 10MHz → 8MHz**: real-hardware testing found 10MHz reliably corrupts the received palette on this wiring (e.g. white renders as green) — a genuine electrical/timing margin limit, not a transport bug. 5MHz and 8MHz were both confirmed clean (8MHz slightly faster); see `doc/hdmi_bridge_phase2_report.md`.

### Fixed (2026-09-05)

- **`MemoryError` on boot** after the above features were added (`msx_menu.py` grew ~21% from the new ROM browser + Display Settings code, enough to fail compiling on-device on a fresh boot): the Display Settings screen and the ROM folder browser were split into their own modules (`msx_display_settings.py`, `msx_rom_browser.py`) and are now imported lazily — only compiled the first time they're actually used, not during the eager boot-time import chain. `msx_menu.py` itself is now smaller than it was before any of this session's changes.
- **ROM browser crash (`UnicodeError`)** when a folder under the SD root contains a filename that isn't valid UTF-8 (e.g. left over from a different OS/filesystem): `uos.ilistdir()` raises `UnicodeError` while decoding it, which wasn't caught alongside the existing `OSError` handling and crashed the whole browse. Now caught the same way — that one directory is treated as unreadable/skipped rather than crashing.

### Added (2026-09-05/06)

- **LCD backlight control** (`msx.set_backlight()`): turned off automatically whenever `display=hdmi` (the LCD panel is still initialized but never rendered to in that mode, so it used to sit lit showing a stale/frozen image) — re-applied live whenever `display` changes via the HDMI Settings menu.
- **HDMI receiver hardware-reset line**: a spare sender GPIO (`HDMI_RESET_PIN` = GP13) wired directly to the receiver Pico 2's RUN pin, pulsed low briefly before `init_hdmi_output()` and whenever HDMI is turned on live from the menu. Fixes a real-hardware issue where the receiver can come up in a bad state if it's powered on (or hot-plugged) while the HDMI cable is already connected (suspected TMDS backfeed disturbing power-on-reset timing). Idle Hi-Z, matching `hdmi_bridge_receiver`'s `notes/sender_reset_line.md` design (no receiver-side firmware change needed).
- **60fps frame pacing**: the HDMI DMA optimization above regularly pushes non-Mega-ROM carts past 70fps on real hardware, which made MSX-clock time (fixed per `run_frame()` call) run ahead of wall-clock time — games played fast-forward and PSG audio pitched up. Each main-loop iteration now sleeps off whatever's left of a 1/60s budget; iterations already at/below 60fps (slower carts, `display=both`, etc.) are unaffected.
- **Per-cartridge rotating save states**: Save/Load State (both the runtime menu and the F5/F8 hotkeys) now key off the currently-loaded cart's own path (`cart_path`) instead of one shared file — each cart's saves sit next to its ROM (e.g. `/sd/games/Foo.ROM` → `Foo.0.sav`, `Foo.1.sav`, …), and up to `msx_menu.MAX_SAVE_SLOTS` (10) past states are kept per cart, rotated on every save. The runtime menu's "Load State" now opens a slot picker (`msx_save_slots.py`, lazily imported) instead of always loading the single most recent file; the F5/F8 hotkeys still always act on slot 0 (most recent) for quick-save/quick-load convenience.
- **Swap Cartridge now opens in the current cart's own folder** (`start_dir`) instead of always starting back at the SD root — the user can still navigate up past it (`..`) to the true root.

### Fixed (2026-09-06)

- **Swap Cartridge ESC regression** introduced by the `start_dir` feature above: when browsing opens below the SD root, a single ESC press used to walk up one folder level instead of cancelling (cancelling needed one ESC per level, back down to the root). ESC now always cancels immediately, regardless of depth — navigate up a level via `".."` + ENTER instead.
- **Runtime menu becoming permanently inaccessible after one `OSError(5,)` (EIO)**: `sdcard.py`'s `readblocks()`/`writeblocks()` raised `OSError(5)` straight from the first failed SD command response (a known real-hardware timing-margin issue on this shared SPI bus — see `lcd_suspend()`'s comment in `msx_menu.py`), which MicroPython's VFS layer then treated as fatal for the whole card: every subsequent SD access for the rest of the session failed the same way, even though the next SPI transaction on its own usually would have succeeded. Both methods now retry up to 3 times (5ms apart, re-issuing `spi.init()` each attempt) before giving up — turns a one-off marginal bus glitch back into a transient hiccup instead of a session-ending failure.
