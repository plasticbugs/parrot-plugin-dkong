// license:BSD-3-Clause
// copyright-holders:Juergen Buchmueller
//
// z80.h -- the Zilog Z80 as MAME 0.288 emulates it (src/devices/cpu/z80:
// z80.cpp, z80.h, z80.inc and z80.lst), in C, for one CPU. z80_ops.h is
// made from MAME's z80.lst by tools/z80gen.py; z80.c is the rest of
// z80_device. An instruction can stop at any of MAME's steps when the
// cycles run out and carry on there in the next call, as MAME's does.
//
// The board supplies the memory and I/O below. Reads and writes in a page
// of z80_rd_page / z80_wr_page go straight to it; the rest call the
// board's dk_rd / dk_wr. MAME's license (BSD-3-Clause) is in README.md.
#ifndef Z80_H
#define Z80_H

#include <stdbool.h>
#include <stdint.h>

extern const uint8_t *z80_rd_page[256];
extern uint8_t *z80_wr_page[256];

// the board's side (dk.c)
uint8_t dk_rd(uint16_t a);
void dk_wr(uint16_t a, uint8_t d);
uint8_t dk_io_in(uint16_t port);
void dk_io_out(uint16_t port, uint8_t d);
uint8_t dk_irq_ack(void);           // the vector the board puts on the bus
void dk_busack(int state);          // BUSACK's callback

void z80_start(void);               // device_start: power on
void z80_reset(void);               // device_reset
int z80_run(int cycles);            // a timeslice: MAME's execute_run; returns the cycles run
void z80_abort(void);               // abort_timeslice: the rest of the timeslice taken back
void z80_keep(int n);               // the rest cut to n cycles (if more), as if given that many
void z80_eat(void);                 // eat_cycles: the rest of the timeslice run (counted) without running
bool z80_stalled(void);
bool z80_bus_settled(void);         // BUSACK as BUSREQ (no change of it to come in a run)
// (dk.c's lazy slices) the state kept, and each memory write's old byte
// logged, until z80_unsnap() or z80_rollback() (the state and memory back)
void z80_snap(void);
void z80_unsnap(void);
void z80_rollback(void);
extern bool z80_undo_full;              // a write the log had no room for: not made (a rollback to come)             // the bus given away (BUSREQ and BUSACK): a run only eats
int z80_done(void);                 // cycles run so far in this timeslice (m_cycles_running - m_icount)
void z80_set_irq(int state);        // INPUT_LINE_IRQ0: ASSERT_LINE (1) or CLEAR_LINE (0)
void z80_set_nmi(int state, bool ran);  // INPUT_LINE_NMI; ran: the CPU's total cycles aren't 0
void z80_set_busreq(int state);     // Z80_INPUT_LINE_BUSREQ

typedef struct {
	uint16_t pc, sp, af, bc, de, hl, ix, iy, af2, bc2, de2, hl2, wz;
	uint8_t r, i, im, iff1, iff2, halt;
} z80_regs_t;
void z80_regs(z80_regs_t *r);       // as MAME's state interface gives them (F from the flags, R from r and r2)
extern void (*z80_trace)(void);     // before each instruction (MAME's debugger_instruction_hook), tests only

#endif
