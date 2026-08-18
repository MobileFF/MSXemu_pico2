import machine
import os

# OC: set CPU clock before UART so baud divisor uses the new frequency
try:
    machine.freq(250_000_000)
except Exception:
    pass

# GP28: 外部SPI CS ピン。ファームウェア起動前にフローティングで誤アサートしないよう
# PULL_UP を設定してHIGH（非選択）状態を保つ。
machine.Pin(28, machine.Pin.IN, machine.Pin.PULL_UP)

# UART0を起動し、REPLをUARTに複製する設定
# Baudrateを115200に設定（標準設定）
uart = machine.UART(0, baudrate=115200, tx=machine.Pin(0), rx=machine.Pin(1), txbuf=32)
os.dupterm(uart)
# # Boot script for PB-1000 Emulator
# # This runs automatically on power-up
# 
# import gc
# import machine
# import time
# 
# # Disable debug output for production
# # machine.freq(133000000)  # Set CPU frequency if needed
# 
# # Print banner
# print("=" * 40)
# print("PB-1000 Emulator for Raspberry Pi Pico 2")
# print("=" * 40)
# 
# # Free up memory
# gc.collect()
# print(f"Free memory: {gc.mem_free()} bytes")
# 
# # Wait a moment for serial to stabilize
# time.sleep_ms(100)
# 
# # Import and run main
# try:
#     import main
#     main.main()
# except Exception as e:
#     print(f"Error starting emulator: {e}")
#     import sys
#     sys.print_exception(e)
