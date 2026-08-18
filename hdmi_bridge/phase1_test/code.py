# PICO-HDMI-PLUS 単体動作確認 (Phase 1)
# CircuitPython (Pico 2 / RP2350) を PICO-HDMI-PLUS に挿した状態で書き込む。
# 本体MSXエミュレータ用Pico2とは無接続・無関係。HDMI出力だけを単独で確認するためのテスト。
#
# ピン割り当ては PICO-HDMI-PLUS の固定配線 (D0=赤, CK=クロック, D2=青, D1=緑) を仮定。
# 色が入れ替わって見える場合は red/green/blue の dp/dn を GP16,17 / GP18,19 の組で
# 入れ替えて試すこと(配線ミスではなくチャンネル割り当ての当てずっぽうのため)。

import board
import picodvi
import framebufferio
import displayio
import time

displayio.release_displays()

fb = picodvi.Framebuffer(
    320, 240,
    clk_dp=board.GP14, clk_dn=board.GP15,
    red_dp=board.GP12, red_dn=board.GP13,
    green_dp=board.GP18, green_dn=board.GP19,
    blue_dp=board.GP16, blue_dn=board.GP17,
    color_depth=8,
)
display = framebufferio.FramebufferDisplay(fb)

WIDTH, HEIGHT = fb.width, fb.height

bitmap = displayio.Bitmap(WIDTH, HEIGHT, 8)
palette = displayio.Palette(8)
colors = [0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000]
for i, c in enumerate(colors):
    palette[i] = c

bar_w = WIDTH // 8
for x in range(WIDTH):
    idx = min(x // bar_w, 7)
    for y in range(HEIGHT):
        bitmap[x, y] = idx

group = displayio.Group()
group.append(displayio.TileGrid(bitmap, pixel_shader=palette))
display.root_group = group

while True:
    time.sleep(1)
