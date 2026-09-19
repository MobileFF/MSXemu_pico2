#!/usr/bin/env python3
"""
tools/dsk_manager.py — GUI manager for MSX .dsk (floppy image) files.

Runs on your PC (Windows/Mac/Linux) — NOT on the Pico — to prepare disk
images for this project's virtual FDD (mp/msx_fdd.py, msx.ini's mode=disk).
It's a thin GUI wrapper around the `mtools` command-line suite (mdir/
mcopy/mdel/mmd/mrd/mren/mformat), not a from-scratch FAT12 reader/writer:
mtools has ~30 years of real-world FAT12 handling behind it, whereas a
hand-rolled implementation here risks the exact same "one wrong byte
silently corrupts your disk image" failure mode already flagged for this
project's own DSKIO/GETDPB hook implementation (see msx_fdd.py's
docstring). Every actual read/write of the .dsk file happens inside
mtools; this script only builds command lines, runs them, and parses
`mdir`'s output for display.

Requirements:
  - Python 3 with tkinter (bundled with the python.org installers on
    Windows/Mac; on Debian/Ubuntu Linux, `apt install python3-tk` if
    `import tkinter` fails)
  - mtools on PATH: Linux `apt install mtools` / Mac `brew install
    mtools` / Windows: via WSL, MSYS2, or a native build — make sure
    mdir.exe etc. end up on PATH either way.

Usage:
    python3 tools/dsk_manager.py [image.dsk]

Two standard MSX floppy sizes are offered when creating a new image —
360KB and 720KB (see mp/main.py's msx.ini docs) — mtools' `-f` presets
already know both; the emulator's FDC (src/msx/wd179x.c) reads the
sector-size/cluster-size/FAT-layout fields straight out of the BPB
mtools writes, matching standard MSX formatting either way. New Image
also sanitizes the boot sector (see sanitize_boot_sector() below) so
images this tool creates work with Disk ROMs that execute it as part of
their own startup check, not just ones that don't bother, and marks
clusters 2-3 as bad (see reserve_bad_clusters() below) to route real
files around a real-hardware data-loss bug in that specific location.
"""
import os
import re
import shutil
import subprocess
import sys
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog

MTOOLS_TOOLS = ("mdir", "mcopy", "mdel", "mmd", "mrd", "mformat", "mren")

# (label, size_kb) — matches mtools' own -f preset names.
IMAGE_PRESETS = [("720KB (MSX 3.5\", standard)", 720),
                 ("360KB (MSX 3.5\", SS/DD)", 360)]

