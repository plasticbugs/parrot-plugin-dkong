// dkcv: the cart's side of Donkey Kong on the ColecoVision. Portable C (no
// pico-sdk), used by the plugin and by the Mac tests.
//
// The console runs gbkernel (coleco/gbcv/kernel/gbkernel.asm): each TV
// frame it takes a command list from the cart and runs it. This makes those
// lists, from the board's RAM (dk.h) and its sound (dksnd.h):
//
//   once:                         dkcv_init(e)
//   after dk_init:                dkcv_prepare()
//   at each VBLANK the cart uses: dkcv_picture(e, board)   (or dkcv_canvas for a message)
//   each list:                    dkcv_sound(e, samples, DKCV_PCM_N)
//                                 len = dkcv_tv_frame(e, list)
//
// The picture. MAME shows the board's 256 x 224 turned (ROT270): 224 wide
// and 256 high, 32 rows of 28 tiles. All of it fits the TMS9918's 192
// lines at 3/4 (sdk/cvscale.h): 168 x 192. The scores, the lives, the
// level and the bonus go to the side margins in pieces, as Pac-Man's do,
// and the playfield (rows 2 to 31) sits in the middle (layout[] in
// dkcv.c). Graphics II: each 8 x 1 pixels in two colors.
//
// The board's sprites are scaled one by one (16 x 16 to 12 x 12), so they
// keep their shape as they move. A moving one (Mario first, in two
// colors) gets a TMS9918 sprite for its main color where a line has room
// (4 a line, never more, so nothing flickers); the rest of it, and every
// sprite that stays put (Donkey Kong, Pauline), goes in the bitmap.
//
// A list carries the bytes that differ from what the console holds, the
// sprites' first, as many as the frame has time for.
//
// The sound. dksnd.c's samples (the sound board, as MAME computes it) go
// out as DKCV_PCM_N a list through the SN76489's three tones parked at a
// period of 1, their attenuators summed as a DAC (dkdac3.h, Movies' and
// Dragon's Lair's way). The console runs the lists back to back, so a
// list's length is the sample clock: DKCV_LIST_T T, 100 samples, 5,962 a
// second.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dk.h"
#include "dksnd.h"

#define DKCV_W      256
#define DKCV_H      192
#define DKCV_BYTES  6144            // pattern bytes, and as many color bytes
#define DKCV_LIST_MAX 3000
#define DKCV_PCM_N  100             // samples a list
#define DKCV_LIST_T 60036           // a list's length in the console's T (3,579,545 a second)
#define DKCV_ENTRIES 128            // the board's sprites (a bank of sprite RAM)
#define DKCV_MARIO  19              // Mario's (0x694C, copied to sprite RAM by the 8257)
// the most tiles a picture draws again; the rest, rows at a time, go to the
// next (dkrun.c skips a list between)
#define DKCV_TILES 480
// the board's Z80 cycles (3.072 MHz) in a list, << 16
#define DKCV_LIST_CYCLES_Q16 ((uint64_t)DKCV_LIST_T * DK_CLOCK * 65536u / 3579545u)
// the samples' rate, << 8: DKCV_PCM_N a list
#define DKCV_RATE_Q8 ((uint32_t)((uint64_t)DKCV_PCM_N * 3579545u * 256u / DKCV_LIST_T))
// a step of dksnd's circuits in its ticks << 16, with `over` steps a sample
#define DKCV_STEP_Q16(over) ((uint64_t)DKSND_TICK_HZ * DKCV_LIST_T * 65536u / (3579545u * DKCV_PCM_N * (uint64_t)(over)))

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define DKCV_HOT __attribute__((section(".time_critical.dkcv")))
#else
#define DKCV_HOT
#endif

typedef struct {
    uint32_t tv_frames, pictures, pat_bytes, col_bytes, spr_bytes, runs, refreshed, pcm_frames;
    uint32_t settled;               // lists that ended with nothing left to send
    uint32_t t_max;                 // the longest list, in T
    uint32_t tiles;                 // tiles drawn again
    uint32_t deferred;              // pictures that left changed tiles to the next
    uint32_t cells;                 // 8 x 1 places made again
    uint32_t hw, hw_refused;        // TMS9918 sprites given, and moving ones a line had no room for
    uint32_t clipped;               // samples the limiter took in
} dkcv_stats_t;

