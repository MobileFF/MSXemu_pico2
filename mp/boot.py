import machine
import os

# 2026-09-21: board_config.py (mp/*.py 側) はここではまだ import できない
# ("from board_config import *" は main.py 側、boot.py より後にしか動かない)
# ため、ボード判定だけ msx.get_board_type() を直接使う。以前は pico2 決め打ち
# (250MHz固定 + GP28固定PULL_UP)だったが、これが原因でpizero実機のUARTが
# 無反応になる実機不具合が発生した — GP28はpico2ではHDMIブリッジCSだが、
# pizeroではUSBホストD+ピン(xroar参考実装のピン配置表で確認)であり、単純な
# PULL_UP入力設定はUSBホスト機能と衝突しうる。250MHz固定もpizero実機で
# クロックロックに問題が起きる可能性を排除できていない（bare-metalテストは
# 150MHzでのみ動作確認済み、252MHz/250MHzは未検証）。
try:
    import msx
    _board = msx.get_board_type()
except Exception:
    _board = "pico2"

if _board == "pico2":
    # OC: set CPU clock before UART so baud divisor uses the new frequency
    try:
        machine.freq(250_000_000)
    except Exception:
        pass

    # GP28: 外部SPI CS ピン（HDMIブリッジ用）。ファームウェア起動前に
    # フローティングで誤アサートしないよう PULL_UP を設定してHIGH（非選択）
    # 状態を保つ。pizeroにはHDMIブリッジが無く、GP28はUSBホストD+のため
    # ここでは触らない。
    machine.Pin(28, machine.Pin.IN, machine.Pin.PULL_UP)

# UART0を起動し、REPLをUARTに複製する設定
# Baudrateを115200に設定（標準設定）— GP0/GP1はpico2/pizero共通
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
