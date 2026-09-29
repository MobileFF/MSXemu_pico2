#ifndef Z80_Z80_H_
#define Z80_Z80_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct z80 z80;
struct z80 {
  uint8_t (*read_byte)(void*, uint16_t);
  void (*write_byte)(void*, uint16_t, uint8_t);
  uint8_t (*port_in)(z80*, uint8_t);
  void (*port_out)(z80*, uint8_t, uint8_t);
  // Optional subroutine hook (NULL by default — zero overhead when
  // unused). Called with (userdata, target address) at four points —
  // mirrors this project's sibling PB-1000 emulator's HD61700 call-hook
  // design (see modmsx.c's set_call_hook() comment for the full
  // rationale), adapted to the Z80's real CALL/JP/JR opcodes:
  //
  //   1. Immediate-target CALL nn / CALL cc,nn (condition true) / RST n
  //      — checked in call(), before the return address would be pushed.
  //   2. Immediate-target JP nn / JP cc,nn (condition true) — checked in
  //      jump_checked(), before the jump would take effect.
  //   3. Immediate-target JR e / JR cc,e / DJNZ e (condition true) —
  //      checked in jr(), before the relative jump would take effect.
  //   4. A generic trap checked once per z80_step(), right before
  //      fetching/decoding whatever instruction sits at z->pc — catches
  //      every other way a hooked address can be reached (an indirect
  //      JP (HL)/(IX)/(IY), a RET landing there, plain fall-through, an
  //      interrupt vector, or a CALL to a *different* real address that
  //      itself immediately jumps there — anything (1)-(3) can't see).
  //
  // Returning true intercepts: for (1)-(3), the push/jump is skipped
  // entirely and execution just continues at the instruction after the
  // CALL/JP/JR, as if the "subroutine" had already run and returned; for
  // (4), a return address is popped off the stack and execution resumes
  // there instead (simulates an instant RET) — this REQUIRES the hooked
  // address to be a genuine subroutine entry point that a real call
  // already pushed a return address for; it is never safe to intercept
  // via (4) at a bare JP/JR/fall-through target with nothing pushed.
  // Returning false passes through: the real CALL/JP/JR (or, for (4),
  // the real instruction at z->pc) executes normally — useful for a hook
  // that only wants to observe/log before the original code runs. After
  // a (1)-(3) passthrough, hook_suppress_active is set so the trap in
  // (4) doesn't immediately re-fire the instant execution lands on the
  // same now-unintercepted address.
  //
  // This is a project addition (not part of upstream superzazu/z80) —
  // see msx's set_call_hook()/clear_call_hook()/set_call_hook_enabled()
  // Python API.
  bool (*call_hook)(void*, uint16_t);
  void* userdata;
  bool hook_suppress_active; // see call_hook's comment above (point 4)

  // 2026-09-20: real-hardware finding — a small, fixed set of always-on
  // extension hooks (e.g. mp/ext/'s auto-loaded modules) was enough to
  // make `call_hook` non-NULL for an entire session, and every one of
  // (1)-(4) above unconditionally calls through the function pointer
  // (into modmsx.c's table scan) whenever that's true — including (4),
  // which runs on *every single instruction*, not just CALL/JP/JR ones.
  // That's a real, measured FPS cost even for a cartridge game that
  // never touches a hooked address. This bitmap (1 bit per Z80 address,
  // set/cleared by msx.set_call_hook()/clear_call_hook()/
  // set_call_hook_enabled() — see modmsx.c) turns the overwhelmingly
  // common "nothing hooked here" case into one cheap array-index-and-
  // bit-test before ever reaching the function pointer, at all four
  // trigger points — see hook_registered() in z80.c.
  uint8_t hook_bitmap[8192]; // 65536 addresses / 8 bits per byte

  unsigned long cyc; // cycle count (t-states)

  uint16_t pc, sp, ix, iy; // special purpose registers
  uint16_t mem_ptr; // "wz" register
  uint8_t a, b, c, d, e, h, l; // main registers
  uint8_t a_, b_, c_, d_, e_, h_, l_, f_; // alternate registers
  uint8_t i, r; // interrupt vector, memory refresh

  // flags: sign, zero, yf, half-carry, xf, parity/overflow, negative, carry
  bool sf : 1, zf : 1, yf : 1, hf : 1, xf : 1, pf : 1, nf : 1, cf : 1;

  uint8_t iff_delay;
  uint8_t interrupt_mode;
  uint8_t int_data;
  bool iff1 : 1, iff2 : 1;
  bool halted : 1;
  bool int_pending : 1, nmi_pending : 1;
};

void z80_init(z80* const z);
void z80_step(z80* const z);
void z80_debug_output(z80* const z);
void z80_gen_nmi(z80* const z);
void z80_gen_int(z80* const z, uint8_t data);

// Marks/unmarks `addr` in hook_bitmap — call whenever a hook is
// registered, cleared, or enabled/disabled (see hook_bitmap's comment).
void z80_hook_bitmap_set(z80* const z, uint16_t addr, bool set);

#endif // Z80_Z80_H_