typedef struct {
    // the console's VRAM as the lists so far leave it, and the newest picture
    uint8_t vram_pat[DKCV_BYTES], vram_col[DKCV_BYTES];
    uint8_t want_pat[DKCV_BYTES], want_col[DKCV_BYTES];
    uint8_t vram_sat[128], want_sat[128];       // the sprites' attributes (0x1B00)
    uint8_t vram_spat[64][32], want_spat[64][32];   // their patterns, 16 x 16 (0x3800)
    uint64_t spat_dirty;            // the patterns that differ (a bit each)
    bool regs_done;                 // the sprites' registers set (16 x 16, their tables)
    bool names_done;
    uint16_t names_at;
    uint16_t cursor;                // where the next list's scan starts
    uint16_t refresh_at;
    bool dirty;                     // want differs from vram somewhere (or may)
    uint32_t bands;                 // the same, for each band of 8 lines (256 bytes)

    // the picture as it was made last
    bool pic_valid;
    uint8_t flip, pal;              // the board's, at the last picture
    uint8_t vram_seen[0x400];       // the board's video RAM the tiles were drawn from
    uint32_t pending[32];           // tiles changed but not drawn yet (DKCV_TILES), row by row
    uint8_t row_next;               // the row the next picture starts at
    uint16_t tiles_most;            // the most tiles the next picture draws (0: DKCV_TILES; dkrun.c's)
    uint32_t cells_prev[24];        // the 8 x 8 places the sprites were in
    int16_t ex[DKCV_ENTRIES], ey[DKCV_ENTRIES];     // each sprite's place last time (-32768: not shown)
    bool hw[DKCV_ENTRIES];          // had a TMS9918 sprite last time

    // the sound
    bool pcm;                       // the lists carry samples (the game is on)
    uint8_t pcm_gain;               // 0-32: the DAC comes up over the first lists
    int32_t hp_x, hp_y;             // the high-pass's last input and output (<< 8)
    uint8_t pcm_bytes[DKCV_PCM_N][3];  // each sample's three attenuators, in the order written
    uint8_t dac_att[3];             // the attenuators as the last sample left them
    uint8_t tone_bytes[8];
    uint8_t tone_n;
    uint8_t resync;
    dkcv_stats_t stats;
} dkcv_t;

void dkcv_init(dkcv_t *e);
// the colors of the board's pens and its tiles turned, from its PROMs and
// ROMs (after dk_init), before the first picture: on the cart they take
// about 120 ms, so the plugin makes them before the game's lists start
// (the first picture makes them otherwise)
void dkcv_prepare(void);
// room the plugin may use before the first dkcv_picture (a message's
// canvas, the card's reads); dkcv_canvas and dkcv_sound leave it alone
uint8_t *dkcv_scratch(size_t *n);

// what a picture is made from: the board's tile RAM, its sprite RAM (the
// bank shown), and the flip and palette bank, at a VBLANK
typedef struct {
    uint8_t vram[0x400], spr[0x200];
    uint8_t flip, pal;
} dkcv_board_t;
void dkcv_board(dkcv_board_t *b);       // the board as it is
// a new picture of b; false: it left tiles for the next
bool dkcv_picture(dkcv_t *e, const dkcv_board_t *b);
// a new picture: 256 x 192 pixels, a TMS9918 color each (1-15); for messages
void dkcv_canvas(dkcv_t *e, const uint8_t *px);

// The next list's sound: n samples at DKCV_RATE_Q8, MAME's +-1.0 as
// +-32767; n = 0: none (a message: the console waits for the vertical
// blank again, and the DAC is let go)
void dkcv_sound(dkcv_t *e, const int16_t *s, int n);

// the next list into out (DKCV_LIST_MAX bytes); returns its length
size_t dkcv_tv_frame(dkcv_t *e, uint8_t *out);

// the picture the console shows once the lists so far have run, sprites
// and all, a TMS9918 color a pixel (tests, previews)
void dkcv_screen(const dkcv_t *e, uint8_t *px);
// the picture wanted, the same way
void dkcv_wanted(const dkcv_t *e, uint8_t *px);
// the picture before the TMS9918's limits: the board's colors as TMS9918
// colors, scaled and laid out, every pixel its own color (tests, previews)
void dkcv_ideal(uint8_t *px);
// the tiles as scaled and laid out so far, the board's pens (tests); the
// layout's pieces of MAME's picture: its tiles' columns and rows, and where
// the piece goes
void dkcv_scaled(uint8_t *px);
int dkcv_pieces(void);
void dkcv_layout(int i, int *c0, int *r0, int *nc, int *nr, int *x, int *y);
// a tile as dkcv draws it: turned (row v's pens, pixel u at bits 2u)
const uint16_t *dkcv_turned(int flip, int code);
// bytes the console still needs (tests; a whole scan)
uint32_t dkcv_backlog(const dkcv_t *e);
// the DAC's level for the three tones' attenuators (tests: the lists' sound as a WAV)
float dkcv_dac_level(int att0, int att1, int att2);
// the TMS9918's colors as 0xRRGGBB (previews)
extern const uint32_t dkcv_tms_rgb[16];
// the board's pens (dk_rgb) as TMS9918 colors
extern uint8_t dkcv_pen[256];
// moving sprites' main colors as the TMS9918's sprites (true), or all in
// the bitmap (tests: the two compared)
extern bool dkcv_sprites;
