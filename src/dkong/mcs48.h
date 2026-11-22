// license:BSD-3-Clause
// copyright-holders:Dan Boris, Mirko Buffoni, Aaron Giles, Couriersud
//
// mcs48.h -- the Fujitsu MB8884 (an 8035: the MCS-48 with no ROM, 64 bytes
// of RAM and the external bus) as MAME 0.288 emulates it
// (src/devices/cpu/mcs48/mcs48.cpp), in C, for one CPU. Each instruction
// takes its cycles and then does what it does, as MAME's does, so the
// board sees a port write at the same time MAME's does. MAME's license
// (BSD-3-Clause) is in README.md.
#ifndef MCS48_H
#define MCS48_H

#include <stdbool.h>
#include <stdint.h>

// the board's side (dk.c)
extern const uint8_t *mcs48_rom;        // 4K, the program (A0-A11)
uint8_t dk_snd_ext_r(uint8_t a);        // MOVX A,@Rn (the I/O space)
uint8_t dk_snd_bus_r(void);             // INS A,BUS and the bus's ANL/ORL
uint8_t dk_snd_p2_r(void);              // port 2 (port 1 reads 0xff: nothing is on it)
void dk_snd_p1_w(uint8_t d);
void dk_snd_p2_w(uint8_t d);
int dk_snd_t0(void);                    // the T0 and T1 test inputs
int dk_snd_t1(void);

void mcs48_start(void);                 // device_start
void mcs48_reset(void);                 // device_reset
int mcs48_run(int cycles);              // execute_run for a timeslice; returns the cycles run
void mcs48_abort(void);                 // abort_timeslice
int mcs48_done(void);                   // cycles run so far in this timeslice
void mcs48_set_irq(int state);          // MCS48_INPUT_IRQ

typedef struct {
	uint16_t pc;
	uint8_t a, psw, p1, p2, timer, prescaler, f1, a11;
	uint8_t ram[64];
} mcs48_regs_t;
void mcs48_regs(mcs48_regs_t *r);
extern void (*mcs48_trace)(void);       // before each instruction, tests only

#endif
