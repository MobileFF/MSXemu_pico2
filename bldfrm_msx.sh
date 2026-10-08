#!/bin/bash
# MSX1 Emulator firmware build script — Raspberry Pi Pico 2 (RP2350, default)
# and Waveshare RP2350-PiZero (RP2350B, onboard DVI).
#
# Prerequisites:
#   MicroPython : ~/projects/micropython.msx   (github.com/micropython/micropython)
#   mpy-cross   : cd ~/projects/micropython.msx && make -C mpy-cross  (first time only)
#
# 2026-09-27: switched from the checkout shared with sibling projects
# (PB-1000 emulator, vgmplay-pico) at ~/projects/micropython to this
# project's own dedicated copy at ~/projects/micropython.msx, after a
# real incident where another session's `rm -rf build-RPI_PICO2` on the
# shared checkout wiped this project's CMakeCache (and, separately,
# this project's own fresh reconfigure raced with and clobbered that
# session's own in-progress build output right back). Each project
# having its own checkout removes this whole class of cross-session
# interference. The dedicated copy currently has no .git metadata (it
# was a plain file copy of the shared checkout, not a fresh clone) —
# fine for building, but `git pull`/submodule updates would need `git
# init` + remote/submodule setup redone first if MicroPython itself
# ever needs updating here.
#
# Usage:
#   chmod +x bldfrm_msx.sh   (see NOTE below — may not persist on this filesystem)
#   ./bldfrm_msx.sh              # Pico 2 (default)
#   ./bldfrm_msx.sh pico2        # Pico 2, explicit
#   ./bldfrm_msx.sh pizero       # Waveshare RP2350-PiZero
#
# NOTE: this file lives on a Google-Drive-mounted filesystem that has been
# observed to not persist chmod reliably — if `./bldfrm_msx.sh` reports
# "Permission denied", use `bash bldfrm_msx.sh [target]` instead.
#
# Output:
#   pico2 : ~/projects/micropython.msx/ports/rp2/build-RPI_PICO2/firmware.uf2
#           → firmware/firmware_msx.uf2
#   pizero: ~/projects/micropython.msx/ports/rp2/build-WAVESHARE_RP2350_PIZERO/firmware.uf2
#           → firmware/firmware_msx_pizero.uf2
#
# To flash: hold BOOTSEL, plug USB, copy the .uf2 to the RPI-RP2 drive.
#
# 2026-09-20, phase 2 of the RP2350-PiZero port (調査用/RP2350-PiZero_HDMI
#出力適用調査.md §7): added the target argument. The pizero target's board
# definition (src/msx/boards/WAVESHARE_RP2350_PIZERO/) is NOT yet real-
# hardware verified — see that directory's own comments.

set -e

TARGET="${1:-pico2}"
case "${TARGET}" in
    pico2)
        MP_BOARD="RPI_PICO2"
        BOARD_DIR_ARG=""
        UF2_NAME="firmware_msx.uf2"
        ;;
    pizero)
        MP_BOARD="WAVESHARE_RP2350_PIZERO"
        UF2_NAME="firmware_msx_pizero.uf2"
        ;;
    *)
        echo "ERROR: unknown target '${TARGET}' (expected: pico2, pizero)"
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Paths ─────────────────────────────────────────────────────────────────────
SRC_ORIG="${SCRIPT_DIR}/src"          # Google Drive source (authoritative)
DST_DIR="${HOME}/projects/msx_emu"    # Local copy (fast I/O for build)
MP_RPI_PORT="${HOME}/projects/micropython.msx/ports/rp2"
BUILD_DIR="${MP_RPI_PORT}/build-${MP_BOARD}"
MSX_MODULES="${DST_DIR}/src/msx/micropython_msx.cmake"
# pizero's board definition is not part of the MicroPython checkout (no
# official pico-sdk/MicroPython support for this board) — it ships in this
# project's own src/msx/boards/, copied alongside src/msx/ below, and
# pointed at via BOARD_DIR= instead of the stock boards/<NAME> lookup.
if [ "${TARGET}" = "pizero" ]; then
    BOARD_DIR_ARG="BOARD_DIR=${DST_DIR}/src/msx/boards/${MP_BOARD}"
