# Build Guide

## Prerequisites

### 1. Toolchain and Dependencies

#### Linux (Ubuntu/Debian) / WSL2 / ChromeOS Crostini

```bash
sudo apt update
sudo apt install -y cmake python3 python3-pip git build-essential gcc-arm-none-eabi libnewlib-arm-none-eabi
```

#### macOS

```bash
brew install cmake python3 git
brew install --cask gcc-arm-embedded
```

### 2. Clone MicroPython

```bash
mkdir -p ~/projects && cd ~/projects
git clone https://github.com/micropython/micropython.git
cd micropython
git submodule update --init
```

### 3. Build mpy-cross (once)

```bash
cd ~/projects/micropython
make -C mpy-cross
```

### 4. Pico SDK Setup

MicroPython's `ports/rp2` pulls in the Pico SDK as a submodule, so `git submodule update --init` above should already cover it. See [pico-sdk](https://github.com/raspberrypi/pico-sdk) if you need it standalone.

---

## Build Steps

`bldfrm_msx.sh` automates the whole build. Run it from the project root (`MSX_emu_pico2/`). There are three build targets:

```bash
./bldfrm_msx.sh                # Pico 2, native USB host (default)
./bldfrm_msx.sh pico2          # same, explicit
./bldfrm_msx.sh pico2_piousb   # Pico 2, PIO-USB host (GP4/GP5) + native USB CDC REPL
                                # — only for a board that has actually been wired with a
                                #   second USB connector on GP4(D+)/GP5(D-) via 22Ω series
                                #   resistors. Do NOT flash this onto an unwired board
                                #   (see the Hardware Guide for details)
./bldfrm_msx.sh pizero         # Waveshare RP2350-PiZero
```

The first time, you'll also need:

```bash
chmod +x bldfrm_msx.sh
```

### What the script does

1. Copies the `src/` folder (the Google Drive source, the authoritative copy) to `~/projects/msx_emu/src` (local disk, for build I/O speed)
2. Deletes the previous build directory (`~/projects/micropython/ports/rp2/build-RPI_PICO2`, or `build-WAVESHARE_RP2350_PIZERO` for pizero) to force cmake to reconfigure
3. Runs `make` with (the `pico2` default shown):

```bash
make -C ~/projects/micropython/ports/rp2 \
     BOARD=RPI_PICO2 \
     USER_C_MODULES=~/projects/msx_emu/src/msx/micropython_msx.cmake \
     WERROR=0 \
     MICROPY_C_HEAP_SIZE=81920 \
     -j$(nproc)
```

