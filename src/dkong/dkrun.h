// dkrun: the cart's loop, a list at a time, shared by the plugin and the
// Mac tests so both run the board the same way. Portable. (Pac-Man's
// pmrun.c.)
//
// Each list runs the board for the list's time (DKCV_LIST_T of the
// console's T: 16.77 ms, a little more than the board's 16.50 ms frame), to
// a cycle count kept from the start so nothing drifts; DKCV_PCM_N samples
// of the sound go with the list. The board runs DKRUN_LEAD cycles ahead of
// the samples taken, so a list always finds its samples made.
//
// The sound: dksnd.c's circuits stepped DKRUN_OVER times a sample, in
// floats (DKSND_FLOAT), each sample their mean, at exactly DKCV_PCM_N
// samples a list.
//
// The picture: the board at the last VBLANK in the list's time. The board
// stops at the first of MAME's timeslice ends past the list's end, which
// can be up to a frame later, so a run may pass the next list's VBLANK
// too: the board is copied at each VBLANK and the copy kept for the list
// it belongs to. So each list has one picture, of its own frame, and
// about one frame in 62 is never shown (60.6 frames a second, 59.6 lists).
// A picture that changes more than DKCV_TILES tiles leaves the rest for
// the next picture, and the list after it makes none. A list made while
// the plugin has no list waiting (`behind`: the last one went to the
// console the moment it was done, so this one has less time) draws at
// most DKRUN_TILES_BEHIND: a screen change then takes a few more lists,
// and the list stays near its usual cost. In play few tiles change, so
// the pictures there are whole either way.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dk.h"
#include "dkcv.h"
#include "dksnd.h"

#define DKRUN_LEAD 2600                 // Z80 cycles: 0.85 ms, about 5 samples
#define DKRUN_TILES_BEHIND 96           // the most tiles a picture made behind draws (about 3 ms)
#define DKRUN_FRAME 50688               // the board's frame: 264 lines of 192 cycles
#ifndef DKRUN_OVER
#define DKRUN_OVER 4                    // the circuits' steps a sample (23,849 a second)
#endif

typedef struct {
    dkcv_t *cv;
    uint32_t lists;                     // since the start
    uint64_t target;                    // the Z80 cycle this list runs to
    uint32_t underruns;                 // samples a list had to make up
    uint32_t pictures;
    uint32_t rested;                    // lists with no picture, after one that left tiles
    uint32_t hurried;                   // pictures made behind that left tiles for later
    bool rest;
    bool behind;                        // set by the caller before each list (the tests leave it false)
    int16_t pcm[DKCV_PCM_N];
    dkcv_board_t board[2];              // the board at the VBLANK for this list ([cur]), and one past its end
    bool have[2];
    uint8_t cur;
    const dkcv_board_t *shown;          // the one the last picture was made from (for the tests)
} dkrun_t;

// the sound's circuits made ready: before dk_init, which sends them the board's first state
void dkrun_sound_init(void);
// the board started (dk_init done)
void dkrun_start(dkrun_t *r, dkcv_t *cv);
// one list: the board for its time, then the list into out; returns its length
size_t dkrun_list(dkrun_t *r, uint8_t *out);