fi

# 2026-09-21 TRIED AND REVERTED 2026-10-02: real-hardware bring-up found
# this project's builds relied entirely on mp/boot.py's os.dupterm(uart)
# call to get any UART REPL output at all (USB CDC and USB MSC were both
# compile-time disabled at the time — see USER_C_MODULES's own
# MICROPY_HW_USB_CDC/MSC=0). Forcing UART REPL on at the C level for
# pizero removed that single point of failure (UART always responding
# from boot, regardless of what mp/boot.py does or doesn't do) while that
# was pizero's only usable REPL channel at all.
#
# 2026-10-02: no longer needed, and removed at the user's explicit
# request — pizero's keyboard lives on a physically separate USB port
# (PIO-USB) from the board's native USB controller (BOOTSEL/programming
# port), which is now used for a native USB CDC REPL instead (see
# micropython_msx.cmake's MICROPY_HW_USB_CDC=1 for pizero, and
# mp/boot.py, which no longer sets up the UART dupterm for pizero either).
# USB CDC REPL is brought up at the same C/runtime level UART REPL was
# (before any Python boot.py code runs), so the original crash-safety
# property this flag existed for is preserved, just through the standard
# MicroPython mechanism instead of this board-specific one. GP0/1 are now
# free or whatever other purpose on pizero. pico2 was never affected
# either way (it already used UART REPL via boot.py's normal path, and
# still does).

# 2026-09-22: MSX_BOARD_PIZERO/PICO_PIO_USE_GPIO_BASE are ALSO set via
# add_compile_definitions() inside micropython_msx.cmake's MSX_IS_PIZERO
# block, but that alone is not enough — MicroPython's separate QSTR-
# extraction pre-pass (which scans a preprocessed dump of every source
# file for MP_QSTR_* identifiers to build the qstr table BEFORE the real
# compile) captures compile definitions at a point in rp2/CMakeLists.txt
# that runs before USER_C_MODULES is included, so add_compile_definitions
# from our own cmake file never reaches it — new DVI qstrs (e.g.
# MP_QSTR_init_display_hardware_dvi) then fail to compile with "undeclared
# here" even though the exact same define correctly reaches the real
# compile step. Real build failure hit 2026-09-22 (this bug was latent
# since phase 3 first landed: MSX_IS_PIZERO itself was ALSO never
# actually true before that same date — see micropython_msx.cmake's
# "BUG FIX" comment for that separate, compounding mistake). Passing
# these via CFLAGS_EXTRA reaches every compilation unit including the
# qstr pre-pass, same mechanism already proven for
# MICROPY_HW_ENABLE_UART_REPL above.
if [ "${TARGET}" = "pizero" ]; then
    export CFLAGS_EXTRA="${CFLAGS_EXTRA:-} -DMSX_BOARD_PIZERO=1 -DPICO_PIO_USE_GPIO_BASE=1"
fi

# 2026-09-21/22: same bring-up session — a stack-size increase was tried
# here at one point (ports/rp2/CMakeLists.txt hardcodes the RP2350 C
# stack via -Wl,--defsym=__micropy_extra_stack__=4096; PICO_STACK_SIZE is
# NOT consulted by memmap_mp_rp2350.ld at all, a first attempt via that
# route silently did nothing) while chasing a real-hardware bug: garbled
# UART output immediately after msx.init() returned inside run(),
# sometimes followed by a "Hard assert" panic. Stack size turned out to
# be a red herring — extensive bisection eventually isolated the actual
# cause to a single call, msx.boost_peri_clock() (reconfigures clk_peri,
# the UART's clock source, without draining/redoing in-flight UART
# output first) — see mp/main.py's _boost_clock_and_start_usb() comment,
# which skips that call on pizero. No stack-size patch is needed with
# that call skipped; keeping this comment (rather than deleting it
# silently) so a future "let's just bump the stack" attempt isn't
# re-tried blind — it was already tried, verified via nm to actually take
# effect, and did NOT fix the real bug.