# 2026-09-14/15: mformat writes a generic PC/BIOS-oriented x86 bootstrap
# stub into the boot sector (INT 13h/INT 19h calls, meaningless and
# dangerous if ever read as Z80 instructions) starting after the
# standard + extended BPB fields, plus the PC boot signature (0x55 0xAA)
# at the very end. Real-hardware testing against a National CF-3300's
# Disk ROM found it actually fetches and, depending on the boot sector's
# own header, executes this as part of its own boot-time drive
# verification (most MSX-DOS-in-ROM systems don't bother — MSX-DOS
# itself already lives in the Disk ROM, not on the floppy — but this one
# does): it sent the emulator into an unrecoverable retry loop with
# nothing ever displayed on screen.
#
# The actual root cause, per the MSX Datapack's own documented boot
# sequence (調査用/3章 MSX-DOSの構造 — 3.1 MSX-DOSの起動, steps 4/5/7 —
# not any third-party OS's source, to keep this MIT-licensed project
# clear of Nextor's non-commercial/no-derivative-works license):
#
#   4. The boot sector is loaded to C000h-C0FFh. If its very first byte
#      is anything other than 0xFB or 0xE9, the system boots straight to
#      Disk BASIC instead of running any of the sector's own code.
#      mformat writes 0xEB (the PC-style "JMP SHORT" opcode) there,
#      which is neither — on this Disk ROM that turned out to trigger
#      the retry loop above rather than the documented clean Disk BASIC
#      fallback.
#   5/7. C01Eh (i.e. boot-sector offset 0x1E) is CALLed twice: once with
#      CY reset — the convention is a lone "RET NC" there, a no-op given
#      CY=0 — and again, after MSX-DOS's own environment is initialized,
#      with CY set, where a "RET NC" instead falls through into whatever
#      real bootstrap code follows it.
#
# Since this tool only ever creates blank data disks (no MSXDOS.SYS to
# chain-load), the minimal, always-safe implementation of that contract
# is a single unconditional RET (0xC9) at offset 0x1E: it returns
# immediately either way, instead of falling through into a sea of
# zero-filled "code" (0x00 is NOP) that would run off the end of the
# sector into whoever's memory follows.
#
# BOOT_JMP_OFFSET/BOOT_ENTRY_OFFSET are the only two bytes this touches;
# everything else mformat writes (BPB, OEM name, the informational
# extended-BPB fields, the leftover PC bootstrap bytes) is left alone —
# none of it is read as code once these two bytes are in place, and the
# BPB fields in particular are needed as-is for correct geometry.
BOOT_JMP_OFFSET = 0x00
BOOT_JMP_BYTE = 0xE9      # MSX boot-sector validity marker (Datapack 3.1 step 4)
BOOT_ENTRY_OFFSET = 0x1E  # C01Eh entry point (Datapack 3.1 step 5/7)
BOOT_ENTRY_BYTE = 0xC9    # Z80 RET — safe no-op on both the CY=0 and CY=1 calls
SECTOR_SIZE = 512


def sanitize_boot_sector(path):
    """Patch path's boot sector so MSX Disk ROMs that execute it as part
    of their own startup check (see the comment above) treat it as a
    valid, harmless, empty boot sector rather than looping or crashing:
    sets byte 0 to the MSX-DOS boot-sector validity marker (0xE9) and
    byte 0x1E (the C01Eh entry point) to a bare RET. Called automatically
    right after mformat when creating a new image (see NewImageDialog's
    caller below); also reachable on an existing image via the main
    window's "Fix Boot Sector" button, for images already created before
    this existed."""
    with open(path, 'r+b') as f:
        f.seek(BOOT_JMP_OFFSET)
        f.write(bytes([BOOT_JMP_BYTE]))
        f.seek(BOOT_ENTRY_OFFSET)
        f.write(bytes([BOOT_ENTRY_BYTE]))


# 2026-09-15: real-hardware testing (National CF-3300 Disk ROM, this
# project's own FDC emulation, src/msx/wd179x.c) found that saving a
# small file lands its data on clusters 2-3 — the *first* clusters any
# freshly-formatted disk ever hands out — and that data is silently lost:
# it gets written to the wrong physical sector (colliding with the tail
# of the second FAT copy and the start of the root directory) and is
# then overwritten by the next, correct FAT/directory flush. Confirmed
# to reproduce identically on a 720KB image made by WebMSX (an unrelated
# MSX emulator), not just images this tool produces — so whatever's
# wrong isn't specific to this tool's own boot-sector patching. What's
# NOT yet established is which *side* of the emulation boundary the bug
# actually lives on: this Disk ROM's own low-level sector-addressing
# code (which this project only emulates, doesn't control), or this
# project's own wd179x.c/msx_fdd.py side-select handling for the track
# that clusters 2-3 happen to fall on for a standard 720KB layout (the
# only side/track combination no test had ever exercised before this).
# Real WD179x-based controllers/DOS *do* legitimately support marking
# bad clusters (FAT12 value 0xFF7) so a formatting tool can route around
# a media defect — reusing exactly that mechanism here sidesteps the bug
# regardless of which side of the boundary eventually turns out to own
# it, at the cost of 2 clusters (a little under 2KB) of usable space per
# image. This does NOT reclassify the root cause as "just a ROM quirk,
# nothing to fix here" — it is purely a practical workaround; see
# wd179x.c/msx_fdd.py for anyone continuing that investigation.
RESERVED_BAD_CLUSTERS = (2, 3)
FAT12_BAD_CLUSTER = 0xFF7


