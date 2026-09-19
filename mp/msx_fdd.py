"""
msx_fdd.py — virtual FDD (floppy disk drive) support, 2026-09-14 hybrid

DSKIO/GETDPB run against a real WD1793/WD2793-compatible FDC register
emulation (src/msx/wd179x.c), memory-mapped into cart slot 1's page — a
real Disk ROM's own low-level driver code talks to FDC hardware
registers directly (confirmed by disassembly: a BUSY-bit poll against
what turned out to be memory-mapped FDC registers, not ROM data), so a
DSKIO-level hook can never intercept that; only emulating the actual
chip does. See src/msx/wd179x.h for the full rationale/scope.

DSKCHG is the one exception, hooked the old (BDOS-level CALL/RST hook)
way instead of left to the ROM's own real implementation. Real-hardware
testing (2026-09-14) traced this Disk ROM's own DSKCHG (National
CF-3300): it re-reads the boot sector via its own DSKIO and GETDPB calls
(GETDPB here just copies an 18-byte template from a fixed table indexed
by the media ID byte, not a live BPB computation) and then makes a
final pass/fail decision by comparing the drive number against a
register left over from GETDPB's internal media-ID arithmetic — most
likely two unrelated register lifetimes reused across a chain of PUSH/
POP calls rather than a deliberate check, and not something further
static disassembly could resolve with confidence. Confirmed via a real-
hardware trace that DSKIO/GETDPB themselves complete correctly (right
sectors, valid data) right up to that point, with Disk BASIC's FILES
then reporting "Disk offline" and never touching the disk again —
consistent with DSKCHG's own logic misfiring on an unrelated register
comparison rather than any real DSKIO/FDC problem. Hooking DSKCHG
specifically sidesteps that internal logic; DSKCHG's calling convention
itself (A=drive in; CY=0, A=1 changed once / A=0xFF not changed out) is
standardized regardless of this ROM's own implementation quirks.

msx.ini keys (see main.py):
    mode=disk
    diskrom=/sd/msx/DISK.rom
    disk=/sd/disk/disk1.dsk
    fdc_base=0x7FB8   (optional, decimal or 0x-hex — Z80 address of the
                        FDC's Status/Command register within the Disk
                        ROM's own page. Defaults to 0x7FB8, confirmed by
                        disassembly for the Disk ROM this was built
                        against. A different Disk ROM may memory-map its
                        FDC elsewhere — override this if so.)

Geometry (sectors/track, sides) is parsed from the mounted image's own
boot-sector BPB for msx.fdc_mount()'s sector-address math.

Verified on real hardware: Disk BASIC SAVE/LOAD and a full MSX-DOS boot
(MSXDOS.SYS + COMMAND.COM load, both disk sides).

First-pass scope/known gaps (see wd179x.h for the command-set limits):
  - Single drive, standard 3.5" DD (360KB/720KB) geometry only.
  - Swapping the mounted image (runtime menu's "Swap Disk") does not
    reset the machine — it relies on the DSKCHG hook above to let
    MSX-DOS notice the new image on its own.
"""

DEFAULT_FDC_BASE = 0x7FB8
# 2026-09-14: hooking only the public jump-table entry (0x4013) turned out
# to be a no-op on real hardware — an identical FDC command trace with and
# without the hook installed proved it never fired. Confirmed by
# disassembly: this Disk ROM's own internal cross-calls between its
# DSKIO/DSKCHG/GETDPB implementations bypass the jump table and CALL each
# other's real addresses directly (e.g. DSKCHG's own code does `CALL 7495`
# straight to DSKIO's implementation, not `CALL 4010H`) — the same reason
# the original DSKIO/DSKCHG/GETDPB hooks at 0x4010/13/16 never fired
# either, back when DSKIO was still hook-based. The MSX kernel likely
# resolves and caches this ROM's real entry addresses once (e.g. reading
# the jump table's operand bytes directly rather than ever executing a
# CALL to it) and calls the cached real address directly from then on,
# invisible to a hook at the public address. Hooking both here for
# robustness — whichever one actually gets CALLed will fire; the other is
# simply never triggered and costs nothing.
DSKCHG_ADDR = 0x4013
DSKCHG_REAL_ADDR = 0x784E  # this Disk ROM's actual DSKCHG implementation

CF = 0x01  # Carry flag bit within msx.debug_cpu()'s `f` byte (bit0, see
           # msx_debug_get_cpu() in msx_core.c — standard Z80 flag layout)