# 2026-09-22: first real compile of libdvi (previously never actually
# built at all — see micropython_msx.cmake's "BUG FIX" comment) revealed
# a genuine linker region shortage: ports/rp2/memmap_mp_rp2350.ld defines
# SCRATCH_X(rwx) with LENGTH=0k, aliasing its ORIGIN with SCRATCH_Y's (both
# 0x20080000) — MicroPython doesn't use core1 by default, so it just folds
# the whole physical 8KB scratch X+Y bank into one region it calls
# SCRATCH_Y (which the C stack sits at the top of — see __StackTop's own
# comment above) and leaves SCRATCH_X as a zero-budget placeholder, only
# there so the linker script's `.stack1_dummy` section (unused core1
# stack) has somewhere to (trivially) go.
# libdvi's tmds_encode.S deliberately places its hot TMDS-encode loops in
# .scratch_x/.scratch_y sections (standard PicoDVI technique, for zero-
# wait-state execution) — only ~48 bytes actually get pulled in (the
# specific encode variant our code path uses). disp_dvi.c also launches
# core1 (multicore_launch_core1()) to run the DVI driver standalone —
# that hard_assert()s outright if PICO_CORE1_STACK_SIZE is 0 (the
# default, since MicroPython doesn't use core1 out of the box), and its
# stack (.stack1_dummy) also lives in SCRATCH_X. 2KB for SCRATCH_X
# (tmds_encode's 48 bytes + a small core1 stack for core1_dvi_main()'s
# fairly shallow call chain) is carved off the front of the shared 8KB
# bank, leaving SCRATCH_Y (and therefore __StackTop's core0 stack
# budget) at 6KB instead of 8KB.
#
# 2026-09-26 real-hardware finding — PICO_CORE1_STACK_SIZE must stay
# SMALL here, not just "big enough": pico_multicore's own
# multicore_launch_core1() (multicore.c) sets core1's initial SP to
# `&__StackOneBottom`, computed as `__StackOneTop - SIZEOF(.stack1_dummy)`
# — a formula that is only correct if `.stack1_dummy` (core1's real
# stack array) is the LAST thing placed in SCRATCH_X, ending exactly at
# __StackOneTop. Both this script's patched memmap_mp_rp2350.ld and
# stock pico-sdk's own memmap_default.ld place `.scratch_x` (tmds_encode
# .S's ~48 bytes) BEFORE `.stack1_dummy` in the same SCRATCH_X region, so
# the real array actually sits ~48-64 bytes above SCRATCH_X's origin —
# the formula's assumption is simply wrong in general, and only fails to
# matter in stock's 4KB-SCRATCH_X case because the resulting slop happens
# to work in the SAFE direction there (see below). With our 2KB
# SCRATCH_X and the previous PICO_CORE1_STACK_SIZE=0x600 (1536), the
# computed __StackOneBottom (0x800-0x600=0x200 relative) landed BELOW the
# real array's end (~0x30+0x600=0x630) — core1's SP started only ~464
# bytes above the array's real start, not 1536, and once dvi_scanline_
# pump()'s IRQ-context call depth exceeded that, the stack silently
# underflowed into the ~48-byte .scratch_x CODE region (tmds_encode.S's
# own hot loop!) sitting right below it — corrupting the very TMDS
# encoder mid-flight. Confirmed on real hardware: this produced a stable,
# non-recovering "snow" picture (DVI's own queue/IRQ bookkeeping stayed
# perfectly healthy throughout — msx.dvi_debug()'s heartbeat kept
# advancing at full rate and late_scanline_ctr stayed 0 — proving the
# fault was in TMDS-encoded *content*, not the pipeline, which pointed
# straight at the one piece of "content-producing" code sharing memory
# with something else: tmds_encode.S itself). The fix is to declare a
# SMALLER PICO_CORE1_STACK_SIZE: since the real array's start offset is
# fixed (~48-64 bytes, set by .scratch_x's own size) regardless of the
# declared stack size N, computed __StackOneBottom (=2KB-N) only clears
# the real array's end (~64+N) once N is kept safely below roughly half
# of SCRATCH_X's total size. 0x300 (768 bytes) leaves a comfortable ~450-
# byte margin (computed bottom 1280 vs. real end ~832) while still giving
# core1's trivial idle-loop-plus-one-IRQ-handler workload real usable
# depth of ~1200 bytes end to end — ample for dvi_scanline_pump()'s
# handful of local words (no large stack arrays; dvi_row_buf is static).
# Idempotent same as the __micropy_extra_stack__ patch above.
if [ "${TARGET}" = "pizero" ]; then
    RP2350_LD="${MP_RPI_PORT}/memmap_mp_rp2350.ld"
    if [ -f "${RP2350_LD}" ]; then
        sed -i -E \
            -e 's/(SCRATCH_X\(rwx\) : ORIGIN = 0x20080000, LENGTH = )[0-9]+k/\12k/' \
            -e 's/(SCRATCH_Y\(rwx\) : ORIGIN = )0x[0-9a-fA-F]+(, LENGTH = )[0-9]+k/\10x20080800\26k/' \
            "${RP2350_LD}"
        echo "[patch] ${RP2350_LD}: SCRATCH_X 0k->2k, SCRATCH_Y origin+0x800/8k->6k"
        # __StackOneTop/__StackOneBottom (pico_multicore's
        # multicore_launch_core1() hard_assert()s outright without them —
        # "undefined reference to __StackOneBottom" at link time) are
        # simply never defined in this MicroPython-customized linker
        # script at all (core1 unused by default). Stock pico-sdk's own
        # memmap (pico_crt0/rp2350/memmap_default.ld) defines them right
        # next to __StackTop/__StackBottom; insert the same pattern here
        # if not already present (idempotent).
        if ! grep -q '__StackOneTop' "${RP2350_LD}"; then
            sed -i '/__StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);/a\    __StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);\n    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);' "${RP2350_LD}"
            echo "[patch] ${RP2350_LD}: added __StackOneTop/__StackOneBottom"
        fi
        # 2026-09-26 real-hardware finding — __StackBottom is WRONG in this
        # file as shipped (`__StackBottom = __GcHeapEnd;`, a main-RAM
        # address), and it's what actually matters for core0's safety, not
        # just core1's. ports/rp2/main.c does
        # `mp_cstack_init_with_top(&__StackTop, &__StackTop - &__StackBottom)`
        # — i.e. it derives core0's C-stack-overflow-check SIZE from
        # `__StackTop - __StackBottom`. __StackTop correctly points at
        # SCRATCH_Y's real top, but __StackBottom (= __GcHeapEnd) points into
        # the *main RAM* region instead of SCRATCH_Y's real bottom — a
        # completely different, unrelated address. The subtraction still
        # "works" arithmetically (both are plain addresses) but yields a
        # SIZE roughly double the real one (~12KB computed vs. our actual
        # 6KB SCRATCH_Y, or ~12KB vs. stock's 8KB) — MicroPython's own
        # RecursionError safety net is checking against a limit that is
        # NOT the real physical stack size at all, so it never trips before
        # the *real* stack (in the physically separate SCRATCH_Y bank)
        # silently overflows into SCRATCH_X below it — smashing core1's
        # stack and/or tmds_encode.S's own code living there. Confirmed on
        # real hardware to be the actual root cause of a "snow" corruption
        # that appeared reproducibly after executing (almost) any REPL
        # statement (i.e. as soon as MicroPython's call depth on core0 got
        # deep enough — compiling+running a line, calling into one of this
        # project's C bindings, etc.) while core1's own DVI bookkeeping
        # (msx.dvi_debug()'s heartbeat/late_scanline_ctr) stayed perfectly
        # healthy throughout, and while leaving core0 completely idle for
        # 15+ seconds after DVI init never corrupted anything at all. Stock
        # pico2 builds carry the exact same wrong formula (unpatched by us)
        # but apparently never recurse deep enough in practice to hit the
        # gap between the fake ~12KB and the real 8KB — this project's
        # comparatively deep C-binding call chains on pizero do. Fix: make
        # __StackBottom point at SCRATCH_Y's real origin instead, so the
        # computed size always matches the real, physical stack region
        # exactly, regardless of how big/small SCRATCH_Y is configured —
        # this can only make MicroPython's overflow check MORE accurate
        # (a clean RecursionError instead of a silent stack-smash), never
        # less safe. Left un-reverted for pico2 for now (not yet validated
        # there) — see the pico2 branch below.
        if grep -q '__StackBottom = __GcHeapEnd;' "${RP2350_LD}"; then
            sed -i 's/__StackBottom = __GcHeapEnd;/__StackBottom = ORIGIN(SCRATCH_Y);/' "${RP2350_LD}"
            echo "[patch] ${RP2350_LD}: __StackBottom __GcHeapEnd -> ORIGIN(SCRATCH_Y) (real core0 stack bottom)"
        fi
    fi
    # .stack_dummy (a NOLOAD placeholder/sanity-reservation section, NOT
    # the real __StackTop/__StackBottom used to set the initial SP — see
    # memmap_mp_rp2350.ld) is sized from ports/rp2/CMakeLists.txt's
    # separately-hardcoded PICO_STACK_SIZE=0x2000 (8KB), which no longer
    # fits the 6KB SCRATCH_Y above. Shrink it to match (0x1800 = 6KB), and
    # give PICO_CORE1_STACK_SIZE (also hardcoded =0 right next to it) a
    # deliberately SMALL value — see the __StackOneBottom finding in the
    # SCRATCH_X comment above for why bigger is actually less safe here.
    RP2_CMAKELISTS="${MP_RPI_PORT}/CMakeLists.txt"
    if [ -f "${RP2_CMAKELISTS}" ]; then
        sed -i -E \
            -e 's/(PICO_STACK_SIZE=)(0x[0-9a-fA-F]+|[0-9]+)/\10x1800/' \
            -e 's/(PICO_CORE1_STACK_SIZE=)(0x[0-9a-fA-F]+|[0-9]+)/\10x300/' \
            "${RP2_CMAKELISTS}"
        echo "[patch] ${RP2_CMAKELISTS}: PICO_STACK_SIZE -> 0x1800 (6KB), PICO_CORE1_STACK_SIZE -> 0x300 (768B declared, ~1.2KB real usable — see SCRATCH_X comment)"
    fi
