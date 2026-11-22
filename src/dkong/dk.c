// license:BSD-3-Clause
// copyright-holders:Couriersud, Curt Coder, Carl, Aaron Giles, O. Galibert
//
// dk.c -- the Donkey Kong board (dkong, TKG-04) as MAME 0.288 emulates
// it, in C for the cart:
//
//   dkong.cpp, dkong_v.cpp, dkong_a.cpp (Couriersud):
//     the memory map, inputs, NMI, DMA glue, palette, tiles, sprites and
//     the sound CPU's glue
//   i8257.cpp (Curt Coder, Carl): the DMA controller
//   latch8.cpp (Couriersud): the sound latches; watchdog.cpp (Aaron Giles)
//   resnet.cpp (Couriersud): the palette's resistor networks
//   schedule.cpp, diexec.cpp, screen.cpp (Aaron Giles), sound.cpp (O.
//     Galibert, Aaron Giles): MAME's timeslices (three devices, the 60 kHz quantum and
//     the DMA's perfect quantum), timers, suspend and triggers, VBLANK,
//     input line events and the streams' 50 Hz update
//
// Times are MAME's attotimes as attoseconds from a whole second (epoch),
// moved on a second at a time between timeslices: the same sums MAME does
// on (seconds, attoseconds), in one 64-bit number. MAME's license
// (BSD-3-Clause) is in README.md.
#include "dk.h"

#include <string.h>

#include "dksnd.h"
#include "mcs48.h"
#include "z80.h"

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define DK_HOT __attribute__((section(".time_critical.dk")))
#else
#define DK_HOT
#endif

// ---------------------------------------------------------------------------
// time

typedef int64_t at_t;                   // attoseconds since the epoch (epoch_s seconds)
#define AS_PER_SEC 1000000000000000000LL
#define AT_NEVER INT64_MAX
static uint32_t epoch_s;

// floor(a * f / 1e18) for 0 <= a < 1e18, without 128-bit sums
static inline uint64_t mul_frac(int64_t a, uint32_t f) {
	uint64_t ah = (uint64_t)a / 1000000000u, al = (uint64_t)a % 1000000000u;
	uint64_t x = ah * f, y = al * f;
	uint64_t q1 = x / 1000000000u, r1 = x % 1000000000u;
	return q1 + (r1 * 1000000000u + y) / (uint64_t)AS_PER_SEC;
}

