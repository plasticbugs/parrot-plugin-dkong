// dkcv.c -- see dkcv.h. The lists and the sound are Pac-Man's (../pacman/
// pmcv.c, from Q*bert's qbcv.c); the layout and the sprites are new.
#include "dkcv.h"

#include <math.h>
#include <string.h>

#include "../sdk/cvscale.h"
#include "dkdac3.h"

// ---- lists (qbcv.c's, from gbkernel.asm's costs) ---------------------------------
#define FRAME_T    (262 * 228)
#define VBLANK_T   (70 * 228)
#define C_START    480
#define C_RUN_VB   106
#define C_BYTE_VB  23
#define C_RUN_ACT  106
#define C_BYTE_ACT 31
#define C_PSG      75
#define C_PSG_BYTE 52
#define C_REG      114
#define C_END_VB   54
#define C_END      52
#define VB_LIMIT   (VBLANK_T - 600)
#define LIST_LIMIT (FRAME_T - 1200)

// The console goes from one list straight into the next, so a list's
// length is the sample clock (qbcv.c's)
#define C_FRAME    357
#define PCM_L      DKCV_LIST_T
#define PCM_FIRST  (C_FRAME + C_END_VB)
#define C_SAMPLE   (C_PSG + 3 * C_PSG_BYTE)
#define PCM_STEP   (((PCM_L - PCM_FIRST - C_SAMPLE - C_END) << 8) / (DKCV_PCM_N - 1))

#define PATTERNS 0x0000
#define COLORS   0x2000
#define NAMES    0x1800
#define SAT      0x1B00
#define SPAT     0x3800
#define BLACK    1

typedef struct {
    uint8_t *b;
    size_t n;
    uint32_t t;
    bool vblank;
} list_t;

static void l_run_head(list_t *l, uint16_t addr, int n) {
    l->t += l->vblank ? C_RUN_VB + n * C_BYTE_VB : C_RUN_ACT + n * C_BYTE_ACT;
    l->b[l->n++] = 1;
    l->b[l->n++] = (uint8_t)addr;
    l->b[l->n++] = (uint8_t)((addr >> 8) & 0x3F) | 0x40;
    l->b[l->n++] = (uint8_t)n;
}

static void l_psg(list_t *l, const uint8_t *d, int n) {
    l->t += C_PSG + C_PSG_BYTE * n;
    l->b[l->n++] = 2;
    l->b[l->n++] = (uint8_t)n;
    memcpy(l->b + l->n, d, n);
    l->n += n;
}

static void l_reg(list_t *l, uint8_t r, uint8_t v) {
    l->t += C_REG;
    l->b[l->n++] = 3;
    l->b[l->n++] = v;
    l->b[l->n++] = 0x80 | r;
}

static void l_end_vblank(list_t *l) {
    if (!l->vblank) return;
    l->vblank = false;
    l->t += C_END_VB;
    l->b[l->n++] = 0;
}

static int room(const list_t *l) {
    int t = l->vblank ? ((int)VB_LIMIT - (int)l->t - C_END_VB - C_RUN_VB) / C_BYTE_VB
                      : ((int)LIST_LIMIT - (int)l->t - C_END - C_RUN_ACT) / C_BYTE_ACT;
    return t > 255 ? 255 : t;
}

// ---- colors -----------------------------------------------------------------------
const uint32_t dkcv_tms_rgb[16] = {0x000000, 0x000000, 0x21C842, 0x5EDC78, 0x5455ED, 0x7D76FC, 0xD4524D, 0x42EBF5,
                                   0xFC5554, 0xFF7978, 0xD4C154, 0xE6CE80, 0x21B03B, 0xC95BBA, 0xCCCCCC, 0xFFFFFF};

static float tms_lab[16][3];
static uint16_t nearer[16][16];         // [c][a] bit b: c is nearer a than b
#define NEARER(c, a, b) ((nearer[c][a] >> (b)) & 1)
static bool colors_ready;