def _get_fat12_entry(fat, cluster):
    off = cluster * 3 // 2
    if cluster % 2 == 0:
        return fat[off] | ((fat[off + 1] & 0x0F) << 8)
    else:
        return (fat[off] >> 4) | (fat[off + 1] << 4)


def _set_fat12_entry(fat, cluster, value):
    """Pack `value` (12 bits) into `cluster`'s entry within `fat`
    (bytearray of one whole FAT copy), 2 entries per 3 bytes, exactly as
    described in the MSX Datapack (図3.17/3.18 FATの実例/読み方)."""
    off = cluster * 3 // 2
    if cluster % 2 == 0:
        fat[off] = value & 0xFF
        fat[off + 1] = (fat[off + 1] & 0xF0) | ((value >> 8) & 0x0F)
    else:
        fat[off] = (fat[off] & 0x0F) | ((value & 0x0F) << 4)
        fat[off + 1] = (value >> 4) & 0xFF


def reserve_bad_clusters(path, clusters=RESERVED_BAD_CLUSTERS):
    """Mark `clusters` as permanently bad (FAT12 value 0xFF7) in every
    FAT copy on path — see RESERVED_BAD_CLUSTERS' comment above for why.
    Intended for a freshly-formatted (all-clusters-free) image — called
    right after mformat, before anything is ever copied onto it — but
    skips any cluster that's already non-free (checked against the
    first FAT copy) rather than blindly overwriting it, so calling this
    again on an image that already has real files is a safe no-op for
    those clusters instead of corrupting their chain. Reads the FAT
    layout (reserved sectors, FAT count/size, bytes/sector) from path's
    own boot-sector BPB rather than assuming mformat's defaults, so it
    works for any size preset this tool offers."""
    with open(path, 'r+b') as f:
        boot = f.read(512)
        bps      = boot[0x0B] | (boot[0x0C] << 8)
        reserved = boot[0x0E] | (boot[0x0F] << 8)
        nfat     = boot[0x10]
        fatsize  = boot[0x16] | (boot[0x17] << 8)

        first_fat_off = reserved * bps
        f.seek(first_fat_off)
        first_fat = bytearray(f.read(fatsize * bps))
        to_reserve = [c for c in clusters
                      if _get_fat12_entry(first_fat, c) == 0]

        for copy in range(nfat):
            fat_off = (reserved + copy * fatsize) * bps
            f.seek(fat_off)
            fat = bytearray(f.read(fatsize * bps))
            for c in to_reserve:
                _set_fat12_entry(fat, c, FAT12_BAD_CLUSTER)
            f.seek(fat_off)
            f.write(fat)


class MtoolsError(RuntimeError):
    """Raised with mtools' own stderr/stdout text — surfaced verbatim in
    the GUI rather than reinterpreted, so a real mtools error message
    (e.g. "Directory ::/SUBDIR non empty") reaches the user unchanged."""


def find_missing_tools():
    """[] if every required mtools executable is on PATH, else the
    missing names — checked once at startup so a clear error shows before
    the first actual command fails confusingly."""
    return [t for t in MTOOLS_TOOLS if shutil.which(t) is None]


def run(args):
    """Run one mtools command. Never touches the .dsk file directly
    ourselves — see module docstring."""
    try:
        proc = subprocess.run(args, capture_output=True, text=True)
    except FileNotFoundError:
        raise MtoolsError(f"'{args[0]}' not found on PATH — is mtools installed?")
    if proc.returncode != 0:
        msg = (proc.stderr or proc.stdout or "").strip()
        raise MtoolsError(msg or f"{args[0]} exited with code {proc.returncode}")
    return proc.stdout