// The same for the two clocks the times are counted in, without the
// library's 64-bit divide (the M33 has none): f / 1e18 is 3 / den, and
// 3a / den comes from a multiply by 2^64 / den, one short at most, put right.
#define DEN_SND 39062500000ull          // 1e18 / DKSND_TICK_HZ * 3
#define DEN_Z80 976562500000ull         // 1e18 / DK_CLOCK * 3
static inline uint64_t mulhi64(uint64_t a, uint64_t b) {
	uint32_t a0 = (uint32_t)a, a1 = (uint32_t)(a >> 32), b0 = (uint32_t)b, b1 = (uint32_t)(b >> 32);
	uint64_t p00 = (uint64_t)a0 * b0, p01 = (uint64_t)a0 * b1, p10 = (uint64_t)a1 * b0, p11 = (uint64_t)a1 * b1;
	uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
	return p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
static inline uint64_t frac3(int64_t a, uint64_t den, uint64_t inv) {
	uint64_t n = 3 * (uint64_t)a;                  // a < 1e18: n < 2^62
	uint64_t q = mulhi64(n, inv);
	if (n - q * den >= den) q++;
	return q;
}
static uint64_t at_ticks(at_t t, uint32_t f) {
	uint64_t s = epoch_s;
	while (t >= AS_PER_SEC) { t -= AS_PER_SEC; s++; }
	if (t >= 0 && f == DKSND_TICK_HZ) return s * f + frac3(t, DEN_SND, UINT64_MAX / DEN_SND);
	if (t >= 0 && f == DK_CLOCK) return s * f + frac3(t, DEN_Z80, UINT64_MAX / DEN_Z80);
	return s * f + mul_frac(t, f);
}

#define PIX_AS (AS_PER_SEC / 6144000)   // HZ_TO_ATTOSECONDS(PIXEL_CLOCK)
#define HTOTAL 384
#define VTOTAL 264
#define VBSTART 240
#define VBEND 16
#define SCAN_AS (PIX_AS * HTOTAL)
#define FRAME_AS (SCAN_AS * VTOTAL)
#define VBLANK_AS (SCAN_AS * (VTOTAL - (VBSTART - VBEND)))
#define QUANTUM_AS (AS_PER_SEC / 60000) // set_maximum_quantum(attotime::from_hz(60000))
#define PERFECT_AS (AS_PER_SEC / DK_CLOCK)  // the 2nd smallest device cycle (the Z80's and the 8257's)
#define USEC_AS 1000000000000LL

static const dk_roms_t *roms;
dk_inputs_t dk_in = { 0x00, 0x00, 0x00, DK_DSW0 };
uint8_t dk_ram[0xc00], dk_spriteram[0x400], dk_videoram[0x400];
uint8_t dk_flip, dk_sprite_bank, dk_palette_bank, dk_nmi_mask;
uint8_t dk_latch_3d, dk_latch_6h, dk_latch_vp2;
uint32_t dk_frames, dk_resets;
void (*dk_on_vblank)(void);
void (*dk_on_line)(int line);
void (*dk_on_lazy)(int what);
#ifdef DK_NO_TRACE
#define ON_LAZY(w) do { } while (0)
#else
#define ON_LAZY(w) do { if (dk_on_lazy) dk_on_lazy(w); } while (0)
#endif
static uint8_t dma_latch, grid_on, grid_col;
static bool watchdog_fired;
static uint8_t zero_page[0x100];        // the unmapped and unloaded ROM: 0
static uint8_t sink[0x100];             // writes that go nowhere

// ---------------------------------------------------------------------------
// the scheduler (src/emu/schedule.cpp, src/emu/diexec.cpp)

enum { D_Z80, D_DMA, D_SND, D_N };      // MAME's device order: maincpu, dma8257, soundcpu
#define SUSPEND_REASON_TRIGGER 0x0004

typedef struct {
	at_t local;                         // m_localtime
	int64_t aspc;                       // m_attoseconds_per_cycle
	int64_t apc_mid;                    // a cycle in local_time() mid-slice: cycles_to_attotime(1), the
	                                    // clock's attoseconds (truncated) times its clocks a cycle
	uint32_t cps;                       // m_cycles_per_second
	int divshift;
	uint32_t divisor;
	uint64_t total;                     // m_totalcycles
	uint32_t suspend, nextsuspend;
	bool eatcycles, nexteatcycles;
	int trigger;
} exec_t;

static exec_t ex[D_N];
static int order[D_N];                  // the execute list: the running ones, then the suspended
static int executing = -1;
static bool suspend_changes_pending;
static at_t basetime;
static bool perfect_on;                 // the DMA's perfect quantum (a quantum_slot of 0, its actual PERFECT_AS)
static at_t perfect_expire;

static int dma_run(int cycles);
static void dma_abort(void);
static int dma_done(void);

static void exec_init(exec_t *e, uint32_t cps, uint32_t clock, int divider) {
	memset(e, 0, sizeof *e);
	e->cps = cps;
	e->aspc = AS_PER_SEC / cps;
	e->apc_mid = (AS_PER_SEC / clock) * divider;
	int64_t attos = e->aspc;
	e->divshift = 0;
	while (attos >= (1LL << 31)) { e->divshift++; attos >>= 1; }
	e->divisor = (uint32_t)attos;
}

// MAME's divu_64x32(delta >> divshift, divisor): the cycles a device has
// to the slice's end, for the two clocks (the Z80's and the 8257's, and the
// sound CPU's), without the library's 64-bit divide (the M33 has none): x
// times 2^62 / d, one short at most, put right. Past 2^40, the divide.
#define Z_AS (AS_PER_SEC / DK_CLOCK)            // 325,520,833,333
#define Z_SHIFT 8
#define Z_DIV ((uint32_t)(Z_AS >> Z_SHIFT))
#define S_AS (AS_PER_SEC / DK_SND_CLOCK)        // 2,500,000,000,000
#define S_SHIFT 11
#define S_DIV ((uint32_t)(S_AS >> S_SHIFT))
_Static_assert(Z_DIV < (1u << 31) && (Z_AS >> (Z_SHIFT - 1)) >= (1ll << 31), "the Z80's divshift");
_Static_assert(S_DIV < (1u << 31) && (S_AS >> (S_SHIFT - 1)) >= (1ll << 31), "the sound CPU's divshift");
static inline __attribute__((always_inline)) int div_const(uint64_t x, uint32_t d) {
	if (__builtin_expect(x >> 40 != 0, 0)) return (int)(x / d);
	const uint32_t m = (uint32_t)((1ull << 62) / d);    // d > 2^30: m < 2^32
	uint32_t xl = (uint32_t)x, xh = (uint32_t)(x >> 32);
	uint64_t t = (uint64_t)xh * m + (((uint64_t)xl * m) >> 32);
	uint32_t q = (uint32_t)(t >> 30);
	if (x - (uint64_t)q * d >= d) q++;
	return (int)q;
}

static inline int dev_done(int d) {
	return d == D_Z80 ? z80_done() : d == D_DMA ? dma_done() : mcs48_done();
}

// A batch (timeslice): perfect slices run as one by the Z80, or by the 8257
// while the Z80 has given it the bus. Anything that would have changed the
// slices after it (a timer at the head, a device suspended or resumed) cuts
// it at the slice it came in. The Z80's reads and writes come at the start
// of its chunks (before their cycles go), and the 8257's steps are a cycle
// each, so the device's count then is the chunk's start: batch_q is the
// cycles the batch's first slice would have given it, and the chunk at c
// went in the slice c + 1 - batch_q after the first (or the first). An
// abort stops the device as it would have; a suspend or resume (no abort in
// MAME) lets it run to the end of that slice's cycles.
// Lazy slices (timeslice): a quiet stretch to the next timer as one slice.
static bool lazy_run;                   // the Z80's run in one
static bool lazy_rollback;              // it touched the board: taken back, the stretch run in slices
static at_t lazy_off_until = -AS_PER_SEC;   // (that stretch's end)
static at_t lazy_cap = AT_NEVER;        // a stretch of lines must end before this (a touch there)
#ifndef DK_LAZY_LINES
#define DK_LAZY_LINES 16                // scanline timers a stretch may go through, at most
#endif
static bool lazy_floor[D_N];            // the floor forced (eps_ok)
static at_t lazy_touch_at;              // where the Z80 was when it touched the board
static bool lazy_lines;                 // the sound CPU's part of a stretch through scanline timers
uint32_t dk_lazy_rollbacks;             // (tests) how many

static bool batching;
static int batch_dev;                   // D_Z80 or D_DMA
static int cut_done;                    // the device's cycles at the cut (-1: none)
static int64_t batch_q;
static void dma_keep(int n);
static void batch_cut(bool abort) {
	if (!batching) return;
	if (cut_done < 0) {
		cut_done = batch_dev == D_Z80 ? z80_done() : dma_done();
		int64_t keep = batch_q - cut_done;
		if (!abort) {
			if (batch_dev == D_Z80) z80_keep(keep > 1 ? (int)keep : 1);
			else dma_keep(keep > 1 ? (int)keep : 1);
		}
	}
	if (abort) { if (batch_dev == D_Z80) z80_abort(); else dma_abort(); }
}

static void abort_timeslice(void) {     // device_scheduler::abort_timeslice: the executing device's
	batch_cut(true);
	if (executing == D_Z80) z80_abort();
	else if (executing == D_DMA) dma_abort();
	else if (executing == D_SND) mcs48_abort();
}

static void rebuild_execute_list(void) {
	int n = 0;
	for (int d = 0; d < D_N; d++) if (!ex[d].suspend) order[n++] = d;
	for (int d = 0; d < D_N; d++) if (ex[d].suspend) order[n++] = d;
}

static void apply_suspend_changes(void) {
	uint32_t changed = 0;
	for (int i = 0; i < D_N; i++) {
		exec_t *e = &ex[order[i]];
		changed |= e->suspend ^ e->nextsuspend;
		e->suspend = e->nextsuspend;
		e->eatcycles = e->nexteatcycles;
	}
	if (changed) rebuild_execute_list();
	else suspend_changes_pending = false;
}

// a device's own suspend/resume/trigger (diexec.cpp): only the DMA uses them
static void suspend_resume_changed(int d) {
	suspend_changes_pending = true;
	if (executing != d) batch_cut(false);
	if (executing == d) abort_timeslice();
}
static void dev_suspend(int d, uint32_t reason, bool eat) {
	ex[d].nextsuspend |= reason;
	ex[d].nexteatcycles = eat;
	suspend_resume_changed(d);
}
static void dev_resume(int d, uint32_t reason) {
	ex[d].nextsuspend &= ~reason;
	suspend_resume_changed(d);
}
static void dev_trigger(int d, int trigid) {
	if (executing == d) abort_timeslice();
	if ((ex[d].nextsuspend & SUSPEND_REASON_TRIGGER) && ex[d].trigger == trigid) {
		dev_resume(d, SUSPEND_REASON_TRIGGER);
		ex[d].trigger = 0;
	}
}


// machine().time(): the timer's time in a callback, the running device's while it runs, else the base
static bool in_callback;
static at_t cb_expire;
static at_t now(void) {
	if (in_callback) return cb_expire;
	if (executing >= 0) return ex[executing].local + (at_t)dev_done(executing) * ex[executing].apc_mid;
	return basetime;
}

// ---- timers: a list in expiry order, ties first come first ---------------------------------
enum {
	TM_SND50,                           // the sound manager's streams update (made first)
	TM_VBSTART, TM_VBEND, TM_SCAN0,     // the screen's
	TM_WATCHDOG,
	TM_SCANLINE,                        // dkong_state's scanline timer (video_start, after the devices)
	TM_SYNC,                            // temporary timers (synchronize()) from here
	TM_COUNT = TM_SYNC + 24
};
enum { SY_LATCH_3D, SY_LATCH_6H, SY_LATCH_VP2, SY_INPUT };
enum { IN_Z80_NMI, IN_Z80_BUSREQ, IN_SND_IRQ, IN_N };


typedef struct {
	at_t expire, period;
	int8_t next;                        // the next in the list, -1 at its end
	bool enabled, listed, used;
	uint8_t type;
	int param;
} tmr_t;

static tmr_t tm[TM_COUNT];
static int tm_head = -1;
static int cb_timer = -1;
static bool cb_modified;

static void tm_remove(int i) {
	if (!tm[i].listed) return;
	if (tm_head == i) tm_head = tm[i].next;
	else {
		int p = tm_head;
		while (tm[p].next != i) p = tm[p].next;
		tm[p].next = tm[i].next;
	}
	tm[i].listed = false;
}

// timer_list_insert: before the first that expires later (a disabled or never one isn't listed)
static void tm_insert(int i) {
	tmr_t *t = &tm[i];
	if (!t->enabled || t->expire == AT_NEVER) return;
	int prev = -1, cur = tm_head;
	while (cur >= 0 && tm[cur].expire <= t->expire) { prev = cur; cur = tm[cur].next; }
	t->next = (int8_t)cur;
	if (prev < 0) tm_head = i;
	else tm[prev].next = (int8_t)i;
	t->listed = true;
}

static inline at_t head_expire(void) { return tm_head >= 0 ? tm[tm_head].expire : AT_NEVER; }

static void tm_adjust(int i, at_t delay, int param, at_t period) {
	tmr_t *t = &tm[i];
	if (in_callback && cb_timer == i) cb_modified = true;
	t->param = param;
	t->enabled = true;
	t->used = true;
	if (delay < 0) delay = 0;
	t->expire = delay == AT_NEVER ? AT_NEVER : now() + delay;
	t->period = period;
	tm_remove(i);
	tm_insert(i);
	if (tm_head == i) abort_timeslice();
}

static void synchronize(int type, int param) {
	for (int i = TM_SYNC; i < TM_COUNT; i++) {
		if (tm[i].used) continue;
		tm[i].type = (uint8_t)type;
		tm[i].used = true;
		tm[i].param = param;
		tm[i].period = AT_NEVER;
		tm[i].expire = now();
		tm[i].enabled = true;
		tm_insert(i);
		// (in a stretch through scanline timers, the fires before now are
		// still listed: MAME would have run them, and this would be its head)
		if (tm_head == i || lazy_lines) abort_timeslice();
		return;
	}
}

// the DMA's perfect_quantum(100 us) (add_quantum(0, duration))
static void perfect_quantum(void) {
	at_t cur = now();
	at_t expire = cur + 100 * USEC_AS;
	if (perfect_on && cur >= perfect_expire) perfect_on = false;
	if (perfect_on) {
		if (expire > perfect_expire) perfect_expire = expire;
	} else {
		perfect_on = true;
		perfect_expire = expire;
	}
}

// ---- input lines (device_input::set_state_synced): a batch of events, applied by a timer now
static struct { uint8_t q[32]; int n; } inq[IN_N];

static void input_apply(int line, int state) {
	switch (line) {
	case IN_Z80_NMI: z80_set_nmi(state, ex[D_Z80].total != 0); break;
	case IN_Z80_BUSREQ: z80_set_busreq(state); break;
	case IN_SND_IRQ: mcs48_set_irq(state); break;
	}
	// (a non-clear state signals the device's interrupt trigger, which only the DMA could wait on: it waits on 1)
}

static void empty_event_queue(int line) {
	for (int i = 0; i < inq[line].n; i++) input_apply(line, inq[line].q[i]);
	inq[line].n = 0;
}

static void set_input_line(int line, int state) {
	if (inq[line].n == (int)sizeof inq[line].q) {   // full: applied now (MAME logs it)
		inq[line].n--;
		empty_event_queue(line);
	}
	inq[line].q[inq[line].n++] = (uint8_t)state;
	if (inq[line].n == 1) synchronize(SY_INPUT, line);
}

// ---------------------------------------------------------------------------
// the 8257 DMA (i8257.cpp), in reverse read/write mode (dkong_base sets it)

enum { ST_SI, ST_S0, ST_S1, ST_S2, ST_S3, ST_SW, ST_S4 };
#define MODE_CHAN_ENABLE(x) ((D.transfer_mode >> (x)) & 1)
#define MODE_ROTATING_PRIORITY ((D.transfer_mode >> 4) & 1)
#define MODE_EXTENDED_WRITE ((D.transfer_mode >> 5) & 1)
#define MODE_TC_STOP ((D.transfer_mode >> 6) & 1)
#define MODE_AUTOLOAD ((D.transfer_mode >> 7) & 1)
#define MODE_TRANSFER_MASK (D.ch[D.current_channel].mode)
enum { XFER_VERIFY, XFER_WRITE, XFER_READ };

static struct {
	int icount, cycles_running, cycles_stolen;
	bool tc, msb, ready;
	int hreq, hack;
	int state, current_channel, last_channel;
	uint8_t transfer_mode, status, request, temp;
	struct { uint16_t address, count; uint8_t mode; } ch[4];
} D;

static uint8_t space_rd(uint16_t a);
static void space_wr(uint16_t a, uint8_t d);

static void dma_abort(void) {
	if (D.icount > 0) {
		D.cycles_stolen += D.icount;
		D.cycles_running -= D.icount;
		D.icount = 0;
	}
}
static int dma_done(void) { return D.cycles_running - D.icount; }
static void dma_keep(int n) {           // the rest of the slice cut to n cycles (if more)
	if (D.icount > n) {
		int delta = D.icount - n;
		D.cycles_stolen += delta;
		D.cycles_running -= delta;
		D.icount = n;
	}
}

static void dma_trigger(void) { dev_trigger(D_DMA, 1); }

static void dma_set_hreq(int state) {
	if (D.hreq != state) {
		set_input_line(IN_Z80_BUSREQ, state);   // out_hrq_cb: the Z80's BUSREQ
		D.hreq = state;
		if (executing == D_DMA) abort_timeslice();
	}
}

static inline void dma_set_tc(int state) { D.tc = state; }  // (TC goes nowhere)

static bool dma_next_channel(void) {
	// (i8257.cpp's priority list: 0 to 3, or rotating, the last channel served last)
	unsigned want = D.request & D.transfer_mode & 0x0f;
	if (!want) return false;
	int c;
	if (MODE_ROTATING_PRIORITY) {
		c = (D.last_channel + 1) & 3;
		while (!((want >> c) & 1)) c = (c + 1) & 3;
	} else c = __builtin_ctz(want);
	D.current_channel = D.last_channel = c;
	return true;
}

static void dma_read(void) {
	uint16_t offset = D.ch[D.current_channel].address;
	switch (MODE_TRANSFER_MASK) {
	case XFER_WRITE: D.temp = D.current_channel == 1 ? dma_latch : 0; break;     // ior: p8257_ctl_r on channel 1
	case XFER_READ: D.temp = space_rd(offset); break;
	default: break;
	}
}

static void dma_write(void) {
	uint16_t offset = D.ch[D.current_channel].address;
	switch (MODE_TRANSFER_MASK) {
	case XFER_WRITE: space_wr(offset, D.temp); break;
	case XFER_READ: if (D.current_channel == 0) dma_latch = D.temp; break;        // iow: p8257_ctl_w on channel 0
	default: break;
	}
}

static void dma_advance(void) {
	bool tc = D.tc;
	bool al = MODE_AUTOLOAD && D.current_channel == 2;
	dma_set_tc(0);
	if (tc) {
		D.status |= (uint8_t)(1u << D.current_channel);
		if (al) {
			D.ch[2] = D.ch[3];
		} else if (MODE_TC_STOP) {
			D.transfer_mode &= (uint8_t)~(1u << D.current_channel);
		}
	}
	if (!(al && tc)) {
		D.ch[D.current_channel].count = (uint16_t)((D.ch[D.current_channel].count - 1) & 0x3fff);
		D.ch[D.current_channel].address++;
	}
}

// the transfer touches only plain memory (no dk_rd or dk_wr, so nothing that
// could stop the 8257 in the middle)
static inline bool dma_plain(void) {
	uint16_t a = D.ch[D.current_channel].address;
	switch (MODE_TRANSFER_MASK) {
	case XFER_READ: return z80_rd_page[a >> 8] != 0;
	case XFER_WRITE: return z80_wr_page[a >> 8] != 0;
	default: return true;
	}
}

static DK_HOT int dma_run(int cycles) {
	D.cycles_running = cycles;
	D.cycles_stolen = 0;
	D.icount = cycles;
	do {
		switch (D.state) {
		case ST_SI:
			dma_set_tc(0);
			if (dma_next_channel()) D.state = ST_S0;
			else {
				D.icount = 0;
				dev_suspend(D_DMA, SUSPEND_REASON_TRIGGER, true);
				ex[D_DMA].trigger = 1;
			}
			break;
		case ST_S0:
			dma_set_hreq(1);
			if (D.hack) D.state = ST_S1;
			else {
				D.icount = 0;
				dev_suspend(D_DMA, SUSPEND_REASON_TRIGGER, true);
				ex[D_DMA].trigger = 1;
			}
			break;
		case ST_S1:
			if (D.icount >= 4 && D.hack && D.ready && dma_plain()) {
				// a transfer's four states in one go, the same steps at the same
				// counts (S1, S2, S3, then S4 here; the loop counts S4's cycle)
				dma_set_tc(0);
				D.icount -= 2;
				dma_read();
				if (MODE_EXTENDED_WRITE) dma_write();
				if (D.ch[D.current_channel].count == 0 && MODE_TRANSFER_MASK != XFER_READ) dma_set_tc(1);
				D.icount -= 1;
				if (!MODE_EXTENDED_WRITE) dma_write();
				if (D.ch[D.current_channel].count == 0 && MODE_TRANSFER_MASK == XFER_READ) dma_set_tc(1);
				dma_advance();
				if (D.hack && dma_next_channel()) D.state = ST_S1;
				else {
					dma_set_hreq(0);
					D.current_channel = -1;
					D.state = ST_SI;
				}
				break;
			}
			dma_set_tc(0);
			D.state = ST_S2;
			break;
		case ST_S2:
			D.state = ST_S3;                    // (DACK goes nowhere)
			break;
		case ST_S3:
			dma_read();
			if (MODE_EXTENDED_WRITE) dma_write();
			if (D.ready) {
				D.state = ST_S4;
				if (D.ch[D.current_channel].count == 0 && MODE_TRANSFER_MASK != XFER_READ) dma_set_tc(1);
			} else D.state = ST_SW;
			break;
		case ST_SW:
			if (D.ready) {
				D.state = ST_S4;
				if (D.ch[D.current_channel].count == 0 && MODE_TRANSFER_MASK != XFER_READ) dma_set_tc(1);
			}
			break;
		case ST_S4:
			if (!MODE_EXTENDED_WRITE) dma_write();
			if (D.ch[D.current_channel].count == 0 && MODE_TRANSFER_MASK == XFER_READ) dma_set_tc(1);
			dma_advance();
			if (D.hack && dma_next_channel()) D.state = ST_S1;
			else {
				dma_set_hreq(0);
				D.current_channel = -1;
				D.state = ST_SI;
			}
			break;
		}
		D.icount--;
	} while (D.icount > 0);
	return cycles - D.icount - D.cycles_stolen;
}

static uint8_t dma_reg_read(int offset) {
	uint8_t data = 0;
	if (!(offset & 8)) {
		int c = (offset >> 1) & 3;
		if (!(offset & 1)) data = D.msb ? (uint8_t)(D.ch[c].address >> 8) : (uint8_t)D.ch[c].address;
		else if (D.msb) {
			data = (uint8_t)(D.ch[c].count >> 8);
			if (D.ch[c].mode) data |= D.ch[c].mode == 1 ? 0x80 : 0x40;
		} else data = (uint8_t)D.ch[c].count;
		D.msb = !D.msb;
	} else if (offset == 8) {
		data = D.status;
		D.status &= 0xf0;
	}
	return data;
}

static void dma_reg_write(int offset, uint8_t data) {
	if (!(offset & 8)) {
		int c = (offset >> 1) & 3;
		if (!(offset & 1)) {
			if (D.msb) {
				D.ch[c].address = (uint16_t)((data << 8) | (D.ch[c].address & 0xff));
				if (MODE_AUTOLOAD && c == 2) D.ch[3].address = (uint16_t)((data << 8) | (D.ch[3].address & 0xff));
			} else {
				D.ch[c].address = (uint16_t)((D.ch[c].address & 0xff00) | data);
				if (MODE_AUTOLOAD && c == 2) D.ch[3].address = (uint16_t)((D.ch[3].address & 0xff00) | data);
			}
		} else {
			if (D.msb) {
				D.ch[c].count = (uint16_t)(((data & 0x3f) << 8) | (D.ch[c].count & 0xff));
				D.ch[c].mode = data >> 6;
				if (D.ch[c].mode) D.ch[c].mode = D.ch[c].mode == 1 ? 2 : 1;     // reverse_rw
				if (MODE_AUTOLOAD && c == 2) {
					D.ch[3].count = (uint16_t)(((data & 0x3f) << 8) | (D.ch[3].count & 0xff));
					D.ch[3].mode = D.ch[2].mode;
				}
			} else {
				D.ch[c].count = (uint16_t)((D.ch[c].count & 0xff00) | data);
				if (MODE_AUTOLOAD && c == 2) D.ch[3].count = (uint16_t)((D.ch[3].count & 0xff00) | data);
			}
		}
		D.msb = !D.msb;
	} else if (offset == 8) {
		D.transfer_mode = data;
	}
	if (D.transfer_mode & D.request & 0x0f) {
		// eat_all_cycles: the executing device's cycles to 0, counted as run
		if (executing == D_Z80) z80_eat();
		else if (executing == D_DMA) D.icount = 0;
		dma_trigger();
	}
}

static void dma_request(int channel, int state) {
	if (state) D.request |= (uint8_t)(1u << channel);
	else D.request &= (uint8_t)~(1u << channel);
	dma_trigger();
}

void dk_busack(int state) {             // hlda_w
	D.hack = state;
	dma_trigger();
}

static void dma_reset(void) {
	D.state = ST_SI;
	D.transfer_mode = 0;
	D.status = 0;
	D.msb = false;
	D.current_channel = -1;
	D.last_channel = 3;
	D.hreq = -1;
	D.tc = false;
	memset(D.ch, 0, sizeof D.ch);
	dma_set_hreq(0);
}

void dk_regs_8257(uint8_t out[16]) {
	for (int c = 0; c < 4; c++) {
		out[c * 3] = (uint8_t)D.ch[c].address;
		out[c * 3 + 1] = (uint8_t)(D.ch[c].address >> 8);
		out[c * 3 + 2] = (uint8_t)D.ch[c].count;
	}
	out[12] = D.transfer_mode;
	out[13] = D.status;
	out[14] = (uint8_t)D.state;
	out[15] = D.request;
}

// ---------------------------------------------------------------------------
// the latches (latch8.cpp) and the sound CPU's glue (dkong_a.cpp)

// the sound's times: ticks of 76.8 MHz (dksnd.h), which keep which of MAME's
// 48 kHz samples a time falls in
static inline uint64_t snd_tick_at(at_t t) { return at_ticks(t, DKSND_TICK_HZ); }

uint64_t dk_snd_now(void) {
	if (executing == D_SND) return snd_tick_at(ex[D_SND].local + (at_t)mcs48_done() * ex[D_SND].apc_mid);
	return snd_tick_at(now());
}

// (a device that didn't run in the last timeslice can be up to a cycle behind
// the base time, so the sound CPU's next write can be up to 2.5 us before it)
uint64_t dk_snd_tick(void) { return snd_tick_at(basetime - ex[D_SND].aspc); }

static void latch_update(int which, uint8_t new_val, uint8_t mask) {
	uint8_t *v = which == SY_LATCH_3D ? &dk_latch_3d : which == SY_LATCH_6H ? &dk_latch_6h : &dk_latch_vp2;
	uint8_t old = *v;
	*v = (uint8_t)((*v & ~mask) | (new_val & mask));
	uint8_t changed = old ^ *v;
	if (!changed) return;
	if (which == SY_LATCH_6H) {         // bits 0, 1, 2 to the discrete inputs (6 and 7 to nodes dkong2b hasn't)
		uint64_t when = snd_tick_at(now());
		if (changed & 1) dk_snd_event(when, DK_WALK, *v & 1);
		if (changed & 2) dk_snd_event(when, DK_JUMP, (*v >> 1) & 1);
		if (changed & 4) dk_snd_event(when, DK_STOMP, (*v >> 2) & 1);
	} else if (which == SY_LATCH_VP2) { // bit 7 to DS_DISCHARGE_INV
		if (changed & 0x80) dk_snd_event(snd_tick_at(now()), DK_DISCHARGE, *v >> 7);
	}
}

static uint8_t vp2_read(void) {         // read_cb<5>: 6H's bit 3; xor 0x20
	uint8_t res = (uint8_t)((dk_latch_vp2 & ~0x20) | (((dk_latch_6h >> 3) & 1) << 5));
	return res ^ 0x20;
}

static uint8_t tune_r(uint8_t offset) {
	uint8_t page = vp2_read() & 0x47;
	if (page & 0x40) return (uint8_t)((((dk_latch_3d & ~0xf0) ^ 0x0f) & 0x0f));
	return roms->tune[(page & 7) * 256 + offset];
}

uint8_t dk_snd_ext_r(uint8_t a) { return tune_r(a); }
uint8_t dk_snd_bus_r(void) { return tune_r(0); }
uint8_t dk_snd_p2_r(void) { return vp2_read(); }
void dk_snd_p1_w(uint8_t d) { dk_snd_event(dk_snd_now(), DK_DAC, d); }
void dk_snd_p2_w(uint8_t d) { synchronize(SY_LATCH_VP2, 0xff00 | d); }
int dk_snd_t0(void) { return (~dk_latch_6h >> 5) & 1; }
int dk_snd_t1(void) { return (~dk_latch_6h >> 4) & 1; }

// ---------------------------------------------------------------------------
// the watchdog (watchdog.cpp: no VBLANK count or time set, so 3 seconds from each reset)

static void watchdog_reset(void) { tm_adjust(TM_WATCHDOG, 3 * AS_PER_SEC, 0, AT_NEVER); }

// ---------------------------------------------------------------------------
// the Z80's memory (dkong_map)

static uint8_t in2_r(void) {
	watchdog_reset();
	uint8_t r = (uint8_t)(dk_in.in2 & ~0x40);
	r |= (uint8_t)(((~dk_latch_vp2 >> 4) & 1) << 6);    // the sound CPU's status (virtual_p2 bit4_q_r)
	return r;
}

// (a lazy slice's Z80 touching the board: see timeslice)
static inline bool lazy_touch(void) {
	if (!lazy_run) return false;
	lazy_rollback = true;
	z80_abort();
	return true;
}

uint8_t dk_rd(uint16_t a) {             // 0x7800-0x7fff
	if ((a < 0x7810 || a == 0x7d00) && lazy_touch()) return 0;     // (the 8257's flip-flop, the watchdog)
	if (a < 0x7810) return dma_reg_read(a & 0x0f);
	switch (a) {
	case 0x7c00: return dk_in.in0;
	case 0x7c80: return dk_in.in1;
	case 0x7d00: return in2_r();
	case 0x7d80: return dk_in.dsw0;
	default: return 0;
	}
}

void dk_wr(uint16_t a, uint8_t d) {
	if (lazy_touch()) return;
	if (a < 0x7810) { dma_reg_write(a & 0x0f, d); return; }
	if (a >= 0x7d00 && a <= 0x7d07) {   // ls259.6h bit0_w
		int b = a & 7;
		synchronize(SY_LATCH_6H, ((1 << b) << 8) | ((d & 1) << b));
		return;
	}
	switch (a) {
	case 0x7c00: synchronize(SY_LATCH_3D, 0xff00 | d); break;
	case 0x7c80: grid_col = (d & 7) ^ 7; break;
	case 0x7d80: set_input_line(IN_SND_IRQ, d ? 1 : 0); break;
	case 0x7d81: grid_on = d & 1; break;
	case 0x7d82: dk_flip = d & 1; break;
	case 0x7d83: dk_sprite_bank = d & 1; break;
	case 0x7d84:
		dk_nmi_mask = d & 1;
		if (!dk_nmi_mask) set_input_line(IN_Z80_NMI, 0);
		break;
	case 0x7d85:                        // p8257_drq_w
		dma_request(0, d & 1);
		dma_request(1, d & 1);
		abort_timeslice();
		perfect_quantum();
		break;
	case 0x7d86: case 0x7d87:
		if (d & 1) dk_palette_bank |= (uint8_t)(1u << (a & 1));
		else dk_palette_bank &= (uint8_t)~(1u << (a & 1));
		break;
	default: break;
	}
}

uint8_t dk_io_in(uint16_t port) { (void)port; return 0; }
void dk_io_out(uint16_t port, uint8_t d) { (void)port; (void)d; }
uint8_t dk_irq_ack(void) { return 0xff; }

static void map_pages(void) {
	for (int p = 0; p < 256; p++) {
		z80_rd_page[p] = zero_page;
		z80_wr_page[p] = sink;
		if (p < 0x40) z80_rd_page[p] = roms->maincpu + (p << 8);
		else if (p >= 0x60 && p < 0x6c) z80_rd_page[p] = z80_wr_page[p] = dk_ram + ((p - 0x60) << 8);
		else if (p >= 0x70 && p < 0x74) z80_rd_page[p] = z80_wr_page[p] = dk_spriteram + ((p - 0x70) << 8);
		else if (p >= 0x74 && p < 0x78) z80_rd_page[p] = z80_wr_page[p] = dk_videoram + ((p - 0x74) << 8);
		else if (p >= 0x78 && p < 0x80) z80_rd_page[p] = 0, z80_wr_page[p] = 0;
	}
}

static uint8_t space_rd(uint16_t a) {
	const uint8_t *p = z80_rd_page[a >> 8];
	return p ? p[a & 0xff] : dk_rd(a);
}

static void space_wr(uint16_t a, uint8_t d) {
	uint8_t *p = z80_wr_page[a >> 8];
	if (p) p[a & 0xff] = d;
	else dk_wr(a, d);
}

uint8_t dk_peek(uint16_t a) {
	const uint8_t *p = z80_rd_page[a >> 8];
	if (p) return p[a & 0xff];
	if (a == 0x7c00) return dk_in.in0;
	if (a == 0x7c80) return dk_in.in1;
	if (a == 0x7d00) return (uint8_t)((dk_in.in2 & ~0x40) | (((~dk_latch_vp2 >> 4) & 1) << 6));
	if (a == 0x7d80) return dk_in.dsw0;
	return 0;
}

// ---------------------------------------------------------------------------
// video (dkong_v.cpp)

uint32_t dk_rgb[256];
uint16_t (*dk_tile_rows)[8];
uint32_t (*dk_sprite_rows)[16];
const uint8_t *dk_color_codes;

// resnet.cpp's compute_res_net for dkong_net_info (inputs from MB7052s: TTL out)
typedef struct { int amp; double rbias; int num; double r[3]; } chan_t;
static int res_net(int inputs, const chan_t *c) {
	const double vcc = 5.0, vOL = 0.05, vOH = 4.0, ttlHRes = 50, vBias = 5.0;
	double minout = 0, cut = 0;
	if (c->amp == 2) { minout = 0.7; cut = 0.0; }       // darlington
	else { minout = 0.0; cut = 0.7; }                   // emitter
	double rTotal = 0, v = 0;
	bool opencol = false;
	for (int i = 0; i < c->num; i++)
		if (c->r[i] != 0.0 && !((inputs >> i) & 1)) { rTotal += 1.0 / c->r[i]; v += vOL / c->r[i]; }
	if (c->rbias != 0.0) { rTotal += 1.0 / c->rbias; v += vBias / c->rbias; }
	if (v / rTotal > vOH) opencol = true;
	for (int i = 0; i < c->num; i++)
		if (c->r[i] != 0.0 && ((inputs >> i) & 1) && !opencol) {
			rTotal += 1.0 / (c->r[i] + ttlHRes);
			v += vOH / (c->r[i] + ttlHRes);
		}
	rTotal = 1.0 / rTotal;
	v *= rTotal;
	v = v - cut > minout ? v - cut : minout;
	// RES_NET_MONITOR_SANYO_EZV20
	v = vcc - v;
	v = v - 0.7 > 0 ? v - 0.7 : 0;
	v = v < vcc - 2 * 0.7 ? v : vcc - 2 * 0.7;
	v = v / (vcc - 1.4);
	v = v * vcc;
	return (int)(v * 255 / vcc + 0.4);
}

static uint8_t clamp8(int32_t v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void palette_init(void) {
	static const chan_t net[3] = {
		{ 2, 470, 3, { 1000, 470, 220 } }, { 2, 470, 3, { 1000, 470, 220 } }, { 3, 680, 2, { 470, 220, 0 } } };
	static const chan_t bck[3] = { { 2, 470, 0, { 0 } }, { 2, 470, 0, { 0 } }, { 3, 680, 0, { 0 } } };
	const uint8_t *prom = roms->proms;
	uint8_t rgb[256][3];
	for (int i = 0; i < 256; i++) {
		// dkong_decode_info: R from c-2j bits 1-3, G from c-2j bit 0 (to bit 2) and c-2k bits 2-3, B from c-2k bits 0-1
		uint8_t t[3];
		t[0] = (uint8_t)((prom[i + 256] >> 1) & 0x07);
		t[1] = (uint8_t)(((prom[i + 256] << 2) & 0x04) | ((prom[i] >> 2) & 0x03));
		t[2] = (uint8_t)(prom[i] & 0x03);
		for (int k = 0; k < 3; k++) rgb[i][k] = (uint8_t)res_net(t[k], &net[k]);
		if ((i & 3) == 0)
			for (int k = 0; k < 3; k++) rgb[i][k] = (uint8_t)res_net(1, &bck[k]);
	}
	// palette_t::normalize_range(0, 255) to luminances 0-255
	int32_t ymin = 1000 * 255, ymax = 0;
	for (int i = 0; i < 256; i++) {
		int32_t y = 299 * rgb[i][0] + 587 * rgb[i][1] + 114 * rgb[i][2];
		if (y < ymin) ymin = y;
		if (y > ymax) ymax = y;
	}
	const int32_t tmin = 0, tmax = 255;
	for (int i = 0; i < 256; i++) {
		int32_t y = 299 * rgb[i][0] + 587 * rgb[i][1] + 114 * rgb[i][2];
		int32_t u = ((int32_t)rgb[i][2] - y / 1000) * 492 / 1000;
		int32_t v = ((int32_t)rgb[i][0] - y / 1000) * 877 / 1000;
		int32_t target = tmin + ((y - ymin) * (tmax - tmin + 1)) / (ymax - ymin);
		uint8_t r = clamp8(target + 1140 * v / 1000);
		uint8_t g = clamp8(target - 395 * u / 1000 - 581 * v / 1000);
		uint8_t b = clamp8(target + 2032 * u / 1000);
		dk_rgb[i] = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
	}
	dk_color_codes = prom + 0x200;
}

static void gfx_init(void) {
	// gfx_8x8x2_planar: pen bit 1 from the second half, bit 0 from the first; x 0 is a byte's bit 7
	const uint8_t *g = roms->gfx1;
	for (int t = 0; t < 256; t++)
		for (int y = 0; y < 8; y++) {
			uint8_t lo = g[t * 8 + y], hi = g[0x800 + t * 8 + y];
			uint16_t row = 0;
			for (int x = 0; x < 8; x++) {
				int pen = (((hi >> (7 - x)) & 1) << 1) | ((lo >> (7 - x)) & 1);
				row |= (uint16_t)(pen << (2 * x));
			}
			dk_tile_rows[t][y] = row;
		}
	// spritelayout: planes at 0x1000 (bit 1) and 0 (bit 0), x 0-7 then 8-15 0x800 on, 16 bytes a sprite
	const uint8_t *s = roms->gfx2;
	for (int n = 0; n < 128; n++)
		for (int y = 0; y < 16; y++) {
			uint32_t row = 0;
			for (int x = 0; x < 16; x++) {
				int o = n * 16 + y + (x >= 8 ? 0x800 : 0);
				int b = 7 - (x & 7);
				int pen = (((s[0x1000 + o] >> b) & 1) << 1) | ((s[o] >> b) & 1);
				row |= (uint32_t)pen << (2 * x);
			}
			dk_sprite_rows[n][y] = row;
		}
}

// one line of the picture as screen_update_dkong draws it, from the state now
// (MAME draws a line at its start, from the scanline timer's partial update)
void dk_render_line(uint8_t *row, int line) {
	// the tilemap, flipped both ways when dk_flip
	int ty = dk_flip ? 255 - line : line;
	for (int x = 0; x < DK_W; x++) {
		int tx = dk_flip ? 255 - x : x;
		int ti = (ty >> 3) * 32 + (tx >> 3);
		int code = dk_videoram[ti];
		int color = (dk_color_codes[ti % 32 + 32 * (ti / 32 / 4)] & 0x0f) + 0x10 * dk_palette_bank;
		int pen = (dk_tile_rows[code][ty & 7] >> (2 * (tx & 7))) & 3;
		row[x] = (uint8_t)(color * 4 + pen);
	}
	// the sprites: the first 16 (in RAM order) the line buffer picked on the line before
	int scanline_vf = (line - 1) & 0xff, scanline_vfc = (line - 1) & 0xff, scanline = line & 0xff;
	int add_y, add_x;
	if (dk_flip) { scanline_vf ^= 0xff; scanline_vfc ^= 0xff; add_y = 0xf7; add_x = 0xf7; }
	else { add_y = 0xf9; add_x = 0xf7; }
	const uint8_t *spr = dk_spriteram + (dk_sprite_bank << 9);
	int num = 0;
	for (int offs = 0; num < 16 && offs < 0x200; offs += 4) {
		int y = spr[offs];
		if (((y + add_y + 1 + scanline_vf) & 0xf0) != 0xf0) continue;
		int code = (spr[offs + 1] & 0x7f) + ((spr[offs + 2] & 0x40) << 1);
		int color = (spr[offs + 2] & 0x0f) + 16 * dk_palette_bank;
		int flipx = spr[offs + 2] & 0x80;
		int flipy = spr[offs + 1] & 0x80;
		int x = (spr[offs + 3] + add_x + 1) & 0xff;
		if (dk_flip) { x = (x ^ 0xff) - 15; flipx = !flipx; }
		int top = scanline - ((y + add_y + 1 + scanline_vfc) & 0x0f);
		int sy = line - top;            // this line's row of the sprite, 0-15
		(void)scanline;
		if (sy < 0 || sy > 15) { num++; continue; }
		uint32_t bits = dk_sprite_rows[code & 0x7f][flipy ? 15 - sy : sy];
		for (int pass = 0; pass < 2; pass++) {
			int x0 = pass == 0 ? x : (dk_flip ? x + 256 : x - 256);
			for (int i = 0; i < 16; i++) {
				int px = x0 + i;
				if (px < 0 || px >= DK_W) continue;
				int pen = (bits >> (2 * (flipx ? 15 - i : i))) & 3;
				if (pen) row[px] = (uint8_t)(color * 4 + pen);
			}
		}
		num++;
	}
}

void dk_render(uint8_t fb[DK_H][DK_W]) {
	for (int y = 0; y < DK_H; y++) dk_render_line(fb[y], y + 16);
}

// ---------------------------------------------------------------------------
// the screen's timers and the scanline timer

static at_t vblank_start_time;
static bool frame_done;

static at_t time_until_pos_at(int v, at_t t) {
	v += VTOTAL - VBSTART;
	v %= VTOTAL;
	int64_t target = (int64_t)v * SCAN_AS;
	int64_t cur = t - vblank_start_time;
	if (target <= cur + PIX_AS / 2) target += FRAME_AS;
	while (target <= cur) target += FRAME_AS;
	return target - cur;
}
static at_t time_until_pos(int v) { return time_until_pos_at(v, now()); }

static int vpos(void) {
	int64_t delta = now() - vblank_start_time;
	delta += PIX_AS / 2;
	int v = (int)(delta / SCAN_AS);
	return (VBSTART + v) % VTOTAL;
}

static void cb_vbstart(void) {
	vblank_start_time = now();
	dk_frames++;                        // video().frame_update(): the picture is RAM as it is now
	frame_done = true;
	if (dk_on_vblank) dk_on_vblank();
	if (dk_nmi_mask) set_input_line(IN_Z80_NMI, 1);    // screen_vblank -> vblank_irq
	tm_adjust(TM_VBSTART, time_until_pos(VBSTART), 0, AT_NEVER);
	tm_adjust(TM_VBEND, vblank_start_time + VBLANK_AS - now(), 0, AT_NEVER);
}

static void cb_scanline(int line) {
	if (dk_on_line) dk_on_line(vpos());
	line = (line + 1) % VTOTAL;
	tm_adjust(TM_SCANLINE, time_until_pos(line), line, AT_NEVER);
}

static void board_reset(void);

static void cb_sync(int type, int param) {
	switch (type) {
	case SY_LATCH_3D: case SY_LATCH_6H: case SY_LATCH_VP2:
		latch_update(type, (uint8_t)param, (uint8_t)(param >> 8));
		break;
	case SY_INPUT:
		empty_event_queue(param);
		break;
	}
}

static DK_HOT void execute_timers(void) {
	while (tm_head >= 0 && tm[tm_head].expire <= basetime) {
		int h = tm_head;
		tmr_t *t = &tm[h];
		bool was_enabled = t->enabled;
		if (t->period == 0 || t->period == AT_NEVER) t->enabled = false;
		in_callback = true;
		cb_timer = h;
		cb_expire = t->expire;
		cb_modified = false;
		if (was_enabled) {
			switch (h) {
			case TM_SND50: dk_snd_event(snd_tick_at(now()), DK_SNDSYNC, 0); break;   // the streams' update
			case TM_VBSTART: cb_vbstart(); break;
			case TM_VBEND: break;
			case TM_SCAN0: tm_adjust(TM_SCAN0, time_until_pos(0), 0, AT_NEVER); break;
			case TM_WATCHDOG: watchdog_fired = true; break;
			case TM_SCANLINE: cb_scanline(t->param); break;
			default: cb_sync(t->type, t->param); break;
			}
		}
		if (!cb_modified) {
			if (h >= TM_SYNC) {
				tm_remove(h);
				t->used = false;
			} else {                            // schedule_next_period
				t->expire = t->period == AT_NEVER ? AT_NEVER : t->expire + t->period;
				tm_remove(h);
				tm_insert(h);
			}
		}
		in_callback = false;
		cb_timer = -1;
	}
}

static void rebase(void) {             // a second on: every time less 1e18
	epoch_s++;
	basetime -= AS_PER_SEC;
	for (int d = 0; d < D_N; d++) ex[d].local -= AS_PER_SEC;
	for (int i = 0; i < TM_COUNT; i++) if (tm[i].used && tm[i].expire != AT_NEVER) tm[i].expire -= AS_PER_SEC;
	if (perfect_on) perfect_expire -= AS_PER_SEC;
	lazy_off_until -= AS_PER_SEC;
	if (lazy_cap != AT_NEVER) lazy_cap -= AS_PER_SEC;
	vblank_start_time -= AS_PER_SEC;
}

// one device's part of a slice (the loop in device_scheduler::timeslice),
// with its clock's constants
static inline __attribute__((always_inline)) void slice_dev(int d, at_t *target, bool batch, at_t g0, bool lazy) {
	exec_t *e = &ex[d];
	if (e->suspend && !e->eatcycles) return;
	const int64_t aspc = d == D_SND ? S_AS : Z_AS;
	int64_t delta = *target - e->local;
	if (delta < aspc) return;
	int cycles = d == D_SND ? div_const((uint64_t)delta >> S_SHIFT, S_DIV) : div_const((uint64_t)delta >> Z_SHIFT, Z_DIV);
	if (lazy && lazy_floor[d]) cycles--;
	int ran = cycles;
	if (!e->suspend) {
		executing = d;
		if (batch) { batching = true; batch_dev = d; cut_done = -1; }
		if (lazy && d == D_Z80) { lazy_run = true; z80_snap(); ON_LAZY(0); }
		if (d == D_Z80) ran = z80_run(cycles);
		else if (d == D_DMA) ran = dma_run(cycles);
		else ran = mcs48_run(cycles);
		if (lazy && d == D_Z80) {
			if (lazy_rollback || z80_undo_full) {
				lazy_touch_at = e->local + (at_t)ran * aspc;
				z80_rollback(); lazy_rollback = true; ran = 0;
				ON_LAZY(1);
			}
			else z80_unsnap();
		}
		batching = false;
		lazy_run = false;
		executing = -1;
	}
	e->total += (uint64_t)ran;
	e->local += (int64_t)ran * aspc;
	if (batch && cut_done >= 0) {
		int64_t m = cut_done + 1 - batch_q;
		at_t end = g0 + (m > 0 ? m : 0) * PERFECT_AS;
		if (end < *target) *target = end;
	}
	if (e->local < *target) *target = e->local > basetime ? e->local : basetime;
}

// the cycles the slice to g0 would give a device at z (Z80 clock), as a
// floor (negative when it's ahead)
static int64_t first_cycles(at_t g0, at_t z) {
	return g0 >= z ? (int64_t)((uint64_t)(g0 - z) / (uint64_t)Z_AS) : -(int64_t)((uint64_t)(z - g0 + Z_AS - 1) / (uint64_t)Z_AS);
}

// A lazy slice: when nothing but the Z80 and the sound CPU runs (the 8257
// suspended, eating), with no perfect quantum, no suspend change, no DMA
// request and the Z80's bus settled, a stretch to the next timer runs as one
// slice: the Z80 to its end, then the sound CPU, then the 8257. The same,
// cycle for cycle, while the Z80 doesn't touch the board: each CPU's steps
// depend only on its own inputs, which change only at timers (the sound
// CPU's write to its status, which the Z80 reads in IN2, is a sync, which
// ends the sound CPU's part as it would end its slice), and where each is
// when a timer fires is the first chunk end past its last budget, however
// the stretch was cut. When the Z80 does touch the board (a write, or a
// read of the 8257 or IN2), where the sound CPU and the 8257 would have been
// then depends on the slices MAME would have made, so the Z80's part is
// taken back (its state kept at the start, its memory writes logged) and the
// stretch runs in slices (some five stretches a frame). A stretch can go
// through the scanline timer's fires (lazy_end), which change nothing but
// the timer: then a touch makes the next try end at the last fire before
// it, and the line it was in runs in slices. In such a stretch the sound
// CPU's syncs (its writes to virtual_p2) stop it as they would in MAME,
// where the fires before them would have gone (synchronize). The one other
// difference the cycles given could make is MAME's divide (divu_64x32 of the
// shifted time): for the Z80's clock it gives one cycle more when the timer
// is within Z_REM attoseconds a cycle of the next cycle's start, and that
// depends on how long the last slice was: eps_ok() says when it can't, or
// makes it the floor as the last slice would.
#define Z_REM ((int64_t)Z_AS - ((int64_t)Z_DIV << Z_SHIFT))     // 85
static inline __attribute__((always_inline)) bool eps_ok(at_t end, at_t local, bool *floor_it) {
	*floor_it = false;
	if (end - local < Z_AS) return true;
	uint64_t x = (uint64_t)(end - local);
	uint32_t q = (uint32_t)div_const(x >> Z_SHIFT, Z_DIV);  // MAME's divide: the floor, or one more
	uint64_t qz = (uint64_t)q * (uint64_t)Z_AS;
	if (__builtin_expect(qz <= x, 1)) return true;          // the floor: so for any shorter slice
	uint64_t eps = qz - x;                                    // (one more: q cycles, x short of them)
	if (eps <= (uint64_t)Z_REM) return true;                 // one more for any slice
	if (eps > (uint64_t)Z_REM * 60) { *floor_it = true; return true; }     // a last slice (53 cycles at most) gets the floor
	return false;
}

// The end of a lazy stretch from the head timer at he: he, or, when he is
// the scanline timer's and nothing watches the lines (dk_on_line), up to
// DK_LAZY_LINES of its fires on, before any other timer and before
// lazy_cap. Its callback only sets it again for the next line (from the
// time it fired and the VBLANK's start, which only another timer moves), so
// its fires are known ahead, and they run after the stretch, each at its
// own time (execute_timers).
static at_t lazy_end(at_t he) {
	if (tm_head != TM_SCANLINE || dk_on_line) return he;
	int nx = tm[TM_SCANLINE].next;
	at_t other = nx >= 0 ? tm[nx].expire : AT_NEVER;
	if (lazy_cap < other) other = lazy_cap;
	at_t e = he;
	int line = tm[TM_SCANLINE].param;
	for (int n = 1; n < DK_LAZY_LINES; n++) {
		line = (line + 1) % VTOTAL;
		at_t e2 = e + time_until_pos_at(line, e);
		if (e2 >= other) break;
		e = e2;
	}
	return e;
}

static bool lazy_ok(at_t he, at_t *end) {
#ifdef DK_NO_LAZY
	return false;
#endif
	if (perfect_on || suspend_changes_pending || D.request || basetime < lazy_off_until || he == AT_NEVER) return false;
	if (basetime >= lazy_cap) lazy_cap = AT_NEVER;
	if (he >= lazy_cap) {               // the touch is before the first timer: this one in slices
		lazy_off_until = he;
		lazy_cap = AT_NEVER;
		return false;
	}
	at_t e = lazy_end(he);
	if (e - basetime <= QUANTUM_AS) return false;
	if (ex[D_Z80].suspend || ex[D_SND].suspend || !ex[D_DMA].suspend || !ex[D_DMA].eatcycles) return false;
	if (order[0] != D_Z80 || order[1] != D_SND || !z80_bus_settled()) return false;
	lazy_floor[D_SND] = false;
	*end = e;
	return eps_ok(e, ex[D_Z80].local, &lazy_floor[D_Z80]) && eps_ok(e, ex[D_DMA].local, &lazy_floor[D_DMA]);
}

static DK_HOT void timeslice(void) {
	if (basetime >= AS_PER_SEC) rebase();
	if (perfect_on && basetime >= perfect_expire) perfect_on = false;
	at_t he;
	while (basetime < (he = head_expire())) {
		at_t target, le;
		if (lazy_ok(he, &le)) {
			target = le;
			slice_dev(D_Z80, &target, false, 0, true);
			if (lazy_rollback) {        // (see lazy_ok)
				// again, ending before the touch: in the end the line it's in
				// runs in slices, alone
				lazy_rollback = false;
				lazy_cap = lazy_touch_at < le ? lazy_touch_at : le;
				dk_lazy_rollbacks++;
				continue;
			}
			lazy_lines = le != he;
			slice_dev(D_SND, &target, false, 0, true);
			lazy_lines = false;
			slice_dev(D_DMA, &target, false, 0, true);
			basetime = target;
			continue;
		}
		target = basetime + (perfect_on ? PERFECT_AS : QUANTUM_AS);
		if (he < target) target = he;
		// a slice to the timer in which no device has a cycle to run (after
		// one pulled back short of it) changes nothing but the base time
		if (target == he && !suspend_changes_pending && he - ex[D_Z80].local < Z_AS && he - ex[D_SND].local < S_AS &&
		    (he - ex[D_DMA].local < Z_AS || (ex[D_DMA].suspend && !ex[D_DMA].eatcycles))) {
			basetime = he;
			continue;
		}
		if (suspend_changes_pending) apply_suspend_changes();
		// In the perfect quantum's slices (a Z80 cycle each), while the 8257
		// is suspended, the Z80 runs alone until the sound CPU's next cycle
		// is due: those slices go as one, to the first slice end at which the
		// sound CPU runs (or the head timer). The same, cycle for cycle: the
		// Z80's steps don't depend on how its cycles are given out, nothing
		// else runs before then, and a cut (batch_cut) ends it where the
		// slices would have changed. The slice ends stay on the Z80's grid.
		bool batch = false, dbatch = false;
		at_t g0 = target;
		if (perfect_on && target < he && !ex[D_SND].suspend) {
			bool z_alone = ex[D_DMA].suspend;
			// (the 8257 copying while the Z80 has given it the bus: the Z80's
			// runs only eat, so it eats after the 8257 in each slice instead)
			bool d_alone = !z_alone && order[0] == D_Z80 && order[1] == D_DMA && z80_stalled();
			if (z_alone || d_alone) {
				at_t due = ex[D_SND].local + S_AS;
				bool more = false;
				while (target < due && target < he) { target += PERFECT_AS; more = true; }
				if (he < target) target = he;
				if (more) {
					batch = z_alone;
					dbatch = d_alone;
					batch_q = first_cycles(g0, ex[z_alone ? D_Z80 : D_DMA].local);
				}
			}
		}
		// the execute list as it nearly always is: the Z80 first, then the
		// sound CPU and the 8257 (suspended, last) or the 8257 and the sound CPU
		if (dbatch) {
			slice_dev(D_DMA, &target, true, g0, false);
			slice_dev(D_Z80, &target, false, 0, false);
			slice_dev(D_SND, &target, false, 0, false);
		} else if (order[0] == D_Z80) {
			slice_dev(D_Z80, &target, batch, g0, false);
			if (order[1] == D_SND) { slice_dev(D_SND, &target, false, 0, false); slice_dev(D_DMA, &target, false, 0, false); }
			else { slice_dev(D_DMA, &target, false, 0, false); slice_dev(D_SND, &target, false, 0, false); }
		} else {
			for (int i = 0; i < D_N; i++) {
				int d = order[i];
				if (d == D_Z80) slice_dev(D_Z80, &target, batch, g0, false);
				else if (d == D_DMA) slice_dev(D_DMA, &target, false, 0, false);
				else slice_dev(D_SND, &target, false, 0, false);
			}
		}
		basetime = target;
	}
	execute_timers();
}

// ---------------------------------------------------------------------------

uint64_t dk_cycles(void) { return ex[D_Z80].total + (executing == D_Z80 ? (uint64_t)z80_done() : 0); }
uint64_t dk_snd_cycles(void) { return ex[D_SND].total + (executing == D_SND ? (uint64_t)mcs48_done() : 0); }
uint64_t dk_time(void) { return at_ticks(basetime, DK_CLOCK); }

static void board_reset(void) {         // device_reset in MAME's order
	for (int d = 0; d < D_N; d++) {     // interface_pre_reset: total cycles to 0, resumed
		ex[d].total = 0;
		ex[d].nextsuspend = 0;
		suspend_changes_pending = true;
	}
	z80_reset();
	inq[IN_Z80_NMI].n = inq[IN_Z80_BUSREQ].n = 0;       // interface_post_reset: the input lines
	dma_reset();
	dk_latch_3d = dk_latch_6h = dk_latch_vp2 = 0;
	mcs48_reset();
	inq[IN_SND_IRQ].n = 0;
	// the discrete device's reset (after the sound CPU's, which wrote 0xff to the DAC)
	dk_snd_event(dk_snd_now(), DK_SNDRESET, 0);
	tm_adjust(TM_WATCHDOG, AT_NEVER, 0, AT_NEVER);     // the watchdog: off until the first read of IN2
	tm[TM_WATCHDOG].enabled = false;
	watchdog_fired = false;
}

void dk_init(const dk_roms_t *r) {
	roms = r;
	mcs48_rom = r->sound;
	dk_tile_rows = (uint16_t (*)[8])r->rows;
	dk_sprite_rows = (uint32_t (*)[16])(void *)((uint8_t *)r->rows + 256 * 8 * 2);
	memset(tm, 0, sizeof tm);
	tm_head = -1;
	epoch_s = 0;
	basetime = 0;
	executing = -1;
	in_callback = false;
	cb_timer = -1;
	perfect_on = false;
	exec_init(&ex[D_Z80], DK_CLOCK, DK_CLOCK, 1);
	exec_init(&ex[D_DMA], DK_CLOCK, DK_CLOCK, 1);
	exec_init(&ex[D_SND], DK_SND_CLOCK, 6000000, 15);  // the MB8884: 6 MHz, 15 clocks a cycle
	for (int d = 0; d < D_N; d++) ex[d].suspend = ex[d].nextsuspend = 1;   // SUSPEND_REASON_RESET until reset
	rebuild_execute_list();
	memset(dk_ram, 0, sizeof dk_ram);
	memset(dk_spriteram, 0, sizeof dk_spriteram);
	memset(dk_videoram, 0, sizeof dk_videoram);
	memset(&D, 0, sizeof D);
	D.ready = true;
	memset(inq, 0, sizeof inq);
	dk_flip = dk_sprite_bank = dk_palette_bank = dk_nmi_mask = 0;
	dma_latch = grid_on = grid_col = 0;
	dk_frames = dk_resets = 0;
	vblank_start_time = 0;
	palette_init();
	gfx_init();
	map_pages();

	// the sound manager's 50 Hz timer, made before the devices start
	tm_adjust(TM_SND50, AS_PER_SEC / 50, 0, AS_PER_SEC / 50);
	// device_start, in MAME's order: the CPU, the DMA, the screen's timers, the sound CPU
	z80_start();
	tm_adjust(TM_VBSTART, time_until_pos(VBSTART), 0, AT_NEVER);
	tm_adjust(TM_SCAN0, time_until_pos(0), 0, AT_NEVER);
	mcs48_start();
	// the driver's video_start: the scanline timer
	tm_adjust(TM_SCANLINE, time_until_pos(0), 0, AT_NEVER);
	board_reset();
}

uint32_t dk_run_to(uint64_t until) {
	uint32_t f0 = dk_frames;
	while (dk_time() < until) timeslice();
	return dk_frames - f0;
}

bool dk_frame(void) {
	frame_done = false;
	while (!frame_done) timeslice();
	if (watchdog_fired) {               // schedule_soft_reset: every device's reset
		watchdog_fired = false;
		dk_resets++;
		board_reset();
		return false;
	}
	return true;
}
