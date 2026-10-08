# 2026-09-30 diagnostic-only — isolates PIO-USB keyboard enumeration/HID
# reports from the rest of the boot pipeline (no SD, no DVI init, no cart
# load, no Z80 run_frame(), no audio). Run directly:
#
#   mpremote run test_usb_standalone.py
#
# Purpose: main.py's full pipeline shows usb_host.debug() reporting
# connected=1 (PIO-USB root port sees the keyboard attached) but
# usb_host.get_hid_report() never changes even while keys are held down,
# and usb_host.task() never raises. This script checks whether the same
# thing happens with JUST usb_host running — if enumeration/HID reports
# work fine here, something about running alongside DVI/Z80/audio is
# starving TinyUSB's enumeration state machine of timely usb_host.task()
# calls; if it ALSO fails here, the problem is in USB itself (or the
# physical keyboard/cable), unrelated to the rest of the pipeline.

import time
import msx
import usb_host

msx.init()
usb_host.init()
print("usb_host.init() done — waiting for enumeration...")

last_report = None
t_start = time.ticks_ms()

while True:
    usb_host.task()

    report = usb_host.get_hid_report()
    if report != last_report:
        print(f"HID report changed: {bytes(report) if report else report}")
        last_report = bytes(report) if report else report

    elapsed = time.ticks_diff(time.ticks_ms(), t_start)
    if elapsed >= 500:
        print(f"usb_host.debug() = {usb_host.debug()}")
        t_start = time.ticks_ms()

    time.sleep_ms(5)