_msx = None
_disk_path = None
_fdc_base = DEFAULT_FDC_BASE  # remembers the boot-time msx.ini fdc_base=
                              # value (if any) so a later mount() call
                              # that omits it (e.g. the runtime menu's
                              # Swap Disk, which never knew it in the
                              # first place) still reuses the same Disk
                              # ROM's real register address instead of
                              # silently falling back to the default.
_disk_changed = True  # force an initial "changed" report so DOS reads fresh


def _read_geometry(path):
    """Parse sectors_per_track/num_sides from the disk image's own boot
    sector (BPB offsets 0x18-0x19 / 0x1A-0x1B — see the project's MSX
    Datapack research, 図3.15). Returns (0, 0) on any read failure;
    msx.fdc_mount() treats either 0 as "use the 9-sectors/2-sides
    standard-floppy default" — right for the overwhelming majority of
    MSX floppy images, so a bad/unreadable geometry read still lets
    mounting proceed rather than failing outright."""
    try:
        with open(path, 'rb') as f:
            boot = f.read(0x1C)
    except OSError:
        return 0, 0
    if len(boot) < 0x1C:
        return 0, 0
    sectors_per_track = boot[0x18] | (boot[0x19] << 8)
    num_sides = boot[0x1A] | (boot[0x1B] << 8)
    return sectors_per_track, num_sides


def mount(path, fdc_base=None):
    """Mount a .dsk image and enable the FDC — called at boot (msx.ini's
    disk= key) and from the runtime menu's Swap Disk. Replaces whatever
    was mounted before (msx.fdc_mount() closes the previous file itself).
    Opened 'r+b' (not 'rb') since Write Sector needs write access.
    fdc_base defaults to whatever was last used (see _fdc_base's comment)
    — the same Disk ROM stays loaded across a disk swap, only the image
    changes, so its real register address doesn't change either."""
    global _disk_path, _fdc_base, _disk_changed
    if fdc_base is None:
        fdc_base = _fdc_base
    else:
        _fdc_base = fdc_base
    sectors_per_track, num_sides = _read_geometry(path)
    f = _msx_open_rw(path)
    _msx.fdc_mount(f, fdc_base, sectors_per_track, num_sides)
    _disk_path = path
    _disk_changed = True


def _msx_open_rw(path):
    # Split out only so a real-hardware "can't open r+b" report has one
    # obvious place to look (e.g. a read-only SD card / write-protected
    # image) rather than being buried inside mount().
    return open(path, 'r+b')


def unmount():
    global _disk_path
    if _msx is not None:
        _msx.fdc_unmount()
    _disk_path = None


def _dskchg_hook():
    """DSKCHG — disk-change check (see module docstring for why this one
    routine is hooked instead of left to the real ROM implementation).
    In:  A=drive(0=A:).
    Out: CF=0, A=1 (changed, reported once per actual mount()/swap) or
         A=0xFF (not changed) / CF=1 error (A=2 not ready).
    """
    global _disk_changed
    msx = _msx
    pc, sp, a, f, bc, de, hl, ix, iy, cyc, halted, iff1, im = msx.debug_cpu()
    drive = a

    if drive != 0 or _disk_path is None:
        msx.debug_set_cpu(pc, sp, 2, (f | CF) & 0xFF, bc, de, hl, ix, iy)
        return

    result_a = 1 if _disk_changed else 0xFF
    _disk_changed = False
    msx.debug_set_cpu(pc, sp, result_a, f & ~CF & 0xFF, bc, de, hl, ix, iy)


def register(msx_module):
    """Call once at boot, before msx.reset(), only when msx.ini's
    mode=disk. Records the msx module reference for mount()/unmount()
    and installs the DSKCHG hook at both its public jump-table address
    and its real implementation address (see DSKCHG_REAL_ADDR's comment
    above) — DSKIO/GETDPB need no hook, they run for real against the
    FDC emulation."""
    global _msx
    _msx = msx_module
    msx_module.set_call_hook(DSKCHG_ADDR, _dskchg_hook)
    msx_module.set_call_hook(DSKCHG_REAL_ADDR, _dskchg_hook)
    print(f"msx_fdd: DSKCHG hooks installed at {DSKCHG_ADDR:04X} and {DSKCHG_REAL_ADDR:04X}")
