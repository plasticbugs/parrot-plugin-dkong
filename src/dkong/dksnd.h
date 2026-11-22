// license:BSD-3-Clause
// copyright-holders:K.Wilkins, Couriersud, Derrick Renaud, Frank Palazzolo
//
// dksnd.h -- Donkey Kong's sound board after the sound CPU: the DAC and the
// discrete circuits (walk, jump, stomp, the DAC's decay and filter, the
// amplifier) as MAME 0.288's dkong2b_discrete (audio/dkong_a.cpp, with the
// discrete nodes of sound/disc_*.hxx) computes them, sample by sample.
//
// The board (dk.c) hands over what the circuits see, with when
// (dk_snd_event, in ticks of 76.8 MHz, which the Z80's cycles, the sound
// CPU's and MAME's 48 kHz samples all divide). A change counts from the
// first sample that starts after it and after every change and update
// before it, as MAME's stream updates place it.
//
// At 48 kHz in doubles (the tests) it is MAME's sound. The cart computes
// it in floats (DKSND_FLOAT) at a few steps to each of its samples and
// takes their mean.
#ifndef DKSND_H
#define DKSND_H

#include <stdint.h>

#define DKSND_TICK_HZ 76800000u         // the ticks of dk_snd_event's times

// rate_q8: the samples a second, << 8; over: the circuits' steps to a
// sample (MAME's way: 48 kHz, 1)
void dksnd_init(uint32_t rate_q8, int over);
// the same with a step's length given in ticks << 16 (the cart's: a list's
// time over its samples and their steps, so the rate is the lists' exactly)
void dksnd_init_step(uint64_t step_q16, int over);
// the samples up to where the board is (dk_snd_tick), MAME's -1..1 as
// -32767..32767 (clipped); returns how many (up to max)
int dksnd_take(int16_t *out, int max);
uint32_t dksnd_dropped(void);           // events the queue had no room for (tests)

#endif
