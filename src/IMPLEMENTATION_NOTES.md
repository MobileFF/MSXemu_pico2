# HD61700 CPU Core - Implementation Notes

## Current Implementation Status

### Completed
- Basic CPU state structure (registers, PC, flags)
- Memory/IO callback system
- Fetch cycle
- Basic instruction decoder framework

### Instruction Set (Partial)
Currently implemented opcodes:
- `0x00`: NOP
- `0x01`: SLP (Sleep/Halt)
- `0x40-0x5F`: LD reg, imm8 (example - needs verification)
- `0x60`: JP imm16 (Jump absolute)
- `0x80-0x9F`: Arithmetic operations (placeholder)

### TODO - Full Instruction Set

Based on HD61700.TXT and MAME sources, the following instruction categories need implementation:

#### Data Transfer (LD, LDI, LDD等)
- Register to Register
- Memory to Register
- Immediate to Register  
- Indexed addressing modes

#### Arithmetic/Logical
- ADD, ADC, SUB, SBC
- AND, OR, XOR
- INC, DEC
- Multi-byte operations (ADBM, SBBM, etc.)

#### Control Flow
- JP, JR (conditional/unconditional)
- CALL (CAL), RETURN (RTN)
- TRAP (TRP)

#### Stack Operations
- PUSH (PHU, PHS, PPU, PPS)
- POP
- PRE (Prepare stack)

#### I/O and Special
- IN, OUT (Port I/O)
- LCD commands (STL, LDL, etc.)
- Timer operations

## Opcode Encoding

The HD61700 uses variable-length instructions (1-4 bytes):
- Byte 1: Opcode
- Byte 2-4: Operands (register numbers, immediates, addresses)

### Register Encoding
- Main registers: $00-$1F (32 registers)
- SIR (Index registers): SX, SY, SZ (5-bit values)
- Index registers: IX, IY, IZ (16-bit)

## Memory Map (PB-1000)

```
0x0000-0x17FF:  Internal ROM (6KB) - rom0.bin
0x1800-0x?:     External ROM bank
0x2000-0x9FFF:  RAM (32KB)
0xA000-0xFFFF:  Banked memory
```

## Next Steps

1. Study MAME `hd61700_device::state_string_export()` for complete opcode table
2. Implement instruction groups systematically
3. Add disassembler for debugging
4. Test with simple ROM code

## References

- `src/hd61700.cpp` (MAME) - Complete implementation
- `HD61700.TXT` - Assembly language manual
- PB-1000 Technical Reference - System architecture
