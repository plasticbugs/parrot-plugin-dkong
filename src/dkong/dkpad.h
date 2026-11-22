// dkpad.h -- the controls: the ColecoVision controller's joystick, fire
// buttons and keypad, or a USB pad, as Donkey Kong's 4-way stick and jump
// button. Portable, header-only. (Pac-Man's pmpad.h, with the jump.)
//
// The stick goes through MAME 0.288's 4-way logic (digital_joystick::
// frame_update in emu/ioport.cpp; dkong.cpp's stick is PORT_4WAY), once a
// list as MAME does it once a frame: up with down, or left with right,
// count as neither; a diagonal keeps the way that just came on (left held,
// then up too: up), and a diagonal straight from the middle, or from the
// other diagonal, goes across (MAME picks the horizontal axis).
//
// The ColecoVision controller: the joystick moves, either fire button
// jumps, keypad * or # is a coin, 1 one player, 2 two players. A USB pad:
// the d-pad or left stick moves, a face button jumps, - is a coin, + one
// player. One coin a press, however long it is held.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../sdk/gamepad.h"

// the ColecoVision keypad's codes, as the kernel reports them
enum { DKKEY_0 = 0xA, DKKEY_1 = 0xD, DKKEY_2 = 0x7, DKKEY_3 = 0xC, DKKEY_STAR = 0x9, DKKEY_HASH = 0x6, DKKEY_NONE = 0xF };

// directions: MAME's joystick bits (JOYDIR_UP, DOWN, LEFT, RIGHT)
enum { DKDIR_UP = 1, DKDIR_DOWN = 2, DKDIR_LEFT = 4, DKDIR_RIGHT = 8 };

// the board's inputs (dk.h, active high)
enum { DKIN0_RIGHT = 0x01, DKIN0_LEFT = 0x02, DKIN0_UP = 0x04, DKIN0_DOWN = 0x08, DKIN0_JUMP = 0x10 };
enum { DKIN2_START1 = 0x04, DKIN2_START2 = 0x08, DKIN2_COIN = 0x80 };

typedef struct {
    uint8_t current, previous, way4;    // MAME's m_current, m_previous, m_current4way
    uint8_t coin_left;                  // frames of the coin pulse still to give the game
    uint8_t coin_wait;                  // frames the coin key must still be up before a press counts
} dkpad_t;

// A coin is a pulse of DKPAD_COIN frames from the press, however long the
// key is held; another needs the key up for DKPAD_COIN_UP frames (one coin
// for a key that bounces)
#define DKPAD_COIN    6
#define DKPAD_COIN_UP 3

// the USB pad's buttons that jump
#define DKPAD_JUMP_BUTTONS (GP_SOUTH | GP_EAST | GP_WEST | GP_NORTH)

// MAME's digital_joystick::frame_update for a 4-way stick: the 8 ways in
// (DKDIR_ bits), the 4-way's out
static inline uint8_t dkpad_4way(dkpad_t *s, unsigned dirs) {
    s->previous = s->current;
    s->current = (uint8_t)(dirs & 15);
    if ((s->current & (DKDIR_UP | DKDIR_DOWN)) == (DKDIR_UP | DKDIR_DOWN)) s->current &= (uint8_t)~(DKDIR_UP | DKDIR_DOWN);
    if ((s->current & (DKDIR_LEFT | DKDIR_RIGHT)) == (DKDIR_LEFT | DKDIR_RIGHT)) s->current &= (uint8_t)~(DKDIR_LEFT | DKDIR_RIGHT);
    if (s->current != s->previous) {
        s->way4 = s->current;
        if ((s->way4 & (DKDIR_UP | DKDIR_DOWN)) && (s->way4 & (DKDIR_LEFT | DKDIR_RIGHT))) s->way4 ^= s->way4 & s->previous;
        if ((s->way4 & (DKDIR_UP | DKDIR_DOWN)) && (s->way4 & (DKDIR_LEFT | DKDIR_RIGHT))) s->way4 &= (uint8_t)~(DKDIR_UP | DKDIR_DOWN);
    } else {
        s->way4 &= s->current;
    }
    return s->way4;
}

// The console's joystick nibble (low = held: bit 0 up, 1 right, 2 down, 3 left) as DKDIR_ bits
static inline unsigned dkpad_joy_dirs(uint8_t joy) {
    return (!(joy & 1) ? DKDIR_UP : 0) | (!(joy & 2) ? DKDIR_RIGHT : 0) | (!(joy & 4) ? DKDIR_DOWN : 0) |
           (!(joy & 8) ? DKDIR_LEFT : 0);
}

// sdk/cvpad.h's cvpad_dirs() (GP_UP ...) as DKDIR_ bits
static inline unsigned dkpad_gp_dirs(uint32_t gp) {
    return (gp & GP_UP ? DKDIR_UP : 0) | (gp & GP_DOWN ? DKDIR_DOWN : 0) | (gp & GP_LEFT ? DKDIR_LEFT : 0) |
           (gp & GP_RIGHT ? DKDIR_RIGHT : 0);
}

// One list's inputs. joy, key: the console's controller as cvf_input()
// gives them; fire: its fire buttons (bit 0 left, bit 1 right, low = held).
// pad_dirs: cvpad_dirs() of a USB pad (0: none), pad_buttons its buttons.
// Sets IN0 (the stick, the jump) and IN2 (the coin, the starts), active
// high as MAME's.
static inline void dkpad_frame(dkpad_t *s, uint8_t joy, uint8_t fire, uint8_t key, uint32_t pad_dirs, uint32_t pad_buttons,
                               uint8_t *in0, uint8_t *in2) {
    uint8_t w = dkpad_4way(s, dkpad_joy_dirs(joy) | dkpad_gp_dirs(pad_dirs));
    uint8_t v0 = 0, v2 = 0;
    if (w & DKDIR_UP) v0 |= DKIN0_UP;
    if (w & DKDIR_LEFT) v0 |= DKIN0_LEFT;
    if (w & DKDIR_RIGHT) v0 |= DKIN0_RIGHT;
    if (w & DKDIR_DOWN) v0 |= DKIN0_DOWN;
    if ((fire & 3) != 3 || (pad_buttons & DKPAD_JUMP_BUTTONS)) v0 |= DKIN0_JUMP;
    bool coin = key == DKKEY_STAR || key == DKKEY_HASH || (pad_buttons & GP_MINUS);
    if (key == DKKEY_1 || (pad_buttons & GP_PLUS)) v2 |= DKIN2_START1;
    if (key == DKKEY_2) v2 |= DKIN2_START2;
    if (coin && !s->coin_wait) s->coin_left = DKPAD_COIN;
    if (coin) s->coin_wait = DKPAD_COIN_UP;
    else if (s->coin_wait) s->coin_wait--;
    if (s->coin_left) { v2 |= DKIN2_COIN; s->coin_left--; }
    *in0 = v0;
    *in2 = v2;
}