else
    # pico2 never needs any of the pizero-only patches above, and must
    # not silently inherit them from a previous pizero build sharing this
    # same vendored MicroPython checkout (all these sed patches mutate
    # ~/projects/micropython.msx in place, which persists across builds/
    # targets — this project's own dedicated checkout now, but pico2 and
    # pizero still share it with each other) — explicitly restore stock
    # values here so build order (pizero then pico2, or vice versa) never
    # changes pico2's output.
    RP2350_LD="${MP_RPI_PORT}/memmap_mp_rp2350.ld"
    if [ -f "${RP2350_LD}" ]; then
        sed -i -E \
            -e 's/(SCRATCH_X\(rwx\) : ORIGIN = 0x20080000, LENGTH = )[0-9]+k/\10k/' \
            -e 's/(SCRATCH_Y\(rwx\) : ORIGIN = )0x[0-9a-fA-F]+(, LENGTH = )[0-9]+k/\10x20080000\28k/' \
            "${RP2350_LD}"
        sed -i '/__StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);/d; /__StackOneBottom = __StackOneTop - SIZEOF(\.stack1_dummy);/d' "${RP2350_LD}"
        # Revert the __StackBottom fix too (see the pizero branch's long
        # comment) — pico2 keeps stock's original (buggy but unvalidated-
        # as-a-problem-here) formula until this fix is confirmed safe on
        # pizero hardware and deliberately extended to pico2 too.
        sed -i 's/__StackBottom = ORIGIN(SCRATCH_Y);/__StackBottom = __GcHeapEnd;/' "${RP2350_LD}"
    fi
    RP2_CMAKELISTS="${MP_RPI_PORT}/CMakeLists.txt"
    if [ -f "${RP2_CMAKELISTS}" ]; then
        sed -i -E \
            -e 's/(PICO_STACK_SIZE=)0x[0-9a-fA-F]+/\10x2000/' \
            -e 's/(PICO_CORE1_STACK_SIZE=)[0-9a-fA-Fx]+/\10/' \
            "${RP2_CMAKELISTS}"
        sed -i -E 's/(--defsym=__micropy_extra_stack__=)[0-9]+/\14096/' "${RP2_CMAKELISTS}"
    fi