_DATE_RE = re.compile(r'^\d{4}-\d{2}-\d{2}$')
_TIME_RE = re.compile(r'^\d{1,2}:\d{2}$')


def parse_mdir(output):
    """Parse `mdir`'s human-readable listing into a list of
    {'name', 'is_dir', 'size', 'date', 'time'} dicts.

    mtools has no machine-readable listing mode that also carries
    size/date/is-dir (only `-b`, which drops everything but the name), so
    this parses the normal columnar output. Deliberately tolerant: any
    line that doesn't look exactly like a directory-entry row (the volume/
    header lines, the blank separator, the "N files"/"NNN bytes free"
    summary) is silently skipped rather than raising — worst case a stray
    line just doesn't show up in the list; it never causes a wrong read of
    real entries. '.' and '..' are dropped (Python-side navigation via the
    Up button already covers that, and it avoids double-entries)."""
    entries = []
    for line in output.splitlines():
        tokens = line.split()
        if len(tokens) < 4:
            continue
        if not _TIME_RE.match(tokens[-1]) or not _DATE_RE.match(tokens[-2]):
            continue
        size_tok = tokens[-3]
        name_tokens = tokens[:-3]
        # A file's 8.3 name prints as two space-separated tokens (base,
        # ext) even with no visible dot; a directory or an extension-less
        # file prints as just one. Never more than two — FAT 8.3 names
        # can't contain a space.
        if not (1 <= len(name_tokens) <= 2):
            continue
        is_dir = (size_tok == '<DIR>')
        if not is_dir and not size_tok.isdigit():
            continue
        name = '.'.join(name_tokens) if len(name_tokens) == 2 else name_tokens[0]
        if name in ('.', '..'):
            continue
        entries.append({'name': name, 'is_dir': is_dir,
                        'size': 0 if is_dir else int(size_tok),
                        'date': tokens[-2], 'time': tokens[-1]})
    return entries


def mpath(components, name=None):
    """Build an mtools '::'-relative path for `name` inside the directory
    given by `components` (a list of path parts, [] = image root)."""
    parts = list(components)
    if name is not None:
        parts.append(name)
    return "::" + "/".join(parts)


