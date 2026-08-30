"""
test_call_hook_basic_call.py — BASICのCALL/USR()相当の呼び出しをフックし、
フック実行後に呼び出し元へ正しく制御が戻ることを実測で確認するデモ。

MSX BASICの`CALL`文や`X=USR(n)`は、内部的には単なるZ80の`CALL nn`命令
としてターゲットアドレスを呼び出しているだけであり、フック機構はこれを
CPUの命令実行レベルで検知するため「BASICが呼んだCALLかどうか」を区別
しない。従って:

  - フックが横取りすると、CALL命令の直後の命令から(スタックには一切
    触れずに)実行が継続する — 本物のサブルーチンが一瞬でRETしたのと
    同じ状態になる。
  - その直後の命令はBASIC側の`CALL`/`USR()`実装コード自身の続きなので、
    そのままBASICへ正常に制御が戻る。
  - `USR()`はHLレジスタを戻り値として使う呼び出し規約なので、`USR()`
    経由で呼ばれるアドレスをフックする場合は msx.debug_set_cpu() で
    HLに戻り値を設定してから戻る必要がある。

この一連の流れを、実際のBASIC起動やSDカードのROMファイルに依存せず、
CPUを直接駆動して検証する。手法:

  1. slot_selectを全ページRAMに設定する(msx.reset()直後は全ページ
     BIOS ROMなので、これをしないとRAMにコードを書いても実行できない)。
  2. MSX RAMに「CALL <フック先> の直後にHALT」という2命令だけの
     テストプログラムを書き込む — BASICの`CALL`/`USR()`実装コードの
     "呼び出し元" を模したもの(呼び出した後にHALTで停止=「呼び出し元
     に戻ってきた」ことの目印)。
  3. フック先アドレスに、USR()のHL引数/戻り値の受け渡しを模した
     コールバックを登録する。
  4. msx.debug_set_cpu()でPC/HLを設定し、msx.debug_step()で1命令ずつ
     実行して、CALL直後のHALTまで正しく制御が戻ることと、HLの戻り値が
     引き継がれることを確認する。

前提: なし(BIOS/カートリッジ/SDカード不要)。

実行方法 (Thonny / mpremote REPL):
    import test_call_hook_basic_call
"""
import msx

msx.init()
msx.reset()

# 全ページをRAM(スロット3)にマップする。2bit x 4ページ = 0b11_11_11_11
msx.debug_set_slot(0xFF)

# ── テストプログラムをRAMに書き込む ───────────────────────────────────────
#   0xC000: CD 00 C1     CALL 0xC100      (BASICの CALL/USR() が発行するのと
#                                          同じ、ただのZ80 CALL命令)
#   0xC003: 76           HALT             ("呼び出し元(BASIC)に戻ってきた"
#                                          ことの目印)
TEST_PC   = 0xC000
HOOK_ADDR = 0xC100

msx.debug_poke(TEST_PC + 0, 0xCD)               # CALL nn
msx.debug_poke(TEST_PC + 1, HOOK_ADDR & 0xFF)
msx.debug_poke(TEST_PC + 2, HOOK_ADDR >> 8)
msx.debug_poke(TEST_PC + 3, 0x76)               # HALT

# ── フック: USR()の呼び出し規約(HL=引数、HL=戻り値)を模したコールバック ──
def usr_style_hook():
    pc, sp, a, f, bc, de, hl, ix, iy, cyc, halted, iff1, im = msx.debug_cpu()
    print(f"  [HOOK] called, HL(USR argument) = 0x{hl:04X}")
    result = (hl + 1) & 0xFFFF                  # 適当な"計算結果"
    msx.debug_set_cpu(pc, sp, a, f, bc, de, result, ix, iy)
    print(f"  [HOOK] returning, HL(USR result) = 0x{result:04X}")

msx.set_call_hook(HOOK_ADDR, usr_style_hook)

# ── CPUを「BASICが X = USR(0x2000) を実行した直後」の状態にする ─────────
# (PC=テストプログラム先頭、HL=USR()の引数、SPは今回の経路では一度も
#  触れられないので値は何でもよい)
msx.debug_set_cpu(TEST_PC, 0xC200, 0, 0, 0, 0, 0x2000, 0, 0)

pc0, _, _, _, _, _, hl0, *_ = msx.debug_cpu()
print(f"before: PC=0x{pc0:04X}  HL=0x{hl0:04X}")

# ── CALL -> フック -> HALT まで1命令ずつ実行 ──────────────────────────────
for _ in range(10):
    msx.debug_step(1)
    halted = msx.debug_cpu()[10]
    if halted:
        break

pc, sp, a, f, bc, de, hl, ix, iy, cyc, halted, iff1, im = msx.debug_cpu()
print(f"after:  PC=0x{pc:04X}  HL=0x{hl:04X}  halted={halted}")

ok = True
if pc != TEST_PC + 4:
    print(f"  !!! PC不一致: CALL直後のHALT(0x{TEST_PC + 4:04X})に"
          f"戻っていません (0x{pc:04X})")
    ok = False
if not halted:
    print("  !!! HALTしていません — 制御が戻ってきていない可能性")
    ok = False
if hl != 0x2001:
    print(f"  !!! HL不一致: フックの戻り値(0x2001)が伝わっていません (0x{hl:04X})")
    ok = False

if ok:
    print("\n=== OK: フック実行後、CALL命令の直後(呼び出し元=BASIC相当)に")
    print("        正しく制御が戻り、USR()のHL戻り値も引き継がれました ===")
else:
    print("\n=== NG: 上記の不一致を確認してください ===")