fi

echo "=== MSX1 Emulator — Firmware Build (target: ${TARGET}) ==="
echo "Source  : ${SRC_ORIG}"
echo "Local   : ${DST_DIR}/src"
echo "Modules : ${MSX_MODULES}"
echo "Board   : ${MP_BOARD}"
echo "Build   : ${BUILD_DIR}"
echo ""

# ── Validate environment ───────────────────────────────────────────────────────
if [ ! -d "${MP_RPI_PORT}" ]; then
    echo "ERROR: MicroPython RP2 port not found at ${MP_RPI_PORT}"
    echo "  Clone MicroPython: cd ~/projects && git clone https://github.com/micropython/micropython.git"
    echo "  Then: cd micropython && make -C mpy-cross && git submodule update --init"
    exit 1
fi

# ── Copy source to local directory ────────────────────────────────────────────
echo "[copy] Copying source files to ${DST_DIR}/src …"
rm -rf "${DST_DIR}/src"
cp -r "${SRC_ORIG}" "${DST_DIR}/src"
echo "[copy] Done."
echo ""

# 2026-09-30 TRIED AND REVERTED: this block used to populate frozen_mp/
# for src/msx/boards/WAVESHARE_RP2350_PIZERO/manifest.py to freeze the
# lazily-imported menu modules (fixing a real-hardware DVI "No Signal"
# lockup) — reverted after it caused a separate, real-hardware-confirmed
# regression (intermittent mpremote/UART connection failures). See that
# board's mpconfigboard.cmake for the full writeup.

