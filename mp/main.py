# main.py — thin launcher.
#
# All of this project's actual boot/run logic lives in msx_app.py, which
# is frozen into the firmware (src/msx/manifest_freeze.py, doc/
# memory_usage.md §9) to remove its ~1300-line compile cost from every
# boot — msx_app.py's own content used to BE this file, verbatim, before
# 2026-10-17.
#
# This file itself must stay a real, unfrozen .py file on the device
# filesystem: it's the script MicroPython's boot sequence auto-execs on
# power-up, and what `mpremote run mp/main.py` sends over the wire for
# dev testing — a frozen module is only ever reached via the normal
# `import` machinery, not by being auto-exec'd as the top-level script,
# so this one small file necessarily keeps paying its own (now tiny)
# compile cost every boot.
#
# `import main; main.run()` (the REPL dev-iteration shortcut documented
# in build_guide.md/usage_guide.md, for re-running without a full power
# cycle) keeps working unchanged: `run` is re-exported below under this
# module's own name, exactly as if it were still defined here directly.
from msx_app import run

if __name__ == '__main__':
    run()