static void oklab(uint32_t rgb, float *out) {
    float c[3];
    for (int i = 0; i < 3; i++) {
        float v = (float)((rgb >> (16 - 8 * i)) & 255) / 255.0f;
        c[i] = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    }
    float l = cbrtf(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    float m = cbrtf(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    float s = cbrtf(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    out[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

static float d2(const float *a, const float *b) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

static void colors_init(void) {
    for (int i = 1; i < 16; i++) oklab(dkcv_tms_rgb[i], tms_lab[i]);
    for (int c = 1; c < 16; c++)
        for (int a = 1; a < 16; a++)
            for (int b = 1; b < 16; b++)
                if (d2(tms_lab[c], tms_lab[a]) <= d2(tms_lab[c], tms_lab[b])) nearer[c][a] |= (uint16_t)(1u << b);
    colors_ready = true;
}


// The board's pens (dk_rgb: the PROMs through MAME's resistor networks) as
// TMS9918 colors: the nearest in OKLab (made at the first picture, once
// the board has its PROMs)
uint8_t dkcv_pen[256];
static bool pens_ready;

static void pens_init(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t rgb = dk_rgb[i];
        float lab[3];
        oklab(rgb, lab);
        uint8_t best = 1;
        float bd2 = 1e9f;
        for (int t = 1; t < 16; t++) {
            float d = d2(lab, tms_lab[t]);
            if (d < bd2) { bd2 = d; best = (uint8_t)t; }
        }
        dkcv_pen[i] = best;
    }
    pens_ready = true;
}

// ---- the layout ------------------------------------------------------------------
// MAME's picture, turned (224 x 256, 28 x 32 tiles), in pieces of tiles,
// each scaled by 3/4 to its place on the TMS9918's screen. The playfield
// (rows 2 to 31, 168 x 180) in the middle, its rows where they are but
// 6 lines up; the scores, the lives, the level and the bonus to the sides,
// in the 40 pixels either side that no place of the playfield shares (5 of
// the TMS9918's 8-pixel places): 1UP and its score, HIGH SCORE and its
// score and the lives on the left, 2UP and its score, the level (L=01)
// and the bonus box on the right. Their tiles are left out of the
// playfield. A score is 6 digits, 36 pixels.
typedef struct { uint8_t c0, r0, nc, nr; uint8_t x, y; } piece_t;
#define PF_X  44
#define PF_TOP 2                        // the playfield's first row
#define PF_Y  6                         // its first line
#define PX(c) (PF_X + 6 * (c))
#define PY(r) (PF_Y + 6 * ((r) - PF_TOP))
#define LEFT  2
#define RIGHT 218
static const piece_t layout[] = {
    {1, 0, 6, 2, LEFT, 6},              // "1UP", its score
    {9, 0, 4, 1, LEFT + 6, 26},         // "HIGH"
    {14, 0, 5, 1, LEFT + 3, 33},        // "SCORE"
    {11, 1, 6, 1, LEFT, 40},            // the high score
    {1, 3, 6, 1, LEFT, 54},             // the lives
    {21, 0, 6, 2, RIGHT, 6},            // "2UP", its score
    {21, 3, 4, 1, RIGHT + 6, 26},       // the level
    {21, 5, 6, 3, RIGHT, 40},           // the bonus box
    // the playfield, but those
    {0, 2, 28, 1, PX(0), PY(2)},
    {0, 3, 1, 1, PX(0), PY(3)},
    {7, 3, 14, 1, PX(7), PY(3)},
    {25, 3, 3, 1, PX(25), PY(3)},
    {0, 4, 28, 1, PX(0), PY(4)},
    {0, 5, 21, 3, PX(0), PY(5)},
    {27, 5, 1, 3, PX(27), PY(5)},
    {0, 8, 28, 24, PX(0), PY(8)},
};
#define PIECES (int)(sizeof layout / sizeof layout[0])
#define PF_W (28 * 6)
#define PF_H (30 * 6)

static int8_t tile_piece[32][28];       // the piece each of MAME's tiles is in (-1: none)

// Each of the board's sprites, turned and scaled by itself (16 x 16 to 12
// x 12), kept while its code, color and flips stay the same (the cache,
// below), and those on the screen in a picture (vis)
typedef struct {
    uint32_t key;                       // what it was made from (0: nothing)
    uint8_t n, col[3];                  // its colors (TMS9918): its pens', 3 at most (cvscale34 only picks)
    uint16_t at[3][12];                 // where each is: bit c of row r
} scaled_t;

typedef struct {
    uint8_t e;                          // the entry (sprite RAM / 4)
    int16_t x, y;                       // its 12 x 12's top left on the TMS9918's screen
    int16_t x0, y0, x1, y1;             // that, clipped to the playfield
    const scaled_t *img;
    uint16_t top[12];                   // where it shows: bit c of row r, column c (in its place, a pen, none after it with one there)
    uint16_t hw_m;                      // its colors in TMS9918 sprites (bit c: color c)
    uint8_t hw_n, hw_c[2];              // them, as a count and colors
} vis_t;

// The picture's state, kept from one to the next: the room the plugin has
// before the game (dkcv_scratch: a message's canvas, the card's reads), and
// made as new at the first picture. Each part is described where it's used.
static union {
    struct {
        uint8_t small[DKCV_H][DKCV_W / 2];
        scaled_t cache[DKCV_ENTRIES];
        uint16_t turned[2][256][8];
        vis_t vis[DKCV_ENTRIES];
        uint32_t ov_hash[24][32];
        uint8_t strip[8][224];
        uint8_t scaled_rows[6][DKCV_W];
        uint16_t hw_pat[32][16];
        uint8_t sband[8][DKCV_W];
    } p;
    uint8_t room[48 * 1024];
    uint64_t align;                     // (the plugin keeps the card's state in it)
} pic_u;

// a row of MAME's tiles, the right way up (the board's pens); the picture
// scaled and laid out, as TMS9918 colors, two pixels a byte (x even in the
// low half); a row of tiles scaled, in pens, before it goes in
#define strip pic_u.p.strip
#define small pic_u.p.small
#define SMALL(y, x) ((small[y][(x) >> 1] >> (((x) & 1) * 4)) & 15)
#define scaled_rows pic_u.p.scaled_rows
static bool layout_ready, small_clear, turned_ready;

static void layout_init(void) {
    memset(tile_piece, -1, sizeof tile_piece);
    for (int p = 0; p < PIECES; p++)
        for (int r = layout[p].r0; r < layout[p].r0 + layout[p].nr; r++)
            for (int c = layout[p].c0; c < layout[p].c0 + layout[p].nc; c++) tile_piece[r][c] = (int8_t)p;
    layout_ready = true;
}

uint8_t *dkcv_scratch(size_t *n) {
    *n = sizeof pic_u.room;
    small_clear = false;
    turned_ready = false;
    return pic_u.room;
}

// ---- the 8 x 8 places to make again ----------------------------------------------------
static uint32_t cells[24];              // a bit a column, a word a band

static void mark(int x0, int y0, int x1, int y1) {     // pixels x0..x1 - 1, y0..y1 - 1
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DKCV_W) x1 = DKCV_W;
    if (y1 > DKCV_H) y1 = DKCV_H;
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t m = (x1 + 7) / 8 >= 32 ? ~0u : (1u << ((x1 + 7) / 8)) - 1;
    m &= ~((1u << (x0 / 8)) - 1);
    for (int b = y0 / 8; b <= (y1 - 1) / 8; b++) cells[b] |= m;
}

// The board's tiles turned (MAME's ROT270): the picture's tile at row R,
// column C is tile RAM's (29 - C) * 32 + R with the screen flipped (as the
// upright cabinet has it), (C + 2) * 32 + 31 - R without; here row v's
// pens of a tile, pixel u at bits 2u, both ways (made from dk_tile_rows at
// the first picture)
#define turned pic_u.p.turned

// four of a row's 2-bit pens (a byte of turned's) as four bytes, the first lowest
static uint32_t pens4[256];

static void turn_tiles(void) {
    for (int b = 0; b < 256; b++)
        pens4[b] = (uint32_t)(b & 3) | (uint32_t)(b >> 2 & 3) << 8 | (uint32_t)(b >> 4 & 3) << 16 | (uint32_t)(b >> 6 & 3) << 24;
    for (int t = 0; t < 256; t++)
        for (int v = 0; v < 8; v++) {
            uint16_t w0 = 0, w1 = 0;
            for (int u = 0; u < 8; u++) {
                w1 |= (uint16_t)(((dk_tile_rows[t][7 - u] >> (2 * v)) & 3) << (2 * u));
                w0 |= (uint16_t)(((dk_tile_rows[t][u] >> (2 * (7 - v))) & 3) << (2 * u));
            }
            turned[0][t][v] = w0;
            turned[1][t][v] = w1;
        }
    turned_ready = true;
}

const uint16_t *dkcv_turned(int flip, int code) {
    if (!turned_ready) turn_tiles();
    return turned[flip ? 1 : 0][code & 0xFF];
}

static const dkcv_board_t *bd;          // the board the picture is made from

static inline int tile_index(int flip, int r, int c) { return flip ? (29 - c) * 32 + r : (c + 2) * 32 + 31 - r; }

// a tile of MAME's picture into the strip, from the board's tile RAM and
// the color PROM (by the tile's column and its row / 4, dkong_v.cpp)
static void draw_tile(int r, int c) {
    int i = tile_index(bd->flip, r, c);
    const uint16_t *rows = turned[bd->flip ? 1 : 0][bd->vram[i]];
    int color = (dk_color_codes[i % 32 + 32 * (i / 32 / 4)] & 0x0f) + 0x10 * bd->pal;
    uint32_t base = (uint32_t)(color * 4) * 0x01010101u;        // (124 + 3 at most: no carry)
    for (int v = 0; v < 8; v++) {
        unsigned w = rows[v];
        uint32_t lo = pens4[w & 0xff] + base, hi = pens4[w >> 8] + base;
        memcpy(&strip[v][c * 8], &lo, 4);
        memcpy(&strip[v][c * 8 + 4], &hi, 4);
    }
}

// Row r's changed tiles (a bit a column) scaled again, with the tiles
// either side of each, which look at its edge: in each piece the row is
// in, from the first to the last of them, drawn into the strip with one
// more tile each side to look at
static void scale_row(int r, uint32_t changed) {
    for (int p = 0; p < PIECES; p++) {
        const piece_t *pc = &layout[p];
        if (r < pc->r0 || r >= pc->r0 + pc->nr) continue;
        uint32_t in = ((1u << pc->nc) - 1) << pc->c0, m = changed & in;
        if (!m) continue;
        int lo = __builtin_ctz(m) - 1, hi = 31 - __builtin_clz(m) + 1;
        if (lo < pc->c0) lo = pc->c0;
        if (hi > pc->c0 + pc->nc - 1) hi = pc->c0 + pc->nc - 1;
        for (int c = lo - 1; c <= hi + 1; c++)
            if (c >= pc->c0 && c < pc->c0 + pc->nc) draw_tile(r, c);
        cvscale34(&strip[0][pc->c0 * 8], 224, pc->nc * 8, 8, (lo - pc->c0) * 8, (hi + 1 - pc->c0) * 8,
                  &scaled_rows[0][pc->x], DKCV_W);
        // in as colors, from the first column cvscale34 made to the last
        int x0 = pc->x + (lo - pc->c0) * 6, x1 = pc->x + (hi + 1 - pc->c0) * 6, y0 = pc->y + (r - pc->r0) * 6;
        for (int v = 0; v < 6; v++) {
            uint8_t *d = small[y0 + v];
            const uint8_t *sr = scaled_rows[v];
            int x = x0, xe = x1;
            if (x & 1) { d[x >> 1] = (uint8_t)((d[x >> 1] & 0x0F) | dkcv_pen[sr[x]] << 4); x++; }
            if (xe & 1) { xe--; d[xe >> 1] = (uint8_t)((d[xe >> 1] & 0xF0) | dkcv_pen[sr[xe]]); }
            for (; x < xe; x += 2) d[x >> 1] = (uint8_t)(dkcv_pen[sr[x]] | dkcv_pen[sr[x + 1]] << 4);
        }
        for (uint32_t k = (m | m << 1 | m >> 1) & in; k; k &= k - 1) {
            int x = pc->x + (__builtin_ctz(k) - pc->c0) * 6, y = pc->y + (r - pc->r0) * 6;
            mark(x, y, x + 6, y + 6);
        }
    }
}

// ---- the sprites ------------------------------------------------------------------------
#define cache pic_u.p.cache
#define vis pic_u.p.vis
static int nvis;
#define ov_hash pic_u.p.ov_hash         // a hash of each 8 x 8 of the sprites over the tiles, as the last picture left it

// Which sprite shows at a pixel is the last in vis whose 12 x 12 has a pen
// there (0 is see-through), in its place. That isn't kept as a picture:
// each sprite's pixels that show are worked out (top) from those after it
// that it meets, and the 8 x 8 places from them when they're needed.
static void tops(void) {
    for (int k = 0; k < nvis; k++) {    // less those after it (theirs not yet taken from)
        vis_t *v = &vis[k];
        for (int j = k + 1; j < nvis; j++) {
            const vis_t *w = &vis[j];
            int dx = w->x - v->x, dy = w->y - v->y;
            if (dx <= -12 || dx >= 12 || dy <= -12 || dy >= 12) continue;
            for (int r = dy > 0 ? dy : 0; r < 12 && r - dy < 12; r++) {
                uint32_t o = w->top[r - dy];
                v->top[r] &= (uint16_t)~(dx >= 0 ? o << dx : o >> -dx);
            }
        }
    }
}
// The sprites over band b's lines, painted in sband[] (only in the places
// marked in `at`, a bit a column, each cleared first): each pixel the
// color of the sprite that shows there (0x10 too: a TMS9918 sprite shows
// it), 0 none
#define sband pic_u.p.sband
DKCV_HOT static void paint_band(int b, uint32_t at) {
    for (uint32_t m = at; m; m &= m - 1) {
        int cx = __builtin_ctz(m);
        for (int k = 0; k < 8; k++) memset(&sband[k][cx * 8], 0, 8);
    }
    int cy0 = b * 8;
    for (int q = 0; q < nvis; q++) {
        const vis_t *v = &vis[q];
        if (v->y0 >= cy0 + 8 || cy0 >= v->y1) continue;
        int y0 = v->y0 > cy0 ? v->y0 : cy0, y1 = v->y1 < cy0 + 8 ? v->y1 : cy0 + 8;
        const scaled_t *im = v->img;
        uint8_t out[3];
        for (int i = 0; i < im->n; i++) out[i] = (uint8_t)(im->col[i] | ((v->hw_m >> im->col[i]) & 1) << 4);
        for (int y = y0; y < y1; y++) {
            uint32_t t = v->top[y - v->y];
            if (!t) continue;
            uint8_t *row = &sband[y - cy0][v->x];        // (its pixels that show are in the playfield, x >= PF_X)
            for (int i = 0; i < im->n; i++)
                for (uint32_t m = t & im->at[i][y - v->y]; m; m &= m - 1) row[__builtin_ctz(m)] = out[i];
        }
    }
}

bool dkcv_sprites = true;

static int fdiv4(int a) { return a >= 0 ? a / 4 : -((-a + 3) / 4); }

// Where MAME's dkong_v.cpp draws sprite RAM's entry at offs, turned: lines
// L .. L + 15 (the picture's x, + 16) and columns 240 - x .. (its y); row
// u of the turned 16 x 16 is the sprite's line sy(u), column v its pixel
// 15 - v. The flips as draw_sprites has them.
static void entry_image(const uint8_t *s, int flip, uint8_t img[16][16], int *X0, int *Y0) {
    int y = s[0], code = (s[1] & 0x7f) + ((s[2] & 0x40) << 1), color = (s[2] & 0x0f) + 16 * bd->pal;
    bool flipx = s[2] & 0x80, flipy = s[1] & 0x80;
    int x = (s[3] + 0xf8) & 0xff, first;
    if (flip) {
        x = (x ^ 0xff) - 15;
        flipx = !flipx;
        first = (y - 7) & 0xff;         // line y - 7 is the sprite's row 15
    } else {
        first = (247 - y) & 0xff;       // its row 0
    }
    *X0 = first - 16;
    *Y0 = 240 - x;
    if (!img) return;
    for (int u = 0; u < 16; u++) {
        int sy = flip ? 15 - u : u;
        uint32_t bits = dk_sprite_rows[code & 0x7f][flipy ? 15 - sy : sy];
        for (int v = 0; v < 16; v++) {
            int i = 15 - v;
            int pen = (bits >> (2 * (flipx ? 15 - i : i))) & 3;
            img[v][u] = pen ? (uint8_t)(color * 4 + pen) : 0;
        }
    }
}

// the board's sprites as the picture's: where each is, scaled, clipped
// to the playfield
static void find_sprites(void) {
    nvis = 0;
    for (int e = 0; e < DKCV_ENTRIES; e++) {
        const uint8_t *s = &bd->spr[4 * e];
        int X0, Y0;
        entry_image(s, bd->flip, NULL, &X0, &Y0);
        // on the screen at all: lines 16-239, and the playfield's rows
        if (X0 <= -16 || X0 >= 224 || Y0 <= PF_TOP * 8 - 16 || Y0 >= 256) continue;
        vis_t *v = &vis[nvis];
        v->e = (uint8_t)e;
        v->x = (int16_t)(PF_X + fdiv4(3 * X0 + 2));
        v->y = (int16_t)(PF_Y + fdiv4(3 * (Y0 - PF_TOP * 8) + 2));
        v->x0 = v->x < PF_X ? PF_X : v->x;
        v->y0 = v->y < PF_Y ? PF_Y : v->y;
        v->x1 = v->x + 12 > PF_X + PF_W ? PF_X + PF_W : v->x + 12;
        v->y1 = v->y + 12 > PF_Y + PF_H ? PF_Y + PF_H : v->y + 12;
        if (v->x0 >= v->x1 || v->y0 >= v->y1) continue;
        uint32_t key = 1u << 24 | (uint32_t)bd->pal << 17 | (uint32_t)(bd->flip ? 1 : 0) << 16 | (uint32_t)s[2] << 8 | s[1];
        scaled_t *c = &cache[e];
        if (c->key != key) {
            uint8_t img[16][16], sc[12][12];
            entry_image(s, bd->flip, img, &X0, &Y0);
            cvscale34(&img[0][0], 16, 16, 16, 0, 16, &sc[0][0], 12);
            memset(c->at, 0, sizeof c->at);
            c->n = 0;
            for (int r = 0; r < 12; r++)
                for (int u = 0; u < 12; u++) {
                    if (!sc[r][u]) continue;
                    uint8_t col = dkcv_pen[sc[r][u]];
                    int q = 0;
                    while (q < c->n && c->col[q] != col) q++;
                    if (q == c->n) {
                        if (q == 3) continue;       // (can't be: a sprite has 3 pens)
                        c->col[c->n++] = col;
                    }
                    c->at[q][r] |= (uint16_t)(1u << u);
                }
            c->key = key;
        }
        // its pens in its place (top: less those after it, in tops())
        uint16_t cols = (uint16_t)(((1u << (v->x1 - v->x)) - 1) & ~((1u << (v->x0 - v->x)) - 1));
        uint16_t any = 0;
        for (int r = 0; r < 12; r++) {
            int y = v->y + r;
            v->top[r] = y >= v->y0 && y < v->y1 ? (c->at[0][r] | c->at[1][r] | c->at[2][r]) & cols : 0;
            any |= v->top[r];
        }
        if (!any) continue;
        v->img = c;
        v->hw_n = 0;
        v->hw_m = 0;
        nvis++;
    }
}

// ---- the picture -------------------------------------------------------------------
// 8 pixels (TMS9918 colors; 0: the sprite covers it, any color will do,
// with `under` the color it would show) as a pattern byte and a color
// byte, the color byte kept as the console has it where it will do
static inline __attribute__((always_inline)) void span(dkcv_t *e, unsigned a, const uint8_t *c, const uint8_t *under) {
    uint8_t vc = e->vram_col[a], vp = e->vram_pat[a];
    uint8_t c0 = c[0];
    if (c0 && c[1] == c0 && c[2] == c0 && c[3] == c0 && c[4] == c0 && c[5] == c0 && c[6] == c0 && c[7] == c0) {
        // one color, all shown (as below: kept as the console has it if
        // either of its colors is this one)
        uint8_t vf = vc >> 4, vb = vc & 15;
        if (vf == c0 || vb == c0) {
            e->want_col[a] = vc;
            e->want_pat[a] = vf == vb ? vp : vf == c0 ? 0xFF : 0x00;
        } else {
            e->want_col[a] = (uint8_t)(c0 << 4 | c0);
            e->want_pat[a] = vp;
        }
        return;
    }
    if (c0) {                           // two colors, all shown (the general case's answer)
        uint8_t d = 0;
        unsigned m0 = 0;
        int i = 0;
        for (; i < 8; i++) {
            uint8_t x = c[i];
            if (x == c0) m0 |= 0x80u >> i;
            else if (!x || (d && x != d)) break;
            else d = x;
        }
        if (i == 8) {
            static const uint8_t pop4[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
            int n0 = pop4[m0 & 15] + pop4[m0 >> 4];     // (the M33 has no popcount instruction)
            uint8_t a1 = n0 > 4 ? c0 : n0 < 4 ? d : (c0 < d ? c0 : d), a2 = a1 == c0 ? d : c0;
            uint8_t vf = vc >> 4, vb = vc & 15, fg, bg;
            if (vf == a1 && vb == a2) { fg = a1; bg = a2; }
            else { fg = a2; bg = a1; }
            e->want_pat[a] = (uint8_t)(fg == c0 ? m0 : ~m0 & 0xFF);
            e->want_col[a] = (uint8_t)(fg << 4 | bg);
            return;
        }
    }
    uint8_t cnt[16] = {0};
    int shown = 0;
    for (int i = 0; i < 8; i++)
        if (c[i]) cnt[c[i]]++, shown++;
    if (!shown) {                       // all covered: as it is, if the console has it so
        if (vc) { e->want_pat[a] = vp; e->want_col[a] = vc; return; }
        c = under;
        for (int i = 0; i < 8; i++) cnt[c[i]]++;
    }
    // the two most used (ties: the lower color), the second 0 if one
    uint8_t a1 = 0, a2 = 0;
    for (int k = 1; k < 16; k++)
        if (cnt[k] > cnt[a1] || (!a1 && cnt[k])) a1 = (uint8_t)k;
    for (int k = 1; k < 16; k++)
        if (k != a1 && cnt[k] && (!a2 || cnt[k] > cnt[a2])) a2 = (uint8_t)k;
    uint8_t fg, bg;
    uint8_t vf = vc >> 4, vb = vc & 15;
    if (!a2) {                          // one color
        if (vf == a1 || vb == a1) { fg = vf; bg = vb; }
        else { fg = a1; bg = a1; }
    } else if (vf == a1 && vb == a2) { fg = a1; bg = a2; }
    else if (vf == a2 && vb == a1) { fg = a2; bg = a1; }
    else { fg = a2; bg = a1; }
    uint8_t pat = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t x = c[i];
        bool on;
        if (!x) {                       // covered: the console's bit, if it shows what's under well enough
            uint8_t u = under[i];
            bool keep = (vp >> (7 - i)) & 1;
            uint8_t kc = keep ? fg : bg;
            if (kc == u || fg == bg) on = keep;
            else on = u == fg || (u != bg && NEARER(u, fg, bg));
        } else {
            on = x == fg || (x != bg && fg != bg && NEARER(x, fg, bg));
            if (fg == bg) on = (vp >> (7 - i)) & 1;
        }
        pat = (uint8_t)(pat << 1 | on);
    }
    e->want_pat[a] = pat;
    e->want_col[a] = (uint8_t)(fg << 4 | bg);
}

// an 8 x 8 place with sprites in it (band b painted), made again
DKCV_HOT static void make_over(dkcv_t *e, int b, int cx) {
    uint8_t c[8], u[8];
    e->bands |= 1u << b;
    for (int k = 0; k < 8; k++) {
        int y = b * 8 + k;
        for (int i = 0; i < 8; i++) {
            int x = cx * 8 + i;
            uint8_t bgc = SMALL(y, x), o = sband[k][x];
            u[i] = bgc;
            c[i] = !o ? bgc : (o & 0x10) ? 0 : o & 15;
        }
        span(e, (unsigned)(b * 256 + cx * 8 + k), c, u);
        e->stats.cells++;
    }
}

// the 8 x 8 places still marked in cells[] (none with sprites: those are
// made as they're looked at, in place_sprites), made again
DKCV_HOT static void make_cells(dkcv_t *e) {
    uint8_t c[8];
    for (int b = 0; b < 24; b++) {
        if (!cells[b]) continue;
        e->bands |= 1u << b;
        for (uint32_t m = cells[b]; m; m &= m - 1) {
            int cx = __builtin_ctz(m);
            for (int k = 0; k < 8; k++) {
                const uint8_t *row = &small[b * 8 + k][cx * 4];
                for (int i = 0; i < 4; i++) {
                    c[2 * i] = row[i] & 15;
                    c[2 * i + 1] = row[i] >> 4;
                }
                span(e, (unsigned)(b * 256 + cx * 8 + k), c, c);
            }
            e->stats.cells += 8;
        }
        cells[b] = 0;
    }
}

// n bytes the same (n a multiple of 4; newlib's memcmp goes a byte at a time)
static inline bool same(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t x, y;
        memcpy(&x, p + i, 4);
        memcpy(&y, q + i, 4);
        if (x != y) return false;
    }
    return true;
}

// ---- the sprites' tables ---------------------------------------------------------------
// The patterns go in slots the console's attributes don't use, so a
// sprite never shows a pattern half sent; the attributes go after them.
static int pattern_slot(dkcv_t *e, const uint8_t *p, uint64_t used) {
    for (int s = 0; s < 64; s++)
        if (!(used >> s & 1) && same(e->want_spat[s], p, 32)) return s;
    uint64_t busy = used;
    for (int k = 0; k < 32 && e->vram_sat[4 * k] != 0xD0; k++) busy |= 1ull << (e->vram_sat[4 * k + 2] >> 2);
    for (int k = 0; k < 32 && e->want_sat[4 * k] != 0xD0; k++) busy |= 1ull << (e->want_sat[4 * k + 2] >> 2);
    static uint8_t next;
    for (int t = 0; t < 64; t++) {
        int s = (next + t) & 63;
        if (busy >> s & 1) continue;
        next = (uint8_t)(s + 1);
        memcpy(e->want_spat[s], p, 32);
        if (!same(e->want_spat[s], e->vram_spat[s], 32)) e->spat_dirty |= 1ull << s;
        else e->spat_dirty &= ~(1ull << s);
        return s;
    }
    return 0;
}

// the TMS9918 sprites: a moving sprite's main color (Mario's two most
// used) where its lines have room, none ever a 5th on a line
#define hw_pat pic_u.p.hw_pat           // their pixels (bit 15 - x of row y)
static uint8_t hw_of[32], hw_col[32];   // whose (vis index) and their color
static int nhw;

static bool give_hw(dkcv_t *e, int k, uint8_t *line_n, bool two) {
    vis_t *v = &vis[k];
    unsigned cnt[16] = {0};
    for (int q = 0; q < v->img->n; q++)
        for (int r = 0; r < 12; r++) cnt[v->img->col[q]] += (unsigned)__builtin_popcount(v->top[r] & v->img->at[q][r]);
    uint8_t a = 0, b = 0;
    for (int c = 1; c < 16; c++)
        if (cnt[c] > cnt[a]) a = (uint8_t)c;
    for (int c = 1; c < 16; c++)
        if (c != a && cnt[c] > cnt[b]) b = (uint8_t)c;
    if (!cnt[a]) return false;
    int need = two && cnt[b] >= 4 ? 2 : 1;
    if (nhw + need > 32) { e->stats.hw_refused++; return false; }
    int l0 = v->y < 0 ? 0 : v->y, l1 = v->y + 16 > DKCV_H ? DKCV_H : v->y + 16;
    for (int l = l0; l < l1; l++)
        if (line_n[l] + need > 4) { e->stats.hw_refused++; return false; }
    for (int l = l0; l < l1; l++) line_n[l] = (uint8_t)(line_n[l] + need);
    v->hw_n = (uint8_t)need;
    v->hw_c[0] = a;
    v->hw_c[1] = b;
    v->hw_m = (uint16_t)(1u << a | (need > 1 ? 1u << b : 0));
    for (int j = 0; j < need; j++) {
        hw_of[nhw] = (uint8_t)k;
        hw_col[nhw] = j ? b : a;
        nhw++;
    }
    e->stats.hw += (uint32_t)need;
    return true;
}

static void place_sprites(dkcv_t *e, uint32_t *now) {
    // the places they are in
    memset(now, 0, 24 * sizeof *now);
    for (int k = 0; k < nvis; k++) {
        const vis_t *v = &vis[k];
        uint32_t m = (1u << ((v->x1 - 1) / 8 + 1)) - 1;
        if ((v->x1 - 1) / 8 >= 31) m = ~0u;
        m &= ~((1u << (v->x0 / 8)) - 1);
        for (int b = v->y0 / 8; b <= (v->y1 - 1) / 8; b++) now[b] |= m;
    }
    // the TMS9918's sprites: Mario, those that had one, then those that moved
    nhw = 0;
    if (dkcv_sprites) {
        uint8_t line_n[DKCV_H] = {0};
        bool tried[DKCV_ENTRIES] = {false};
        for (int pass = 0; pass < 3; pass++)
            for (int k = nvis - 1; k >= 0; k--) {
                int en = vis[k].e;
                if (tried[k]) continue;
                bool want = pass == 0 ? en == DKCV_MARIO
                          : pass == 1 ? e->hw[en]
                          : en >= 12 && (vis[k].x != e->ex[en] || vis[k].y != e->ey[en]);
                if (!want) continue;
                tried[k] = true;
                give_hw(e, k, line_n, en == DKCV_MARIO);
            }
    }
    memset(hw_pat, 0, sizeof hw_pat);
    for (int j = 0; j < nhw; j++) {
        const vis_t *v = &vis[hw_of[j]];
        for (int q = 0; q < v->img->n; q++) {
            if (v->img->col[q] != hw_col[j]) continue;
            for (int r = 0; r < 12; r++)
                for (uint32_t m = v->top[r] & v->img->at[q][r]; m; m &= m - 1)
                    hw_pat[j][r] |= (uint16_t)(0x8000 >> __builtin_ctz(m));
        }
    }
    // the places that differ from the last picture's (or whose tiles
    // changed): those with sprites made again now, those the sprites have
    // left marked for make_cells
    for (int b = 0; b < 24; b++) {
        uint32_t m = now[b] | e->cells_prev[b];
        if (!m) continue;
        if (now[b]) paint_band(b, now[b]);
        static const uint8_t none[8];
        for (; m; m &= m - 1) {
            int cx = __builtin_ctz(m);
            uint32_t h = 2166136261u;
            bool over = now[b] >> cx & 1;
            for (int k = 0; k < 8; k++) {
                const uint8_t *o = over ? &sband[k][cx * 8] : none;
                uint32_t w0, w1;
                memcpy(&w0, o, 4);
                memcpy(&w1, o + 4, 4);
                h = (h ^ w0) * 16777619u;
                h = (h ^ w1) * 16777619u;
            }
            if (h != ov_hash[b][cx]) {
                ov_hash[b][cx] = h;
                cells[b] |= 1u << cx;
            }
            if (over && (cells[b] >> cx & 1)) {
                make_over(e, b, cx);
                cells[b] &= ~(1u << cx);
            }
        }
    }
    for (int en = 0; en < DKCV_ENTRIES; en++) {
        e->ex[en] = e->ey[en] = -32768;
        e->hw[en] = false;
    }
    for (int k = 0; k < nvis; k++) {
        e->ex[vis[k].e] = vis[k].x;
        e->ey[vis[k].e] = vis[k].y;
        e->hw[vis[k].e] = vis[k].hw_n != 0;
    }
}

// the attributes: the front one first (the board's later entries are in front)
static void make_sprites(dkcv_t *e) {
    uint8_t sat[128];
    memset(sat, 0, sizeof sat);
    uint64_t used = 0;
    int n = 0;
    for (int j = nhw - 1; j >= 0; j--) {
        const vis_t *v = &vis[hw_of[j]];
        uint8_t p[32];
        for (int y = 0; y < 16; y++) {
            p[y] = (uint8_t)(hw_pat[j][y] >> 8);        // left half: 0-15
            p[16 + y] = (uint8_t)hw_pat[j][y];          // right half: 16-31
        }
        int s = pattern_slot(e, p, used);
        used |= 1ull << s;
        int y = v->y - 1;
        sat[4 * n] = (uint8_t)(y < -32 ? 0xE0 : y & 0xFF);
        if (sat[4 * n] == 0xD0) sat[4 * n] = 0xD1;
        sat[4 * n + 1] = (uint8_t)v->x;
        sat[4 * n + 2] = (uint8_t)(s << 2);
        sat[4 * n + 3] = hw_col[j];
        n++;
    }
    sat[4 * n] = 0xD0;
    memcpy(e->want_sat, sat, (size_t)(4 * n + 1));
}

void dkcv_board(dkcv_board_t *b) {
    memcpy(b->vram, dk_videoram, sizeof b->vram);
    memcpy(b->spr, dk_spriteram + (dk_sprite_bank << 9), sizeof b->spr);
    b->flip = dk_flip;
    b->pal = dk_palette_bank;
}

DKCV_HOT bool dkcv_picture(dkcv_t *e, const dkcv_board_t *b) {
    bd = b;
    if (!colors_ready) colors_init();
    if (!pens_ready) pens_init();
    if (!layout_ready) layout_init();
    if (!small_clear) {                 // (the plugin's room before the game)
        memset(small, dkcv_pen[0] * 0x11, sizeof small);       // (pen 0, as the tiles' room was)
        memset(cache, 0, sizeof cache);
        memset(ov_hash, 0xff, sizeof ov_hash);
        small_clear = true;
        e->pic_valid = false;
    }
    if (!turned_ready) turn_tiles();
    if (b->flip != e->flip || b->pal != e->pal) {
        e->pic_valid = false;
        e->flip = b->flip;
        e->pal = b->pal;
        memset(cache, 0, sizeof cache);
    }
    // the tiles that changed (all of them after a message), row by row,
    // and those an earlier picture left
    uint32_t changed[32];
    memcpy(changed, e->pending, sizeof changed);
    if (!e->pic_valid || !same(b->vram, e->vram_seen, sizeof e->vram_seen)) {
        for (int r = 0; r < 32; r++)
            for (int c = 0; c < 28; c++) {
                if (tile_piece[r][c] < 0) continue;
                int i = tile_index(b->flip, r, c);
                if (e->pic_valid && b->vram[i] == e->vram_seen[i]) continue;
                changed[r] |= 1u << c;
            }
        memcpy(e->vram_seen, b->vram, sizeof e->vram_seen);
    }
    // drawn from row_next on, up to DKCV_TILES (or tiles_most); the rows
    // past that wait
    int done = 0, r0 = e->row_next, most = e->tiles_most ? e->tiles_most : DKCV_TILES;
    bool full = false;
    for (int k = 0; k < 32; k++) {
        int r = (r0 + k) & 31;
        e->pending[r] = 0;
        if (!changed[r]) continue;
        int n = __builtin_popcount(changed[r]);
        if (!full && done && done + n > most) {
            full = true;
            e->row_next = (uint8_t)r;
            e->stats.deferred++;
        }
        if (full) {
            e->pending[r] = changed[r];
            continue;
        }
        done += n;
        scale_row(r, changed[r]);
    }
    e->stats.tiles += (uint32_t)done;
    if (!e->pic_valid) {                // the black between the pieces too
        for (int i = 0; i < 24; i++) cells[i] = ~0u;
        e->pic_valid = true;
    }
    uint32_t now[24];
    find_sprites();
    tops();
    place_sprites(e, now);
    memcpy(e->cells_prev, now, sizeof now);
    make_cells(e);
    make_sprites(e);
    e->stats.pictures++;
    e->dirty = true;
    return !full;
}

void dkcv_canvas(dkcv_t *e, const uint8_t *px) {
    if (!colors_ready) colors_init();
    if (!layout_ready) layout_init();
    e->pic_valid = false;
    uint8_t c[8];
    for (int y = 0; y < DKCV_H; y++)
        for (int cx = 0; cx < 32; cx++) {
            for (int i = 0; i < 8; i++) {
                uint8_t v = px[y * DKCV_W + cx * 8 + i];
                c[i] = v ? v : BLACK;
            }
            span(e, (unsigned)((y >> 3) * 256 + cx * 8 + (y & 7)), c, c);
        }
    e->bands = ~0u;
    e->want_sat[0] = 0xD0;              // no sprites
    memset(e->cells_prev, 0xff, sizeof e->cells_prev);
    memset(e->hw, 0, sizeof e->hw);
    memset(ov_hash, 0xff, sizeof ov_hash);
    e->stats.pictures++;
    e->dirty = true;
}
static void screen_of(const uint8_t *pat, const uint8_t *colt, const uint8_t *sat, const uint8_t (*spat)[32], uint8_t *px) {
    for (int y = 0; y < DKCV_H; y++)
        for (int x = 0; x < DKCV_W; x++) {
            unsigned a = (unsigned)(((y >> 3) * 32 + (x >> 3)) * 8 + (y & 7));
            uint8_t c = colt[a];
            c = (pat[a] & (0x80 >> (x & 7))) ? c >> 4 : c & 15;
            px[y * DKCV_W + x] = c ? c : BLACK;
        }
    // the sprites, the TMS9918's way: 4 a line at most, the first in front
    uint8_t seen[DKCV_H] = {0};
    for (int k = 0; k < 32 && sat[4 * k] != 0xD0; k++) {
        int y0 = sat[4 * k] >= 0xE0 ? (int)sat[4 * k] - 256 + 1 : sat[4 * k] + 1;
        int x0 = sat[4 * k + 1], s = sat[4 * k + 2] >> 2;
        uint8_t col = sat[4 * k + 3] & 15;
        for (int v = 0; v < 16; v++) {
            int y = y0 + v;
            if (y < 0 || y >= DKCV_H || seen[y] >= 4) continue;
            seen[y]++;
            uint16_t w = (uint16_t)(spat[s][v] << 8 | spat[s][16 + v]);
            for (int u = 0; u < 16; u++) {
                int x = x0 + u;
                if (x < DKCV_W && col && (w & (0x8000 >> u)) && !(px[y * DKCV_W + x] & 0x80))
                    px[y * DKCV_W + x] = col | 0x80;    // (the front sprite's pixel stays)
            }
        }
    }
    for (int i = 0; i < DKCV_W * DKCV_H; i++) px[i] &= 15;
}

void dkcv_screen(const dkcv_t *e, uint8_t *px) {
    screen_of(e->vram_pat, e->vram_col, e->vram_sat, (const uint8_t (*)[32])e->vram_spat, px);
}
void dkcv_wanted(const dkcv_t *e, uint8_t *px) {
    screen_of(e->want_pat, e->want_col, e->want_sat, (const uint8_t (*)[32])e->want_spat, px);
}

void dkcv_scaled(uint8_t *px) {         // (colors now: the tests compare dkcv_pen[] of theirs)
    for (int y = 0; y < DKCV_H; y++)
        for (int x = 0; x < DKCV_W; x++) px[y * DKCV_W + x] = SMALL(y, x);
}

void dkcv_layout(int i, int *c0, int *r0, int *nc, int *nr, int *x, int *y) {
    if (!layout_ready) layout_init();
    const piece_t *p = &layout[i];
    *c0 = p->c0, *r0 = p->r0, *nc = p->nc, *nr = p->nr, *x = p->x, *y = p->y;
}
int dkcv_pieces(void) { return PIECES; }

void dkcv_ideal(uint8_t *px) {
    for (int b = 0; b < 24; b++) {
        paint_band(b, ~0u);
        for (int cx = 0; cx < 32; cx++) {
            for (int k = 0; k < 8; k++)
                for (int i = 0; i < 8; i++) {
                    int y = b * 8 + k, x = cx * 8 + i;
                    uint8_t o = sband[k][x];
                    px[y * DKCV_W + x] = o ? o & 15 : SMALL(y, x);
                }
        }
    }
}

// ---- what the console needs ----------------------------------------------------------
static inline bool need_pat(const dkcv_t *e, unsigned j) { return e->want_pat[j] != e->vram_pat[j]; }
static inline bool need_col(const dkcv_t *e, unsigned j) { return e->want_col[j] != e->vram_col[j]; }

// The next bytes to send, from the cursor on (qbcv.c's next_run)
DKCV_HOT static bool next_run(dkcv_t *e, bool *color, unsigned *s, int *m, unsigned most) {
    if (!e->dirty) return false;
    unsigned i = e->cursor;
    int band = -1;
    for (unsigned k = 0; k < DKCV_BYTES;) {
        if (!(i & 255)) {
            if (band >= 0) e->bands &= ~(1u << band);
            band = -1;
            if (k + 256 <= DKCV_BYTES && !(e->bands >> (i >> 8) & 1)) {
                k += 256;
                i = i + 256 == DKCV_BYTES ? 0 : i + 256;
                continue;
            }
            band = (int)(i >> 8);
        }
        if (!(i & 3) && k + 4 <= DKCV_BYTES) {
            uint32_t w, v, wc, vc;
            memcpy(&w, e->want_pat + i, 4);
            memcpy(&v, e->vram_pat + i, 4);
            memcpy(&wc, e->want_col + i, 4);
            memcpy(&vc, e->vram_col + i, 4);
            if (w == v && wc == vc) {
                k += 4;
                i = i + 4 == DKCV_BYTES ? 0 : i + 4;
                continue;
            }
        }
        bool nc = need_col(e, i);
        if (!nc && !need_pat(e, i)) {
            k++;
            i = i + 1 == DKCV_BYTES ? 0 : i + 1;
            continue;
        }
        e->cursor = (uint16_t)i;
        *color = nc;
        *s = i;
        unsigned end = i + 1, gap = 0;
        for (unsigned j = i + 1; j < DKCV_BYTES && j - i < most; j++) {
            if (nc ? need_col(e, j) : need_pat(e, j)) { end = j + 1; gap = 0; }
            else if (++gap > 3) break;
        }
        *m = (int)(end - i);
        return true;
    }
    e->dirty = false;
    return false;
}

static void put_run(dkcv_t *e, list_t *l, bool color, unsigned s, int n) {
    l_run_head(l, (uint16_t)((color ? COLORS : PATTERNS) + s), n);
    uint8_t *d = l->b + l->n;
    if (color) {
        memcpy(d, e->want_col + s, n);
        memcpy(e->vram_col + s, d, n);
        e->stats.col_bytes += (unsigned)n;
    } else {
        memcpy(d, e->want_pat + s, n);
        memcpy(e->vram_pat + s, d, n);
        e->stats.pat_bytes += (unsigned)n;
    }
    l->n += n;
    e->stats.runs++;
}

// The sprites' bytes go before the picture's, a run at a time: the
// patterns first (into slots no attribute points at, so none shows half
// sent), then the attributes, from the end back, a sprite's 4 bytes in one
// run. From the end back, a longer table is in place before the old end
// mark goes, and a shorter one's end mark goes first.
static int sat_len(const uint8_t *sat) {
    int n = 0;
    while (n < 32 && sat[4 * n] != 0xD0) n++;
    return n < 32 ? 4 * n + 1 : 128;
}

static bool sprites_pending(const dkcv_t *e) {
    if (e->spat_dirty) return true;
    int n = sat_len(e->want_sat);       // 4k + 1, or 128
    return !same(e->vram_sat, e->want_sat, (size_t)(n & ~3)) || (n & 3 && e->vram_sat[n - 1] != e->want_sat[n - 1]);
}

// one run of at most `most` bytes; false if none was sent
static bool sprite_piece(dkcv_t *e, list_t *l, int most) {
    while (e->spat_dirty) {
        int s = __builtin_ctzll(e->spat_dirty);
        const uint8_t *w = e->want_spat[s];
        uint8_t *v = e->vram_spat[s];
        int j = 0, end = 32;
        while (j < 32 && w[j] == v[j]) j++;
        while (end > j && w[end - 1] == v[end - 1]) end--;
        if (j == 32) { e->spat_dirty &= ~(1ull << s); continue; }
        int n = end - j < most ? end - j : most;
        if (n < 1) return false;
        l_run_head(l, (uint16_t)(SPAT + 32 * s + j), n);
        memcpy(l->b + l->n, w + j, n);
        memcpy(v + j, w + j, n);
        l->n += n;
        e->stats.spr_bytes += (unsigned)n;
        if (same(w, v, 32)) e->spat_dirty &= ~(1ull << s);
        return true;
    }
    int len = sat_len(e->want_sat), d = len - 1;
    while (d >= 0 && e->want_sat[d] == e->vram_sat[d]) d--;
    if (d < 0) return false;
    int s = d / 4 * 4;
    if (d - s + 1 > most) return false;
    while (s >= 4 && d - (s - 4) + 1 <= most && memcmp(e->want_sat + s - 4, e->vram_sat + s - 4, 4)) s -= 4;
    int n = d - s + 1;
    l_run_head(l, (uint16_t)(SAT + s), n);
    memcpy(l->b + l->n, e->want_sat + s, n);
    memcpy(e->vram_sat + s, e->want_sat + s, n);
    l->n += n;
    e->stats.spr_bytes += (unsigned)n;
    return true;
}

// what's on screen written again as it is (qbcv.c's)
static void refresh_run(dkcv_t *e, list_t *l, int n) {
    unsigned at = e->refresh_at;
    bool color = at >= DKCV_BYTES;
    unsigned s = color ? at - DKCV_BYTES : at;
    if (s + n > DKCV_BYTES) n = (int)(DKCV_BYTES - s);
    l_run_head(l, (uint16_t)((color ? COLORS : PATTERNS) + s), n);
    memcpy(l->b + l->n, (color ? e->vram_col : e->vram_pat) + s, n);
    l->n += n;
    e->refresh_at = (uint16_t)((at + n) % (2 * DKCV_BYTES));
    e->stats.refreshed += (unsigned)n;
}

uint32_t dkcv_backlog(const dkcv_t *e) {
    uint32_t n = 0;
    if (e->dirty)
        for (unsigned j = 0; j < DKCV_BYTES; j++) n += need_pat(e, j) + need_col(e, j);
    for (uint64_t m = e->spat_dirty; m; m &= m - 1) n += 32;
    int len = sat_len(e->want_sat);
    for (int j = 0; j < len; j++) n += e->want_sat[j] != e->vram_sat[j];
    return n;
}

// the name table: name i at place i in each third (it changes never)
static void names_run(dkcv_t *e, list_t *l, int n) {
    int left = 768 - e->names_at;
    if (n > left) n = left;
    l_run_head(l, (uint16_t)(NAMES + e->names_at), n);
    for (int k = 0; k < n; k++) l->b[l->n++] = (uint8_t)(e->names_at + k);
    e->names_at = (uint16_t)(e->names_at + n);
    if (e->names_at == 768) e->names_done = true;
}

// ---- a list, the console waiting for the vertical blank (messages) -------------------
static void plain_list(dkcv_t *e, list_t *l) {
    if (e->resync) {
        l_end_vblank(l);
        if (e->tone_n) l_psg(l, e->tone_bytes, e->tone_n);
        if (e->resync-- == 2) return;
    } else {
        // the sprites 16 x 16 (R1), every time: a list may run twice
        l_reg(l, 1, 0xE2);
        e->regs_done = true;
        if (e->tone_n) l_psg(l, e->tone_bytes, e->tone_n);
    }
    while (!e->names_done) {
        int n = room(l);
        if (n < 16 && l->vblank) { l_end_vblank(l); continue; }
        if (n < 1) break;
        names_run(e, l, n);
    }
    while (e->names_done) {
        int n = room(l);
        if (n < 8 && l->vblank) { l_end_vblank(l); continue; }
        if (n < 4 || !sprite_piece(e, l, n)) break;
    }
    bool color;
    unsigned s;
    int m;
    while (e->names_done && next_run(e, &color, &s, &m, 255)) {
        int n = room(l);
        if (n < m && l->vblank) {
            if (n >= 8) put_run(e, l, color, s, n);
            l_end_vblank(l);
            continue;
        }
        if (n < 1) break;
        put_run(e, l, color, s, n < m ? n : m);
    }
    for (int k = 0; k < 2 && e->names_done; k++) {
        int n = room(l);
        if (n < 32) break;
        refresh_run(e, l, n);
    }
    l_end_vblank(l);
}

// ---- a list with samples in it (qbcv.c's pcm_list) -------------------------------------
DKCV_HOT static void fill(dkcv_t *e, list_t *l, uint32_t until) {
    int avail = (int)until - (int)l->t;
    if (avail < C_RUN_ACT + C_BYTE_ACT) return;
    int n = (avail - C_RUN_ACT) / C_BYTE_ACT;
    if (n > 255) n = 255;
    if (!e->names_done) { names_run(e, l, n); return; }
    bool color;
    unsigned s;
    int m;
    if (!next_run(e, &color, &s, &m, 1)) {
        if (e->refresh_at % DKCV_BYTES + (unsigned)n > DKCV_BYTES) e->refresh_at = (uint16_t)(e->refresh_at / DKCV_BYTES * DKCV_BYTES);
        refresh_run(e, l, n);
        return;
    }
    if (s + (unsigned)n > DKCV_BYTES) s = DKCV_BYTES - (unsigned)n;
    put_run(e, l, color, s, n);
}

DKCV_HOT static void pcm_list(dkcv_t *e, list_t *l) {
    l->t = C_FRAME;
    l_end_vblank(l);
    unsigned tone_at = 0;
    for (int k = 0; k < DKCV_PCM_N; k++) {
        uint8_t b[10];
        int n = 0;
        b[n++] = e->pcm_bytes[k][0];
        b[n++] = e->pcm_bytes[k][1];
        b[n++] = e->pcm_bytes[k][2];
        while (tone_at < e->tone_n && n < 5) {
            unsigned g = 1;
            while (tone_at + g < e->tone_n && !(e->tone_bytes[tone_at + g] & 0x80)) g++;
            if (n + (int)g > 6) break;
            memcpy(b + n, e->tone_bytes + tone_at, g);
            n += (int)g;
            tone_at += g;
        }
        if (k + 1 == DKCV_PCM_N) {
            l_psg(l, b, n);
            break;
        }
        uint32_t until = PCM_FIRST + (((uint32_t)(k + 1) * PCM_STEP) >> 8);
        int avail = (int)until - (int)l->t - (C_PSG + C_PSG_BYTE * n);
        // the sprites first, a run of theirs and then the picture's; or
        // a short run of the picture's as one of two (qbcv.c's)
        bool color;
        unsigned s;
        int m = 0;
        bool sp = e->names_done && sprites_pending(e) && avail >= 2 * C_RUN_ACT + C_BYTE_ACT * 5;
        bool two = !sp && e->names_done && next_run(e, &color, &s, &m, 7) && m <= 2 &&
                   avail >= 2 * C_RUN_ACT + C_BYTE_ACT * (m + 2);
        int need = sp ? 5 : two ? m + 1 : 1;
        int base = avail - (sp || two ? 2 : 1) * C_RUN_ACT, best = 0, best_left = 99;
        for (int r = 0; r < 3 && base - C_PSG_BYTE * r >= C_BYTE_ACT * need; r++) {
            int left = (base - C_PSG_BYTE * r) % C_BYTE_ACT;
            if (left < best_left) { best_left = left; best = r; }
        }
        while (best--) b[n++] = e->pcm_bytes[k][2];     // (the last attenuator again: no change)
        l_psg(l, b, n);
        if (sp) sprite_piece(e, l, ((int)until - (int)l->t - 2 * C_RUN_ACT) / C_BYTE_ACT - 1);
        if (two) put_run(e, l, color, s, m);
        fill(e, l, until);
    }
    e->stats.pcm_frames++;
    e->resync = 2;
}

size_t dkcv_tv_frame(dkcv_t *e, uint8_t *out) {
    list_t l = {out, 0, C_START, true};
    if (e->pcm && e->regs_done) pcm_list(e, &l);
    else plain_list(e, &l);
    l.t += C_END;
    l.b[l.n++] = 0;
    e->tone_n = 0;
    e->stats.tv_frames++;
    if (l.t > e->stats.t_max) e->stats.t_max = l.t;
    if (!e->dirty) e->stats.settled++;
    return l.n;
}

// ---- the sound -----------------------------------------------------------------------
// The DAC: the three tones at a period of 1, their attenuators as measured
// on the user's console (coleco/ladder.json), summed. dkdac3.h's 512 levels
// go from 0 to 2.40, each a triple of attenuators chosen as Movies' table is
// (tools/mkdac3.py: within 1.5 levels of its level, and neighbouring levels'
// triples alike, so a small change makes small partial sums between a
// sample's three writes). The sound rests at the middle (DAC_MID, 1.20).
//
// 0.1.2: the sound 3.2 times as loud as 0.1.1's (+10 dB). 0.1.1 played it
// through two tones (128 levels, 0 to 1.40), some 13 dB under an ordinary
// ColecoVision cartridge (a game's square tones swing twice as far as a
// parked tone's level can). The third tone gives it the room (1.7 times
// 0.1.1's either side of the rest; the steps about as fine, as neighbouring
// levels often share a triple: 214 different ones in the 512). The board reaches about +-24,000 of 32,767 once the
// high-pass has taken its resting level off, 18,000 in 99 samples of 100
// (the scripted game, test/soundcmp.sh). SCALE levels for 8,192 puts
// 18,000 at 330 levels, past the DAC's 255 either side: the soft knee (from
// 191, 3/4 of the way) rounds the loudest off toward 255, never clipping.
// 7.8% of the scripted game's samples go into it.
#define DAC_LEVELS DKDAC3_LEVELS
#define DAC_MID    255
#define DAC_HALF   255.0f           // the levels either side of DAC_MID
#define DAC_KNEE   191.0f
#define SCALE      150              // 0.1.1 had 20 two-tone levels (1.40 / 127 each): 3.2 times that, in these
#define SCALE_FULL 8192
#define HP_SHIFT   6                // a pole at 15 Hz
static const float ladder[3][16] = {
    {1.0f, 0.7914f, 0.6320f, 0.5084f, 0.4019f, 0.3161f, 0.2459f, 0.1912f, 0.1396f, 0.1080f, 0.0844f, 0.0672f, 0.0524f, 0.0421f, 0.0310f, 0.0f},
    {1.0f, 0.7816f, 0.6125f, 0.4850f, 0.3925f, 0.3109f, 0.2416f, 0.1872f, 0.1373f, 0.1058f, 0.0828f, 0.0653f, 0.0533f, 0.0412f, 0.0306f, 0.0f},
    {1.0f, 0.7979f, 0.6229f, 0.4921f, 0.3942f, 0.3004f, 0.2348f, 0.1874f, 0.1419f, 0.1106f, 0.0846f, 0.0677f, 0.0534f, 0.0420f, 0.0328f, 0.0f}};
static uint8_t limit[512];                  // |level| (0-511) to 0-255: straight to DAC_KNEE, then soft
static int16_t lq[3][16];                   // ladder in 1/16384ths, for put_sample's integer sums
static bool dac_ready;

float dkcv_dac_level(int a0, int a1, int a2) { return ladder[0][a0 & 15] + ladder[1][a1 & 15] + ladder[2][a2 & 15]; }

static void dac_init(void) {
    for (int i = 0; i < 512; i++) {
        float v = (float)i;
        if (v > DAC_KNEE) v = DAC_KNEE + (DAC_HALF - DAC_KNEE) * (1.0f - expf(-(v - DAC_KNEE) / (DAC_HALF - DAC_KNEE)));
        limit[i] = (uint8_t)(v + 0.5f);
    }
    for (int c = 0; c < 3; c++)
        for (int a = 0; a < 16; a++) lq[c][a] = (int16_t)lrintf(ladder[c][a] * 16384.0f);
    dac_ready = true;
}

// a sample's three writes, in the order whose two partial sums (the chip
// between the writes, 52 T each) are nearest the new level. Written a,
// then b, then c: off the new level by |d_b + d_c|, then by |d_c|. In
// integers, the orders tried in turn and the first of equals kept (Cloak's
// clcv.c; about half the cycles of floats).
static const uint8_t order[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
static inline int32_t iabs(int32_t x) { return x < 0 ? -x : x; }
static inline void put_sample(uint8_t *out, int *a0, int *a1, int *a2, uint16_t t) {
    const int w0 = t >> 8, w1 = t >> 4 & 15, w2 = t & 15;
    const int32_t d0 = lq[0][w0] - lq[0][*a0], d1 = lq[1][w1] - lq[1][*a1], d2 = lq[2][w2] - lq[2][*a2];
    const int32_t sum = d0 + d1 + d2;
    const int32_t f0 = iabs(sum - d0), f1 = iabs(sum - d1), f2 = iabs(sum - d2);
    const int32_t l0 = iabs(d0), l1 = iabs(d1), l2 = iabs(d2);
    int32_t best = f0 + l2, v;
    int pick = 0;
    if ((v = f0 + l1) < best) { best = v; pick = 1; }
    if ((v = f1 + l2) < best) { best = v; pick = 2; }
    if ((v = f1 + l0) < best) { best = v; pick = 3; }
    if ((v = f2 + l1) < best) { best = v; pick = 4; }
    if ((v = f2 + l0) < best) pick = 5;
    const uint32_t b = (uint32_t)(0x90 | w0) | (uint32_t)(0xB0 | w1) << 8 | (uint32_t)(0xD0 | w2) << 16;
    out[0] = (uint8_t)(b >> 8 * order[pick][0]);
    out[1] = (uint8_t)(b >> 8 * order[pick][1]);
    out[2] = (uint8_t)(b >> 8 * order[pick][2]);
    *a0 = w0; *a1 = w1; *a2 = w2;
}

void dkcv_sound(dkcv_t *e, const int16_t *s, int n) {
    if (!dac_ready) dac_init();
    e->tone_n = 0;
    if (n <= 0) {
        if (e->pcm) {
            static const uint8_t off[] = {0x9F, 0xBF, 0xDF};
            memcpy(e->tone_bytes, off, sizeof off);
            e->tone_n = sizeof off;
            e->pcm = false;
        }
        return;
    }
    if (!e->pcm) {
        e->pcm = true;
        e->pcm_gain = 0;
        e->hp_x = s[0];
        e->hp_y = 0;
        e->dac_att[0] = e->dac_att[1] = e->dac_att[2] = 15;     // (as the lists before left them)
    }
    // Every list, not just the first: the three tones parked at a period of
    // 1, the noise silent. One byte the console misreads (a bus hiccup) can
    // leave a tone at an audible period, and with the park sent once that
    // was a drone until RESET (seen on the console after some play). Now it
    // lasts a list at most. The noise's control too, so every register is
    // written each list: eight PSG bytes, and the tones' attenuators are
    // every sample's.
    static const uint8_t park[] = {0x81, 0x00, 0xA1, 0x00, 0xC1, 0x00, 0xE4, 0xFF};
    memcpy(e->tone_bytes, park, sizeof park);
    e->tone_n = sizeof park;
    // (the state in locals: the byte stores below could alias e's fields)
    int32_t hx = e->hp_x, hy = e->hp_y, gain = e->pcm_gain;
    int a0 = e->dac_att[0], a1 = e->dac_att[1], a2 = e->dac_att[2];
    uint32_t clipped = 0;
    for (int k = 0; k < DKCV_PCM_N; k++) {
        int32_t x = s[k < n ? k : n - 1];
        hy += ((x - hx) << 8) - (hy >> HP_SHIFT);
        hx = x;
        int32_t v = (hy >> 8) * SCALE / SCALE_FULL;
        int32_t a = v < 0 ? -v : v;
        if (a > (int32_t)DAC_KNEE) clipped++;
        a = limit[a > 511 ? 511 : a];
        int32_t lvl = DAC_MID + (v < 0 ? -a : a);
        // (the DAC's rest is 1.7 times 0.1.1's: up to it over 43 ms, as steep as before)
        if (gain < 32 && !(k & 7)) gain++;
        lvl = lvl * gain / 32;
        put_sample(e->pcm_bytes[k], &a0, &a1, &a2, dk_dac3[lvl < 0 ? 0 : lvl > DAC_LEVELS - 1 ? DAC_LEVELS - 1 : lvl]);
    }
    e->hp_x = hx;
    e->hp_y = hy;
    e->pcm_gain = (uint8_t)gain;
    e->dac_att[0] = (uint8_t)a0;
    e->dac_att[1] = (uint8_t)a1;
    e->dac_att[2] = (uint8_t)a2;
    e->stats.clipped += clipped;
}

void dkcv_prepare(void) {
    if (!pens_ready) pens_init();
    if (!turned_ready) turn_tiles();
}

void dkcv_init(dkcv_t *e) {
    memset(e, 0, sizeof *e);
    e->dirty = true;
    e->bands = (1u << (DKCV_BYTES / 256)) - 1;
    e->vram_sat[0] = 0xD0;              // the kernel's (no sprites)
    e->want_sat[0] = 0xD0;
    for (int i = 0; i < DKCV_ENTRIES; i++) e->ex[i] = e->ey[i] = -32768;
    if (!colors_ready) colors_init();
    if (!dac_ready) dac_init();
    layout_ready = false;
    layout_init();
    pens_ready = false;                 // (made at the first picture, from the board's PROMs)
    turned_ready = false;
    memset(cache, 0, sizeof cache);
    memset(ov_hash, 0xff, sizeof ov_hash);
}