`pico2_piousb` additionally passes `CMAKE_ARGS="-DUSE_PIO_USB=ON"` (enabling `src/msx/micropython_msx.cmake`'s `option(USE_PIO_USB ...)`) and overrides `MICROPY_C_HEAP_SIZE` to `65536` (see below).

4. On success, copies `firmware.uf2` into the project's `firmware/<per-target name>.uf2` (the standing rule: always flash real hardware from this path):
   - `pico2` → `firmware/firmware_msx.uf2`
   - `pico2_piousb` → `firmware/firmware_msx_piousb.uf2`
   - `pizero` → `firmware/firmware_msx_pizero.uf2`

### About `MICROPY_C_HEAP_SIZE`

The MicroPython RP2 port's dedicated heap for C-level `malloc()`/`calloc()` (used by the VDP core, PSG core, and the Mega ROM page cache) **defaults to 0 bytes**. Without setting this explicitly, those `malloc()` calls silently corrupt MicroPython's GC heap region, causing unexplained hangs — a real bug hit earlier in this project.

- `pico2`/`pizero`: **81920 bytes (80KB)** — enough to comfortably cover the VDP core (~16.3KB) plus the Mega ROM page cache (32KB, `MSX_CART_VICTIM_SLOTS=0`) with room to spare. See [memory_usage.md](memory_usage.md) (Japanese only) for details.
- `pico2_piousb`: **65536 bytes (64KB)** — the PIO-USB library plus the native USB CDC stack it enables add ~15.8KB of new `.bss`, squeezing the GC heap, so the C heap is cut to compensate. This is not a new guess — it's the exact value this project used before the Mega ROM victim pool was ever added. Tuned against a real-hardware `MemoryError` compiling `msx_menu.py` on boot, so don't change it casually (see `bldfrm_msx.sh`'s own comment for the full story).

### Build Optimizations in `micropython_msx.cmake`

- The overall build defaults to `CMAKE_BUILD_TYPE=MinSizeRel` (`-Os`, prioritizing flash size), but `msx_core.c` / `z80.c` / `vrEmuTms9918.c` / `emu2149.c` (the emulation hot path) are individually compiled with `-O3` via `set_source_files_properties()`.
- `vrEmuTms9918.c`'s `PICO_BUILD` macro is explicitly defined, enabling that library's RAM-resident execution optimization (`__time_critical_func`), which was previously silently inert since the macro was never defined.

---

## Flashing

1. Hold the Pico 2's **BOOTSEL** button while plugging in the USB cable
2. A drive named `RP2350` (or `RPI-RP2`) appears on your PC
3. Copy `firmware/firmware_msx.uf2` onto that drive
4. The board automatically reboots into the new firmware

---

## SD Card Setup

```
/sd/msx/
  MSX_jp.rom       # 32KB Japanese MSX BIOS/BASIC ROM (or MSX_en.ROM, any filename)
  <cart name>.ROM  # cartridge ROM(s), optional, multiple allowed
  config.txt       # config file, optional
  save.bin         # save state (auto-created)
```

The BIOS ROM is not included in this repository for copyright reasons — provide your own and either point `config.txt`'s `bios=` at it or place it at the default path `/sd/msx/MSX.ROM`.

See `usage_guide_en.md` for `config.txt` details.

---

## Transferring Python Files to the Device

Use `mpremote` to copy the files under `mp/` onto the Pico's filesystem (root `/`).

```bash
mpremote connect /dev/ttyUSB0 fs cp mp/main.py :main.py
mpremote connect /dev/ttyUSB0 fs cp mp/msx_menu.py :msx_menu.py
mpremote connect /dev/ttyUSB0 fs cp mp/msx_keymap.py :msx_keymap.py
```

> The serial port name (`/dev/ttyUSB0`, etc.) varies by environment — check with `ls /dev/ttyUSB*`. It can change when you unplug/replug a USB-UART adapter.

After transferring, it's worth verifying the byte count matches (`mpremote fs cp` occasionally truncates a transfer):

```bash
mpremote connect /dev/ttyUSB0 exec "import os; print(os.stat('main.py')[6])"
```

Compare against the local file's size (e.g. `stat -c%s mp/main.py` on Linux).

---

## Running

```bash
mpremote connect /dev/ttyUSB0
>>> import main
>>> main.run()
```

The copy command above already deploys it as `main.py`, so it auto-runs on power-up. The REPL `import main; main.run()` shown above is just a development shortcut to re-run it without a full power cycle.

---

## Troubleshooting

| Symptom | Cause / fix |
| :--- | :--- |
| Build fails with a `USER_C_MODULES`-related error | Check the path to `src/msx/micropython_msx.cmake`. `bldfrm_msx.sh` copies the source locally every run, so make your edits against the Google Drive `src/` copy |
| Frequent `MemoryError` right from boot | Check that `MICROPY_C_HEAP_SIZE` is set (the default of 0 corrupts the GC heap) |
| Device hangs, REPL unresponsive | Try a physical power cycle (unplug/replug USB). `flash_nuke.uf2` is a last resort that wipes the entire filesystem (including save data — use with caution) |
| `mpremote` fails with "could not enter raw repl" | An infinite loop like `main.run()` may already be running. Send `machine.reset()` or physically reset the board |
| Keyboard doesn't respond | Check that `usb_host.start_bg_timer(8)` is called after `usb_host.init()` (required for the TinyUSB host stack's periodic servicing) |
| SPI/SD card is unstable | The LCD and SD card share the SPI1 bus. This is usually a wiring issue — see `hardware_guide_en.md` |

---

## Related Source Files

- `bldfrm_msx.sh` — the build script itself
- `src/msx/micropython_msx.cmake` — the CMake module definition (source file list, optimization flags, USB host core linkage)
