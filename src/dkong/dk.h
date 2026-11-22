// license:BSD-3-Clause
// copyright-holders:Couriersud, Curt Coder, Carl, Aaron Giles, O. Galibert
//
// dk.h -- the Donkey Kong board (TKG-04, two boards) as MAME 0.288
// emulates it (dkong.cpp, dkong_v.cpp, dkong_a.cpp, i8257.cpp,
// latch8.cpp, watchdog.cpp, the Z80 and MB8884 cores), with MAME's
// scheduler, so the Z80, the 8257 DMA and the sound CPU run the cycles
// MAME's do.
#ifndef DK_BOARD_H
#define DK_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#define DK_W 256                        // the raw screen, before MAME's ROT270
#define DK_H 224                        // (lines 16-239 of 264)
#define DK_CLOCK 3072000u               // the Z80 and the 8257: XTAL(61'440'000) / 20
#define DK_SND_CLOCK 400000u            // the MB8884's cycles: 6 MHz / 15

typedef struct {
	const uint8_t *maincpu;             // 16K: c_5et_g, c_5ct_g, c_5bt_g, c_5at_g
	const uint8_t *sound;               // 2K: s_3i_b (the 8035's program; A11 isn't decoded)
	const uint8_t *tune;                // 2K: s_3j_b (the tunes, read on the bus)
	const uint8_t *gfx1;                // 4K: v_5h_b, v_3pt (tiles)
	const uint8_t *gfx2;                // 8K: l_4m_b, l_4n_b, l_4r_b, l_4s_b (sprites)
	const uint8_t *proms;               // 0x300: c-2k, c-2j (palette), v-5e (tile colors)
	void *rows;                         // DK_ROWS_BYTES, 4-aligned, for the graphics decoded (dk_init
	                                    // reads gfx1 and gfx2 into it, then only it: the plugin
	                                    // gives it the room those were in, them a copy elsewhere)
} dk_roms_t;
#define DK_ROWS_BYTES (256 * 8 * 2 + 128 * 16 * 4)

// the ports as MAME reads them (active high). MAME's defaults: IN0 0x00,
// IN1 0x00, IN2 0x00 (but bit 6, the sound CPU's status), DSW0 0x80
// (3 lives, bonus at 7000, 1 coin 1 credit, upright)
typedef struct {
	uint8_t in0;                        // 0x01 right, 0x02 left, 0x04 up, 0x08 down, 0x10 jump
	uint8_t in1;                        // player 2's (cocktail)
	uint8_t in2;                        // 0x01 service, 0x04 start 1, 0x08 start 2, 0x80 coin
	uint8_t dsw0;
} dk_inputs_t;
#define DK_DSW0 0x80
extern dk_inputs_t dk_in;

void dk_init(const dk_roms_t *roms);    // power on
bool dk_frame(void);                    // run to the next VBLANK start; false if the watchdog reset the board

// The cart's way: run to the Z80's cycle `until` (from power on), stopping
// at the first of MAME's timeslice ends at or past it. Returns the VBLANKs
// passed; dk_on_vblank, if set, is called at each, with the frame's state.
uint32_t dk_run_to(uint64_t until);
uint64_t dk_time(void);                 // where the board is, in Z80 cycles
extern void (*dk_on_vblank)(void);
extern void (*dk_on_line)(int line);    // at each line's start (MAME's partial updates), tests only
// a lazy stretch of the Z80 starts (0) or is rolled back (1): what it did
// (a trace) is undone, as if it hadn't run; tests only (not with DK_NO_TRACE)
extern void (*dk_on_lazy)(int what);

// RAM, as the Z80 sees it
extern uint8_t dk_ram[0xc00];           // 0x6000-0x6bff
extern uint8_t dk_spriteram[0x400];     // 0x7000-0x73ff
extern uint8_t dk_videoram[0x400];      // 0x7400-0x77ff
extern uint8_t dk_flip, dk_sprite_bank, dk_palette_bank, dk_nmi_mask;
extern uint32_t dk_frames;              // VBLANKs so far
extern uint32_t dk_resets;              // watchdog resets so far

// the picture: MAME's screen bitmap (raw, 256 x 224: lines 16-239) in
// pens 0-255; dk_rgb[] is each as MAME's resistor networks make it
void dk_render(uint8_t fb[DK_H][DK_W]);
void dk_render_line(uint8_t *row, int line);  // one line (16-239) from the state now
extern uint32_t dk_rgb[256];            // 0xRRGGBB
// the graphics, decoded: a tile's row (raw orientation), 8 pens of 2 bits,
// pixel x at bits 2x; a sprite's row, 16 pens the same way
extern uint16_t (*dk_tile_rows)[8];      // [256][8], in roms->rows
extern uint32_t (*dk_sprite_rows)[16];   // [128][16], after them
extern const uint8_t *dk_color_codes;   // v-5e: a tile's color by column and row / 4

// sound: what the discrete circuits see, with when (ticks of 76.8 MHz,
// DKSND_TICK_HZ, from power on). The board calls this as MAME's discrete
// device gets its inputs; dksnd.c turns them into samples.
void dk_snd_event(uint64_t when, int node, uint8_t value);
// P1 (the DAC); P2 bit 7; latch 6H bits 0, 1, 2; the discrete device's
// reset; the sound manager's update (50 a second: the sound is made to then)
enum { DK_DAC, DK_DISCHARGE, DK_WALK, DK_JUMP, DK_STOMP, DK_SNDRESET, DK_SNDSYNC };
uint64_t dk_snd_now(void);              // the time now, in those ticks
uint64_t dk_snd_tick(void);             // every event to come is at this time or later

// for the tests
uint64_t dk_cycles(void);               // the Z80's cycles so far
uint64_t dk_snd_cycles(void);           // the MB8884's
uint8_t dk_peek(uint16_t a);            // as the Z80 reads, without side effects
void dk_regs_8257(uint8_t out[16]);     // the 8257's channels (address, count, mode), mode, status, state
extern uint8_t dk_latch_3d, dk_latch_6h, dk_latch_vp2;

#endif
