import machine
import os
import time
from sdcard import SDCard

# SPI1 Pins (Same as pb1000.py)
SCK_PIN  = 10
MOSI_PIN = 11
MISO_PIN = 12
SD_CS_PIN = 15
LCD_CS_PIN = 9
T_CS_PIN   = 16

# SPI runs at 40 MHz (shared with LCD/touch); SDCard drops to 400 kHz during
# transfers and restores to 40 MHz afterwards via restore_baudrate.
SPI_BAUDRATE     = 40_000_000
SD_INIT_BAUDRATE = 400_000

def test_sd():
    print("--- SD Card Unit Test Start ---")

    # 1. SPI/CS Initialization
    try:
        spi = machine.SPI(
            1,
            baudrate=SPI_BAUDRATE,
            sck=machine.Pin(SCK_PIN),
            mosi=machine.Pin(MOSI_PIN),
            miso=machine.Pin(MISO_PIN),
        )
        # All CS lines HIGH before touching the bus
        machine.Pin(LCD_CS_PIN, machine.Pin.OUT, value=1)
        machine.Pin(T_CS_PIN,   machine.Pin.OUT, value=1)
        machine.Pin(SD_CS_PIN,  machine.Pin.OUT, value=1)
        sd_cs = machine.Pin(SD_CS_PIN, machine.Pin.OUT, value=1)
        print(f"SPI1 and CS pins initialized. (LCD_CS={LCD_CS_PIN}, T_CS={T_CS_PIN}, SD_CS={SD_CS_PIN})")
    except Exception as e:
        print(f"ERROR: SPI initialization failed: {e}")
        return

    # 2. SD Card Object
    # restore_baudrate returns the SPI bus to 40 MHz after each SD operation so
    # that the LCD/touch drivers (which share the same SPI) are unaffected.
    try:
        sd = SDCard(spi, sd_cs, baudrate=SD_INIT_BAUDRATE, restore_baudrate=SPI_BAUDRATE)
        sectors = sd.sectors if hasattr(sd, "sectors") else sd.ioctl(4, 0)
        print(f"SD Card object created. Sectors: {sectors}")
    except Exception as e:
        print(f"ERROR: SD Card init failed: {e}")
        print("Check your wiring (CS, SCK, MOSI, MISO) and ensure the card is inserted.")
        return

    # 3. Low-level block read test
    try:
        print("Testing direct block read (Sector 0)...")
        buf = bytearray(512)
        sd.readblocks(0, buf)
        print("Direct block read SUCCESS!")
        print("First 16 bytes:", " ".join(f"{b:02X}" for b in buf[:16]))
    except Exception as e:
        print(f"ERROR: Direct block read failed: {e}")

    # 4. Mount
    vfs_path = "/sd"
    try:
        vfs = os.VfsFat(sd)
        os.mount(vfs, vfs_path)
        print(f"SD Card mounted at {vfs_path}")
    except Exception as e:
        print(f"ERROR: Mounting failed: {e}")
        return

    # 5. Directory Creation
    test_dir = vfs_path + "/test_dir"
    try:
        print(f"Creating directory: {test_dir}")
        if "test_dir" not in os.listdir(vfs_path):
            os.mkdir(test_dir)
        print("Directory exists/created.")
    except Exception as e:
        print(f"ERROR: Directory creation failed: {e}")

    # 6. File Write/Read
    test_file = test_dir + "/hello.txt"
    test_content = "Hello PB-1000 Emulator SD Storage! " + str(time.ticks_ms())
    try:
        print(f"Writing to: {test_file}")
        with open(test_file, "w") as f:
            f.write(test_content)

        print(f"Reading back from: {test_file}")
        with open(test_file, "r") as f:
            read_content = f.read()

        print(f"Content: {read_content}")
        if read_content == test_content:
            print("SUCCESS: File R/W match!")
        else:
            print("FAILED: File R/W mismatch!")
    except Exception as e:
        print(f"ERROR: File R/W failed: {e}")

    # 7. List Files
    try:
        print(f"Files in {vfs_path}:")
        print(os.listdir(vfs_path))
    except Exception as e:
        print(f"ERROR: listdir failed: {e}")

    # 8. Virtual FDD directory check
    # configure_virtual_fdd() disables the FDD when the parent directory of the
    # configured disk image is absent (instead of crashing with ENOENT).
    vfdd_dir = vfs_path + "/disks"
    try:
        os.stat(vfdd_dir)
        print(f"Virtual FDD directory found: {vfdd_dir} — FDD will be enabled.")
    except OSError:
        print(f"Virtual FDD directory not found: {vfdd_dir} — FDD will be disabled (expected behaviour).")
        print(f"  To enable virtual FDD, create {vfdd_dir}/ and place disk1.img inside.")

    print("--- SD Card Unit Test End ---")
    print("If all above was SUCCESS, the hardware/driver is working correctly.")

if __name__ == "__main__":
    test_sd()