class DskManagerApp(tk.Tk):
    def __init__(self, initial_image=None):
        super().__init__()
        self.title("MSX Disk Image Manager")
        self.geometry("640x420")
        self.minsize(480, 300)

        self.image_path = None   # local filesystem path to the .dsk file
        self.cwd = []             # path components inside the image ([] = root)

        self._build_ui()

        missing = find_missing_tools()
        if missing:
            messagebox.showerror(
                "mtools not found",
                "The following mtools programs are not on PATH:\n\n"
                + ", ".join(missing) +
                "\n\nInstall mtools first (Linux: apt install mtools / "
                "Mac: brew install mtools / Windows: via WSL or a native "
                "build), then restart this tool.")

        if initial_image:
            self._open_image(initial_image)

    # -- UI construction ---------------------------------------------------

    def _build_ui(self):
        top = ttk.Frame(self, padding=6)
        top.pack(fill=tk.X)
        ttk.Button(top, text="Open Image...", command=self._choose_open).pack(side=tk.LEFT)
        ttk.Button(top, text="New Image...", command=self._choose_new).pack(side=tk.LEFT, padx=(6, 0))
        self.image_label = ttk.Label(top, text="(no image open)", foreground="#666")
        self.image_label.pack(side=tk.LEFT, padx=(12, 0))

        nav = ttk.Frame(self, padding=(6, 0))
        nav.pack(fill=tk.X)
        self.up_button = ttk.Button(nav, text="↑ Up", command=self._go_up, state=tk.DISABLED)
        self.up_button.pack(side=tk.LEFT)
        self.path_label = ttk.Label(nav, text="/")
        self.path_label.pack(side=tk.LEFT, padx=(8, 0))

        tree_frame = ttk.Frame(self, padding=6)
        tree_frame.pack(fill=tk.BOTH, expand=True)
        columns = ("type", "size", "date", "time")
        self.tree = ttk.Treeview(tree_frame, columns=columns, show="tree headings")
        self.tree.heading("#0", text="Name")
        self.tree.heading("type", text="Type")
        self.tree.heading("size", text="Size")
        self.tree.heading("date", text="Date")
        self.tree.heading("time", text="Time")
        self.tree.column("#0", width=220)
        self.tree.column("type", width=60, anchor=tk.CENTER)
        self.tree.column("size", width=80, anchor=tk.E)
        self.tree.column("date", width=90, anchor=tk.CENTER)
        self.tree.column("time", width=60, anchor=tk.CENTER)
        vsb = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL, command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        vsb.pack(side=tk.RIGHT, fill=tk.Y)
        self.tree.bind("<Double-1>", self._on_double_click)

        btns = ttk.Frame(self, padding=6)
        btns.pack(fill=tk.X)
        for text, cmd in (
            ("Add File(s)...", self._add_files),
            ("Extract Selected...", self._extract_selected),
            ("New Folder...", self._new_folder),
            ("Rename...", self._rename_selected),
            ("Delete Selected", self._delete_selected),
            ("Refresh", self._refresh),
            ("Fix Boot Sector", self._fix_boot_sector),
        ):
            ttk.Button(btns, text=text, command=cmd).pack(side=tk.LEFT, padx=(0, 6))

        self.status = tk.StringVar(value="Open or create a .dsk image to begin.")
        ttk.Label(self, textvariable=self.status, relief=tk.SUNKEN, anchor=tk.W, padding=4).pack(fill=tk.X)

    # -- image open / create -------------------------------------------------

    def _choose_open(self):
        path = filedialog.askopenfilename(
            title="Open disk image",
            filetypes=[("Disk images", "*.dsk *.DSK *.img *.IMG"), ("All files", "*.*")])
        if path:
            self._open_image(path)

    def _open_image(self, path):
        self.image_path = path
        self.cwd = []
        self.image_label.config(text=os.path.basename(path))
        self._refresh()

    def _choose_new(self):
        dlg = NewImageDialog(self)
        self.wait_window(dlg)
        if dlg.result is None:
            return
        path, size_kb, label = dlg.result
        if os.path.exists(path):
            if not messagebox.askyesno(
                    "Overwrite?",
                    f"{path} already exists. Reformat it as a blank "
                    f"{size_kb}KB image? This erases everything on it."):
                return
        try:
            run(["mformat", "-C", "-f", str(size_kb), "-v", label or "MSXDISK",
                 "-i", path, "::"])
            sanitize_boot_sector(path)
            reserve_bad_clusters(path)
        except (MtoolsError, OSError) as e:
            messagebox.showerror("Format failed", str(e))
            return
        self.status.set(f"Created blank {size_kb}KB image: {path}")
        self._open_image(path)

    # -- listing / navigation -------------------------------------------------

    def _require_image(self):
        if not self.image_path:
            messagebox.showinfo("No image", "Open or create a disk image first.")
            return False
        return True

    def _refresh(self):
        if not self.image_path:
            return
        self.tree.delete(*self.tree.get_children())
        try:
            out = run(["mdir", "-i", self.image_path, mpath(self.cwd)])
        except MtoolsError as e:
            messagebox.showerror("Directory listing failed", str(e))
            return
        entries = parse_mdir(out)
        entries.sort(key=lambda e: (not e['is_dir'], e['name']))
        for e in entries:
            self.tree.insert("", tk.END, iid=e['name'], text=e['name'],
                             values=("<DIR>" if e['is_dir'] else "File",
                                     "" if e['is_dir'] else e['size'],
                                     e['date'], e['time']),
                             tags=("dir",) if e['is_dir'] else ("file",))
        self.path_label.config(text="/" + "/".join(self.cwd))
        self.up_button.config(state=tk.NORMAL if self.cwd else tk.DISABLED)
        self.status.set(f"{len(entries)} entr{'y' if len(entries) == 1 else 'ies'}")

    def _selected_entry(self):
        sel = self.tree.selection()
        if not sel:
            return None
        name = sel[0]
        values = self.tree.item(name, "values")
        return {'name': name, 'is_dir': values[0] == "<DIR>"}

    def _on_double_click(self, _event):
        entry = self._selected_entry()
        if entry and entry['is_dir']:
            self.cwd.append(entry['name'])
            self._refresh()

    def _go_up(self):
        if self.cwd:
            self.cwd.pop()
            self._refresh()

    # -- file operations -------------------------------------------------

    def _add_files(self):
        if not self._require_image():
            return
        paths = filedialog.askopenfilenames(title="Select file(s) to add")
        if not paths:
            return
        errors = []
        for local_path in paths:
            name = os.path.basename(local_path)
            try:
                run(["mcopy", "-o", "-i", self.image_path, local_path,
                     mpath(self.cwd, name)])
            except MtoolsError as e:
                errors.append(f"{name}: {e}")
        if errors:
            messagebox.showerror("Some files failed", "\n".join(errors))
        self._refresh()

    def _extract_selected(self):
        if not self._require_image():
            return
        entry = self._selected_entry()
        if not entry or entry['is_dir']:
            messagebox.showinfo("Extract", "Select a file (not a folder) first.")
            return
        dest_dir = filedialog.askdirectory(title="Extract to folder")
        if not dest_dir:
            return
        try:
            run(["mcopy", "-i", self.image_path,
                 mpath(self.cwd, entry['name']), dest_dir + os.sep])
        except MtoolsError as e:
            messagebox.showerror("Extract failed", str(e))
            return
        self.status.set(f"Extracted {entry['name']} to {dest_dir}")

    def _new_folder(self):
        if not self._require_image():
            return
        name = simpledialog.askstring("New Folder", "Folder name (8.3, e.g. GAMES):", parent=self)
        if not name:
            return
        try:
            run(["mmd", "-i", self.image_path, mpath(self.cwd, name)])
        except MtoolsError as e:
            messagebox.showerror("New Folder failed", str(e))
            return
        self._refresh()

    def _rename_selected(self):
        if not self._require_image():
            return
        entry = self._selected_entry()
        if not entry:
            messagebox.showinfo("Rename", "Select a file or folder first.")
            return
        new_name = simpledialog.askstring("Rename", "New name:", initialvalue=entry['name'], parent=self)
        if not new_name or new_name == entry['name']:
            return
        try:
            run(["mren", "-i", self.image_path,
                 mpath(self.cwd, entry['name']), mpath(self.cwd, new_name)])
        except MtoolsError as e:
            messagebox.showerror("Rename failed", str(e))
            return
        self._refresh()

    def _delete_selected(self):
        if not self._require_image():
            return
        entry = self._selected_entry()
        if not entry:
            messagebox.showinfo("Delete", "Select a file or folder first.")
            return
        kind = "folder" if entry['is_dir'] else "file"
        if not messagebox.askyesno("Delete", f"Delete {kind} '{entry['name']}'?"):
            return
        try:
            if entry['is_dir']:
                # DOS/mtools rule: a directory must be empty before it can
                # be removed — mrd fails with a clear message otherwise
                # (surfaced via MtoolsError below) rather than us trying
                # to recursively empty it ourselves.
                run(["mrd", "-i", self.image_path, mpath(self.cwd, entry['name'])])
            else:
                run(["mdel", "-i", self.image_path, mpath(self.cwd, entry['name'])])
        except MtoolsError as e:
            messagebox.showerror("Delete failed", str(e))
            return
        self._refresh()

    def _fix_boot_sector(self):
        """Retroactively apply sanitize_boot_sector() (see its comment)
        to the currently open image — for images created before this
        tool did it automatically on New Image, or images made some
        other way (raw mformat/mkfs.vfat/etc. on the command line) that
        carry the same generic PC-style boot sector header. Doesn't
        touch the FAT, directory, or any file content — only two bytes
        of the boot sector itself."""
        if not self._require_image():
            return
        if not messagebox.askyesno(
                "Fix Boot Sector",
                "Patch this image's boot sector to the MSX-DOS validity "
                "convention (Datapack 3.1)? This is what makes disk "
                "images created by generic tools (mtools, mkfs.vfat, "
                "etc.) work with MSX Disk ROMs that execute the boot "
                "sector as part of their own startup check (some do, "
                "most don't). Doesn't touch any files or the directory."):
            return
        try:
            sanitize_boot_sector(self.image_path)
        except OSError as e:
            messagebox.showerror("Fix Boot Sector failed", str(e))
            return
        self.status.set("Boot sector patched to MSX-DOS convention.")