# ── Clean previous build (forces cmake re-configure with current modules) ─────
echo "[clean] Removing ${BUILD_DIR} …"
rm -rf "${BUILD_DIR}"

# ── Build ─────────────────────────────────────────────────────────────────────
echo "[make] Building firmware (this will run cmake automatically)…"
echo ""

# MICROPY_C_HEAP_SIZE 81920 (was 65536): 2026-09-20, raised to cover
# MSX_CART_VICTIM_SLOTS's shared Mega ROM cache pool (src/msx/msx_core.h,
# now 2 slots = +16KB on cart_cache[]) -- fixes a measured 0% Mega ROM
# bank-switch cache-hit rate far more cheaply than the first attempt (a
# dedicated 2nd resident page per *window*, +32KB/92160 total) did: that
# version's C-heap growth alone pushed *unrelated* lazy-module compiles
# (the runtime menu, see mp/msx_mode_switch.py) into real-hardware
# MemoryErrors, since it and the GC/Python heap share the same fixed
# 512KB SRAM. Went 65536 -> 73728 (1 victim slot) first, then -> 81920
# here (2 slots, real-hardware "still a bit slower than before" report)
# -- each keeps the same ~15.5KB C-heap headroom the original 65536/32KB-
# cache setup had (VDP ~16.3KB + PSG ~0.15KB + cache). See
# doc/memory_usage.md §3.1. This value is Pico2-tuned.
#
# 2026-09-22: pizero briefly used a smaller C heap (65536) here while
# disp_dvi.c still malloc()'d a persistent ~150KB DVI framebuffer (see
# its own PROVENANCE-style header comment, "No persistent DVI
# framebuffer") -- that buffer both starved the GC heap (a real-hardware
# MemoryError loading a cart: 36016 bytes GC-heap free reported, failed
# allocating just 4097 bytes, reproduced identically across a full power
# cycle) and never actually fit in the C heap at any size this project
# could afford anyway (150KB > any reasonable C heap on a 512KB chip
# already carrying a ~305KB msx_state_t). Redesigning disp_dvi.c to
# convert scanlines on the fly from msx->framebuf (no persistent buffer
# at all -- just a few KB of static round-robin row scratch, not part of
# the C-heap pool) removed that pressure entirely, so pizero used the
# same value as pico2 again... until Phase 4 (PIO-USB keyboard support,
# 調査用/RP2350-PiZero_USBキーボード対応調査.md) added enough new static
# RAM (the vendored pio_usb library + its pico-sdk dependencies) that
# 81920 left the GC heap under MicroPython's required 64KB floor on
# pizero specifically (a real link-time `assert(GcHeap is too small)`,
# not a guess).
#
# 2026-09-27: first tried closing that gap by shrinking pizero's C heap
# by 8KB (73728) -- this satisfied the link-time 64KB-floor *check*, but
# real hardware then failed `import main_msx` itself with a genuine
# MemoryError (allocating 1784 bytes) -- the static 64KB floor is a
# minimum for the linker to accept the build at all, not a guarantee
# that's enough for this specific program to actually run. Root cause
# turned out to be unrelated to the C heap at all: msx_core.c's HDMI-
# bridge feature (hdmi_frame_buf_pal4/hdmi_row_buf_raw332, ~24.25KB) was
# being compiled in unconditionally on every board, including pizero,
# where it's structurally unusable (onboard DVI instead, see
# disp_dvi.c) and therefore pure permanent waste. Excluded it for pizero
# (msx_core.c, #ifdef MSX_BOARD_PIZERO) instead, which recovers far more
# RAM than the 8KB C-heap cut ever did -- so pizero's C heap goes back to
# matching pico2's 81920 here, with real headroom to spare (unlike the
# 73728 attempt, which left the DVI TMDS buffers + Mega ROM cache's
# worst case uncomfortably tight against it).
MICROPY_C_HEAP_SIZE_VAL=81920
make -C "${MP_RPI_PORT}" \
     BOARD="${MP_BOARD}" \
     ${BOARD_DIR_ARG} \
     USER_C_MODULES="${MSX_MODULES}" \
     WERROR=0 \
     MICROPY_C_HEAP_SIZE=${MICROPY_C_HEAP_SIZE_VAL} \
     -j"$(nproc)" \
     2>&1 | tee /tmp/msx_build.log

# ── Report ────────────────────────────────────────────────────────────────────
UF2="${BUILD_DIR}/firmware.uf2"
OUT_DIR="${SCRIPT_DIR}/firmware"
echo ""
if [ -f "${UF2}" ]; then
    SIZE=$(du -h "${UF2}" | cut -f1)
    mkdir -p "${OUT_DIR}"
    # Distinct filename per target so pico2/pizero UF2s (and UF2s from other
    # Pico2 projects worked on in parallel on this machine) never collide.
    cp "${UF2}" "${OUT_DIR}/${UF2_NAME}"
    echo "=== Build SUCCESS (${TARGET}) ==="
    echo "Output : ${UF2}  (${SIZE})"
    echo "Copied : ${OUT_DIR}/${UF2_NAME}"
    echo ""
    echo "Flash  : hold BOOTSEL → plug USB → copy firmware/${UF2_NAME} to RPI-RP2 drive"
else
    echo "=== Build FAILED (${TARGET}) ==="
    echo "Log    : /tmp/msx_build.log"
    echo "Hint   : check for missing #include or undefined symbol above"
    exit 1
fi
