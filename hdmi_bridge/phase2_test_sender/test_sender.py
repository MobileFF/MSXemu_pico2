# Phase 2 テスト送信スクリプト(本体MSXエミュレータ用Pico2側、MicroPythonのREPLから
# 手動でimportして実行する。main.py/本番エミュレータには一切組み込まない)。
#
# 既存のSPI1バス(SCK=GP10, MOSI=GP11。LCD/SDカードと共有)に、新設のCS=GP28で
# HDMI側Pico2(当時はhdmi_bridge/phase2_receiver、現在は独立プロジェクト化された
# ../../../hdmi_bridge_receiver)へ256x192のRGB332テストパターンを
# 送り続ける。LCD/SDへのアクセスは行わない単体テストなので、このスクリプト単体を
# 実行している間はバス競合の心配はない(本番統合時はPhase3で排他制御を入れる)。
#
# 使い方 (実機REPLで):
#   >>> import hdmi_test_sender
#   >>> hdmi_test_sender.run()

import machine
import time

CS_PIN = 28
SCK_PIN = 10
MOSI_PIN = 11

IMG_W = 256
IMG_H = 192


def _build_test_frame():
    bar_w = IMG_W // 8
    row = bytearray(IMG_W)
    for x in range(IMG_W):
        bar = x // bar_w
        if bar > 7:
            bar = 7
        row[x] = bar * 32  # 0,32,64,...,224 の8階調バー(RGB332の混ざり方は問わない)
    return bytes(row) * IMG_H


def run(baudrate=10_000_000, fps_limit=30):
    # RP2350のPL022 SPIスレーブはCPOL=0/CPHA=0(モード0)だと数バイトで同期が
    # 崩れる既知の制約があるため、CPOL=1/CPHA=1(モード3)を使う(受信側と揃える)。
    spi = machine.SPI(
        1,
        baudrate=baudrate,
        polarity=1,
        phase=1,
        sck=machine.Pin(SCK_PIN),
        mosi=machine.Pin(MOSI_PIN),
    )
    cs = machine.Pin(CS_PIN, machine.Pin.OUT, value=1)
    frame = _build_test_frame()

    print("sending %d bytes/frame via SPI1 (CS=GP%d), baudrate=%d" % (len(frame), CS_PIN, baudrate))

    period_ms = 1000 // fps_limit
    n = 0
    t_report = time.ticks_ms()
    while True:
        t0 = time.ticks_ms()
        cs.value(0)
        spi.write(frame)
        cs.value(1)
        n += 1

        if time.ticks_diff(t0, t_report) >= 1000:
            print("frames/sec (approx):", n)
            n = 0
            t_report = t0

        dt = time.ticks_diff(time.ticks_ms(), t0)
        if dt < period_ms:
            time.sleep_ms(period_ms - dt)