class NewImageDialog(tk.Toplevel):
    """Modal dialog: pick a destination path, size preset, and volume
    label for a new blank image. Sets self.result to (path, size_kb,
    label) on OK, or leaves it None on cancel."""

    def __init__(self, parent):
        super().__init__(parent)
        self.title("New Disk Image")
        self.resizable(False, False)
        self.result = None
        self.transient(parent)
        self.grab_set()

        frm = ttk.Frame(self, padding=10)
        frm.pack(fill=tk.BOTH, expand=True)

        ttk.Label(frm, text="Save as:").grid(row=0, column=0, sticky=tk.W)
        self.path_var = tk.StringVar()
        ttk.Entry(frm, textvariable=self.path_var, width=40).grid(row=1, column=0, columnspan=2, sticky=tk.EW)
        ttk.Button(frm, text="Browse...", command=self._browse).grid(row=1, column=2, padx=(4, 0))

        ttk.Label(frm, text="Size:").grid(row=2, column=0, sticky=tk.W, pady=(8, 0))
        self.size_var = tk.StringVar(value=IMAGE_PRESETS[0][0])
        ttk.Combobox(frm, textvariable=self.size_var, state="readonly",
                     values=[label for label, _ in IMAGE_PRESETS]).grid(
            row=3, column=0, columnspan=2, sticky=tk.EW, pady=(0, 8))

        ttk.Label(frm, text="Volume label (optional):").grid(row=4, column=0, sticky=tk.W)
        self.label_var = tk.StringVar()
        ttk.Entry(frm, textvariable=self.label_var, width=20).grid(row=5, column=0, sticky=tk.W)

        buttons = ttk.Frame(frm)
        buttons.grid(row=6, column=0, columnspan=3, pady=(12, 0), sticky=tk.E)
        ttk.Button(buttons, text="Cancel", command=self.destroy).pack(side=tk.RIGHT)
        ttk.Button(buttons, text="Create", command=self._on_create).pack(side=tk.RIGHT, padx=(0, 6))

    def _browse(self):
        path = filedialog.asksaveasfilename(
            title="New disk image", defaultextension=".dsk",
            filetypes=[("Disk images", "*.dsk *.DSK"), ("All files", "*.*")])
        if path:
            self.path_var.set(path)

    def _on_create(self):
        path = self.path_var.get().strip()
        if not path:
            messagebox.showinfo("New Disk Image", "Choose a destination path first.", parent=self)
            return
        size_kb = dict(IMAGE_PRESETS)[self.size_var.get()]
        label = self.label_var.get().strip()[:11]  # FAT volume label limit
        self.result = (path, size_kb, label)
        self.destroy()


def main():
    initial_image = sys.argv[1] if len(sys.argv) > 1 else None
    app = DskManagerApp(initial_image)
    app.mainloop()


if __name__ == "__main__":
    main()
