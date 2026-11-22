// license:BSD-3-Clause
// copyright-holders:K.Wilkins, Couriersud, Derrick Renaud, Frank Palazzolo
//
// dksnd.c -- see dksnd.h. Each node is MAME 0.288's (sound/disc_wav.hxx,
// disc_flt.hxx, disc_mth.hxx, disc_dev.hxx, disc_inp.hxx and the custom
// mixer in audio/dkong_a.cpp), for the values dkong2b_discrete gives it,
// stepped in the network's order. MAME's license (BSD-3-Clause) is in
// README.md.
#include "dksnd.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "dk.h"

#ifdef DKSND_FLOAT
typedef float real;
#define EXP expf
#define LOG logf
// (the cart: a divide by a constant as a multiply by its inverse, 1 cycle
// for the FPU's 14; the last bit of a step can differ from the divide's)
#define DIVK(x, k) ((x) * (real)(1.0 / (k)))
#else
typedef double real;
#define EXP exp
#define LOG log
#define DIVK(x, k) ((x) / (k))
#endif

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define DKSND_HOT __attribute__((section(".time_critical.dksnd")))
#else
#define DKSND_HOT
#endif

// the parts (audio/dkong_a.cpp)
#define DK_SUP_V 5.0
#define RES_K(r) ((r) * 1e3)
#define RES_M(r) ((r) * 1e6)
#define CAP_U(c) ((c) * 1e-6)
#define CAP_N(c) ((c) * 1e-9)
#define RES_2_PARALLEL(r1, r2) (((r1) * (r2)) / ((r1) + (r2)))
#define RES_VOLTAGE_DIVIDER(r1, r2) ((r2) / ((r1) + (r2)))
#define DK_R3 RES_K(5.1)
#define DK_R4 RES_K(2)
#define DK_R5 750
#define DK_R6 RES_K(4.7)
#define DK_R7 RES_K(10)
#define DK_R8 RES_K(100)
#define DK_R9 RES_K(10)
#define DK_R10 RES_K(10)
#define DK_R15 RES_K(5.6)
#define DK_R16 RES_K(5.6)
#define DK_R17 RES_K(10)
#define DK_R18 RES_K(4.7)
#define DK_R20 RES_K(10)
#define DK_R25 RES_K(5.1)
#define DK_R26 RES_K(2)
#define DK_R27 150
#define DK_R28 RES_K(4.7)
#define DK_R29 RES_K(10)
#define DK_R30 RES_K(100)
#define DK_R31 RES_K(10)
#define DK_R32 RES_K(10)
#define DK_R35 RES_K(1)
#define DK_R36 RES_K(1)
#define DK_R38 RES_K(18)
#define DK_R39 RES_M(3.3)
#define DK_R44 RES_K(1.2)
#define DK_R45 RES_K(10)
#define DK_R46 RES_K(12)
#define DK_R47 RES_K(4.3)
#define DK_R48 RES_K(43)
#define DK_R49 RES_K(1.2)
#define DK_R50 RES_K(10)
#define DK_R51 RES_K(10)
#define DK_C13 CAP_U(33)
#define DK_C16 CAP_U(1)
#define DK_C17 CAP_U(4.7)
#define DK_C18 CAP_U(1)
#define DK_C19 CAP_U(1)
#define DK_C20 CAP_U(3.3)
#define DK_C21 CAP_U(1)
#define DK_C23 CAP_U(4.7)
#define DK_C24 CAP_U(10)
#define DK_C25 CAP_U(3.3)
#define DK_C26 CAP_U(3.3)
#define DK_C29 CAP_U(3.3)
#define DK_C30 CAP_U(10)
#define DK_C32 CAP_U(10)
#define DK_1N5553_V 0.4
#define CLOCK_2VF (61440000.0 / 5 / 4 / 16 / 12 / 2 / 2)   // 4 kHz: the noise's clock

static double st;                       // a step's time (sample_time())
static real st_r;                       // the same as a real (no conversion each step)
static real inv_st_r;                   // (the cart) 1 / st_r
static uint64_t step_q16;               // a step's length, ticks << 16
#define LFSR_TICKS_Q16 ((uint64_t)(DKSND_TICK_HZ / 4000) << 16)     // CLOCK_2VF's period

static inline double rc_charge_exp(double rc) { return 1.0 - exp(-st / rc); }
static inline double rc_discharge_exp(double rc) { return exp(-st / rc); }
// std::min and std::max as MAME uses them (a NaN goes the same way)
static inline real smin(real a, real b) { return (b < a) ? b : a; }
static inline real smax(real a, real b) { return (a < b) ? b : a; }

// ---- DSS_LFSR_NOISE with dkong_lfsr: 24 bits, F0 = bit 10 ^ bit 23, the
// output F0, clocked at 4 kHz, amplitude 1, bias 0.5
typedef struct { double t_left, t_clock; uint64_t left_q16; uint32_t reg; real out; } lfsr_t;

static void lfsr_reset(lfsr_t *n) {
	n->t_clock = 1.0 / CLOCK_2VF;
	n->t_left = 0;
	n->left_q16 = 0;
	n->reg = 0;                         // reset value 0; F0 of it 0
	n->out = 0;                         // (bit 0 of 0: -0.5 + 0.5)
}

static inline real lfsr_step(lfsr_t *n) {
#ifdef DKSND_FLOAT
	// (the cart: the clock's 19,200 ticks counted in the steps' ticks, no doubles)
	n->left_q16 += step_q16;
	int inc = 0;
	while (n->left_q16 >= LFSR_TICKS_Q16) { n->left_q16 -= LFSR_TICKS_Q16; inc++; }
#else
	double cycles = (n->t_left + st) / n->t_clock;
	int inc = (int)cycles;
	n->t_left = (cycles - inc) * n->t_clock;
#endif
	if (inc > 0) {
		int fbresult = 0;
		for (int c = 0; c < inc; c++) {
			fbresult = (int)((n->reg >> 24) & 1);
			fbresult ^= 1;              // F1: NOT in0 (the feed is 0)
			n->reg = ((n->reg << 1) & 0xffffff) | (uint32_t)fbresult;     // F2: replace, into bit 0
			fbresult = (int)(((n->reg >> 10) ^ (n->reg >> 23)) & 1);      // F0: xor
			n->reg |= (uint32_t)fbresult << 24;
		}
		n->out = (fbresult & 1) ? 1 : 0;
	}
	return n->out;
}

// ---- DSS_COUNTER 0-7, up, on the clock's rising edges
typedef struct { int last_clock; uint32_t count; } counter_t;

static inline real counter_step(counter_t *n, real clk) {
	int clock = (int)clk != 0;
	if (n->last_clock != clock) {
		n->last_clock = clock;
		if (clock == 1) {
			n->count++;
			while (n->count > 7) n->count -= 8;
		}
	}
	return (real)n->count;
}

// ---- DST_RCDISC_MOD
typedef struct { real exp_low[2], gain[2], exp_high[4], vd_gain[4], v_cap, vp; } rcdmod_t;

static void rcdmod_reset(rcdmod_t *n, double r1, double r2, double r3, double r4, double c, double vp) {
	double rc[2], rc2[2];
	rc[0] = r1 + r2;
	if (rc[0] < 1) rc[0] = 1;
	n->exp_low[0] = (real)rc_discharge_exp(c * rc[0]);
	n->gain[0] = (real)RES_VOLTAGE_DIVIDER(rc[0], r4);
	rc[1] = r2;
	if (rc[1] < 1) rc[1] = 1;
	n->exp_low[1] = (real)rc_discharge_exp(c * rc[1]);
	n->gain[1] = (real)RES_VOLTAGE_DIVIDER(rc[1], r4);
	rc2[0] = r4;
	rc2[1] = RES_2_PARALLEL(r3, r4);
	n->exp_high[0] = (real)rc_discharge_exp(c * (rc[0] + rc2[0]));
	n->vd_gain[0] = (real)RES_VOLTAGE_DIVIDER(rc[0], rc2[0]);
	n->exp_high[1] = (real)rc_discharge_exp(c * (rc[1] + rc2[0]));
	n->vd_gain[1] = (real)RES_VOLTAGE_DIVIDER(rc[1], rc2[0]);
	n->exp_high[2] = (real)rc_discharge_exp(c * (rc[0] + rc2[1]));
	n->vd_gain[2] = (real)RES_VOLTAGE_DIVIDER(rc[0], rc2[1]);
	n->exp_high[3] = (real)rc_discharge_exp(c * (rc[1] + rc2[1]));
	n->vd_gain[3] = (real)RES_VOLTAGE_DIVIDER(rc[1], rc2[1]);
	n->v_cap = 0;
	n->vp = (real)vp;
}

static inline real rcdmod_step(rcdmod_t *n, real in1, real in2) {
	real diff, v_cap = n->v_cap, u, vD, out;
	int mod1 = in1 > (real)0.5, mod2 = in2 > (real)0.6;
	int ms = (mod2 << 1) + mod1;
	u = mod1 ? 0 : n->vp;
	diff = u - v_cap;
	vD = diff * n->vd_gain[ms];
	if (vD < (real)-0.6) {
		diff = u + (real)0.6 - v_cap;
		diff -= diff * n->exp_low[mod1];
		v_cap += diff;
		out = mod2 ? 0 : (real)-0.6;
	} else {
		diff -= diff * n->exp_high[ms];
		v_cap += diff;
		out = mod2 ? 0 : (u - v_cap) * n->gain[mod1];
	}
	n->v_cap = v_cap;
	return out;
}

// ---- DST_RCDISC2
typedef struct { real e0, e1, v; } rcd2_t;

static void rcd2_reset(rcd2_t *n, double r0, double r1, double c) {
	n->v = 0;
	n->e0 = (real)rc_discharge_exp(r0 * c);
	n->e1 = (real)rc_discharge_exp(r1 * c);
}

static inline real rcd2_step(rcd2_t *n, real sw, real in0, real in1) {
	real diff = ((sw == 0) ? in0 : in1) - n->v;
	diff = diff - diff * ((sw == 0) ? n->e0 : n->e1);
	n->v += diff;
	return n->v;
}

// ---- DST_DIODE_MIX of two, junctions 0.4 and 0.8 V
static inline real diode_mix2(real a, real b) {
	real max = 0, val = a - (real)DK_1N5553_V;
	if (val > max) max = val;
	val = b - (real)(DK_1N5553_V * 2);
	if (val > max) max = val;
	if (max < 0) max = 0;
	return max;
}

// ---- DST_RCINTEGRATE
#define IES 7e-15
#define ALPHAT 0.99
#define KT 0.026
typedef struct {
	real r1, r2, r3, vp, gain_r1_r2, f, exp0, exp1, c_exp0, c_exp1, em_ic_0_7, vCap, vCE, inv_r2;
	int type;
} rcint_t;

static void rcint_reset(rcint_t *n, double r1, double r2, double r3, double c, double vp, int type) {
	n->type = type;
	n->vCap = n->vCE = 0;
	n->r1 = (real)r1; n->r2 = (real)r2; n->r3 = (real)r3; n->vp = (real)vp;
	n->inv_r2 = (real)(1.0 / r2);
	n->gain_r1_r2 = (real)RES_VOLTAGE_DIVIDER(r1, r2);
	double r = r1 / r2 * r3 + r1 + r3;
	double f = RES_VOLTAGE_DIVIDER(r3, r2);
	double e0 = -1.0 * r * f * c, e1 = -1.0 * (r1 + r2) * c;
	double ee0 = exp(st / e0), ee1 = exp(st / e1);
	n->f = (real)f;
	n->exp0 = (real)ee0;
	n->exp1 = (real)ee1;
	n->c_exp0 = (real)(c / e0 * ee0);
	n->c_exp1 = (real)(c / e1 * ee1);
	n->em_ic_0_7 = (real)(ALPHAT * IES * exp(0.7 / KT - 1.0));
}

static inline real em_ic(real x) { return (real)(ALPHAT * IES) * EXP(DIVK(x, (real)KT) - 1); }

static inline real rcint_step(rcint_t *n, real u) {
	real diff, iQ, iQc, iC, RG, vE, vP = n->vp;
	if (u - (real)0.7 < n->vCap * n->gain_r1_r2) {     // discharge
		diff = 0 - n->vCap;
		iC = n->c_exp1 * diff;
		diff -= diff * n->exp1;
		n->vCap += diff;
		iQ = 0;
		vE = n->vCap * n->gain_r1_r2;
		RG = vE / iC;
	} else {                                            // charge
		diff = (vP - n->vCE) * n->f - n->vCap;
		iC = 0 - n->c_exp0 * diff;
		diff -= diff * n->exp0;
		n->vCap += diff;
#ifdef DKSND_FLOAT
		// ((RG - r3) / RG * (vP - vCE) is (vP - vCE) - r3 * iQ: one divide)
		iQ = iC + (iC * n->r1 + n->vCap) * n->inv_r2;
		RG = (vP - n->vCE) / iQ;
		vE = (vP - n->vCE) - n->r3 * iQ;
#else
		iQ = iC + (iC * n->r1 + n->vCap) / n->r2;
		RG = (vP - n->vCE) / iQ;
		vE = (RG - n->r3) / RG * (vP - n->vCE);
#endif
	}
	if (u > (real)0.7 + vE) {
		vE = u - (real)0.7;
		iQc = n->em_ic_0_7;
	} else
		iQc = em_ic(u - vE);
	n->vCE = smin(vP - (real)0.1, vP - RG * iQc);
	n->vCE = smax(n->vCE, (real)0.1);
	n->vCE = (real)0.1 * n->vCE + (real)0.9 * (vP - vE - iQ * n->r3);
	switch (n->type) {
	case 0: return n->vCap;                             // DISC_RC_INTEGRATE_TYPE1
	case 1: return vE;
	default: return smax(0, vP - iQ * n->r3);
	}
}

// ---- DSS_INVERTER_OSC (a CD4049, DEFAULT_CD40XX_VALUES(5)), types 1 and 2
#define INV_TAB 500
typedef struct { real w, wc, rp, r1, v_cap, v_g2_old; int type; } invosc_t;
// the gates' transfer function, the same for both (made once: 500 exps and
// pows in doubles, which the M33 does in software, some 80 ms of it)
static real inv_tab[INV_TAB];
static bool inv_tab_made;
static const double inv_vB = 5, inv_vOutLow = 5 * 0.02, inv_vOutHigh = 5 * 0.98, inv_vInFall = 5 / 5.0 * 1.5,
                    inv_vInRise = 5 / 5.0 * 3.5, inv_clamp = 0.1;

static void invosc_reset(invosc_t *n, double rc, double rp, double c, int type) {
	n->type = type;
	n->w = (real)exp(-st / (rc * c));
	n->wc = (real)exp(-st / ((rc * rp) / (rp + rc) * c));
	n->v_cap = n->v_g2_old = 0;
	n->rp = (real)rp;
	n->r1 = (real)rc;
	if (inv_tab_made) return;
	double b = (log(0.0 - log(inv_vOutLow / inv_vB)) - log(0.0 - log((inv_vOutHigh / inv_vB)))) / log(inv_vInRise / inv_vInFall);
	double a = log(0.0 - log(inv_vOutLow / inv_vB)) - b * log(inv_vInRise / inv_vB);
	a = exp(a);
	for (int i = 0; i < INV_TAB; i++) {
		double x = (double)i / (double)(INV_TAB - 1) * inv_vB;
		x = x / inv_vB;
		inv_tab[i] = (real)(x > 0 ? inv_vB * exp(-a * pow(x, b)) : inv_vB);
	}
	inv_tab_made = true;
}

static inline real inv_tf(real x) {
	if (x < 0) return (real)inv_vB;
	if (x <= (real)inv_vB) return inv_tab[(int)DIVK((real)(INV_TAB - 1) * x, (real)inv_vB)];
	return inv_tab[INV_TAB - 1];
}

static inline real invosc_step(invosc_t *n) {
	real diff, vG2, vG3, vI = n->v_cap + n->v_g2_old;
	if (n->type == 1) {
		real vG1 = inv_tf(vI);
		vG2 = inv_tf(vG1);
		vG3 = inv_tf(vG2);
	} else {
		vG3 = inv_tf(vI);
		vG2 = inv_tf(vG3);
	}
	int clamped = 0;
	if (vI < (real)-inv_clamp) { vI = (real)-inv_clamp; clamped = 1; }
	else if (vI > (real)(inv_vB + inv_clamp)) { vI = (real)(inv_vB + inv_clamp); clamped = 1; }
	if (clamped) {
		real ratio = n->rp / (n->rp + n->r1);
		diff = vG3 * ratio - (n->v_cap + vG2) + vI * (1 - ratio);
		diff = diff - diff * n->wc;
	} else {
		diff = vG3 - (n->v_cap + vG2);
		diff = diff - diff * n->w;
	}
	n->v_cap += diff;
	n->v_g2_old = vG2;
	return vG3;
}

// ---- dkong_custom_mixer: the 555's control voltage
typedef struct { real i_in1[2], r_total[2], exp[2], out_v, v, r3, i_v, inv_r3; } custom_t;

static void custom_reset(custom_t *n, double r1, double r2, double r3, double r4, double c, double v) {
	double ne555_cv_r = RES_2_PARALLEL(RES_K(5), RES_K(10));
	double r_in0 = RES_2_PARALLEL(r1 + r2, r3), r_in1 = RES_2_PARALLEL(r2, r3);
	double rt0 = RES_2_PARALLEL(r_in0 + r4, ne555_cv_r), rt1 = RES_2_PARALLEL(r_in1 + r4, ne555_cv_r);
	n->i_in1[0] = (real)(v / (r1 + r2));
	n->i_in1[1] = 0;
	n->r_total[0] = (real)rt0;
	n->r_total[1] = (real)rt1;
	n->exp[0] = (real)rc_charge_exp(rt0 * c);
	n->exp[1] = (real)rc_charge_exp(rt1 * c);
	n->out_v = 0;
	n->v = (real)v;
	n->r3 = (real)r3;
	n->i_v = n->v / (real)RES_K(5);     // (a constant: the same as each step's divide)
	n->inv_r3 = (real)(1.0 / r3);
}

static inline real custom_step(custom_t *n, real in1, real in2) {
	int i = (int)in1;
	real i_total = n->i_v;
	i_total += n->i_in1[i];
#ifdef DKSND_FLOAT
	i_total += in2 * n->inv_r3;
#else
	i_total += in2 / n->r3;
#endif
	n->out_v += (i_total * n->r_total[i] - n->out_v) * n->exp[i];
	return n->out_v;
}

// ---- DSD_555_ASTBL with a control voltage node, dkong_555_vco_desc
// (DISC_555_OUT_ENERGY, 5 V, the charge from 5 V, high 4.5 V)
typedef struct { double t_rc_charge, t_rc_discharge; real exp_charge, exp_discharge, cap_voltage, out; int flip_flop; } astbl_t;

static void astbl_reset(astbl_t *n, double r1, double r2, double c) {
	n->t_rc_charge = (r1 + r2) * c;
	n->t_rc_discharge = r2 * c;
	n->exp_charge = (real)rc_charge_exp(n->t_rc_charge);
	n->exp_discharge = (real)rc_charge_exp(n->t_rc_discharge);
	n->flip_flop = 1;
	n->cap_voltage = 0;
	n->out = 0;
	// (MAME's reset steps it once, with the control voltage still 0: it does nothing)
}

static inline real astbl_step(astbl_t *n, real ctrlv) {
	const real v_charge = (real)DK_SUP_V, v_out_high = (real)(DK_SUP_V - 0.5);
	int flip_flop = n->flip_flop;
	real dt, x_time = 0, v_cap = n->cap_voltage, v_cap_next = 0, exponent;
	bool update_exponent = false;
	if (ctrlv < (real)0.25) return n->out;
	real threshold = ctrlv, trigger = ctrlv / 2;
	if (v_cap >= threshold) flip_flop = 0;
	else if (v_cap <= trigger) flip_flop = 1;
	dt = st_r;
	do {
		if (flip_flop) {
			exponent = update_exponent ? 1 - EXP(-dt / (real)n->t_rc_charge) : n->exp_charge;
			v_cap_next = v_cap + ((v_charge - v_cap) * exponent);
			dt = 0;
			if (v_cap_next >= threshold) {
				dt = (real)n->t_rc_charge * LOG(1 / (1 - ((v_cap_next - threshold) / (v_charge - v_cap))));
				x_time = dt;
				v_cap_next = threshold;
				flip_flop = 0;
				update_exponent = true;
			}
		} else {
			exponent = update_exponent ? 1 - EXP(-dt / (real)n->t_rc_discharge) : n->exp_discharge;
			v_cap_next = v_cap - (v_cap * exponent);
			dt = 0;
			if (v_cap_next <= trigger) {
				if (v_cap_next < trigger)
					dt = (real)n->t_rc_discharge * LOG(1 / (1 - ((trigger - v_cap_next) / v_cap)));
				x_time = dt;
				v_cap_next = trigger;
				flip_flop = 1;
				update_exponent = true;
			}
		}
		v_cap = v_cap_next;
	} while (dt != 0);
	n->cap_voltage = v_cap;
#ifdef DKSND_FLOAT
	x_time = x_time * inv_st_r;
#else
	x_time = x_time / st_r;
#endif
	if (x_time == 0) x_time = 1;
	n->out = v_out_high * (flip_flop ? x_time : (1 - x_time));
	n->flip_flop = flip_flop;
	return n->out;
}

// ---- DST_CRFILTER
typedef struct { real exponent, vCap; } crf_t;

static void crf_reset(crf_t *n, double r, double c) {
	n->exponent = (real)rc_charge_exp(r * c);
	n->vCap = 0;
}

static inline real crf_step(crf_t *n, real in) {
	real v_out = in - n->vCap;
	n->vCap += v_out * n->exponent;
	return v_out;
}

// ---- DST_RCDISC: the DAC's decay (Q7, R20, C32)
// (the cart, DKSND_FLOAT: exp(t / -RC) kept as a product, a step's factor
// each step, instead of an exp each step)
typedef struct { int state; real t, exponent0, out, f, step_f; } rcdisc_t;

static void rcdisc_reset(rcdisc_t *n, double r, double c) {
	n->out = 0;
	n->state = 0;
	n->t = 0;
	n->exponent0 = (real)(-1.0 * r * c);
	n->f = 1;
	n->step_f = (real)exp(st / (-1.0 * r * c));
}

static inline real rcdisc_step(rcdisc_t *n, real enable, real in) {
	if (n->state == 0) {
		if (enable != 0) { n->state = 1; n->t = 0; n->f = 1; }
		n->out = 0;
	} else {
		if (enable != 0) {
#ifdef DKSND_FLOAT
			n->out = in * n->f;
			n->f *= n->step_f;
#else
			n->out = in * EXP(n->t / n->exponent0);
			n->t += (real)st;
#endif
		} else
			n->state = 0;
	}
	return n->out;
}

// ---- DST_SALLEN_KEY low pass (calculate_filter2_coefficients, pre-warped)
typedef struct { real a1, a2, b0, b1, b2, x1, x2, y1, y2; } sk_t;

static void sk_reset(sk_t *n, double r1, double r2, double c1, double c2) {
	double freq = 1.0 / (2.0 * M_PI * sqrt(c1 * c2 * r1 * r2));
	double q = sqrt(c1 * c2 * r1 * r2) / (c2 * (r1 + r2));
	double d = 1.0 / q, rate = 1.0 / st;
	double two_over_T = 2 * rate, two_over_T_squared = two_over_T * two_over_T;
	double wc = rate * 2.0 * tan(M_PI * freq / rate);
	double wc_squared = wc * wc;
	double den = two_over_T_squared + d * wc * two_over_T + wc_squared;
	memset(n, 0, sizeof *n);
	n->a1 = (real)(2.0 * (-two_over_T_squared + wc_squared) / den);
	n->a2 = (real)((two_over_T_squared - d * wc * two_over_T + wc_squared) / den);
	n->b0 = n->b2 = (real)(wc_squared / den);
	n->b1 = (real)(2.0 * (wc_squared / den));
}

static inline real sk_step(sk_t *n, real in) {
	real v_out = -n->a1 * n->y1 - n->a2 * n->y2 + n->b0 * in + n->b1 * n->x1 + n->b2 * n->x2;
	n->x2 = n->x1;
	n->x1 = in;
	n->y2 = n->y1;
	n->y1 = v_out;
	return v_out;
}

// ---- DST_MIXER of resistors (dkong_mixer_desc: four of 47k, rF 10k, cF 100 nF, cAmp 1 uF)
typedef struct { real r_total, exp_c_f, exp_c_amp, v_cap_f, v_cap_amp; } mixer_t;

static void mixer_reset(mixer_t *n) {
	double g = 0;
	for (int i = 0; i < 4; i++) g += 1.0 / RES_K(47);
	g += 1.0 / RES_K(10);
	n->r_total = (real)(1.0 / g);
	n->exp_c_f = (real)rc_charge_exp((1.0 / g) * CAP_N(100));
	n->exp_c_amp = (real)rc_charge_exp(RES_K(100) * CAP_U(1));
	n->v_cap_f = n->v_cap_amp = 0;
}

static inline real mixer_step(mixer_t *n, real a, real b, real c, real d) {
	const real r = (real)RES_K(47);
	real i = 0;
	i += DIVK(a, r);
	i += DIVK(b, r);
	i += DIVK(c, r);
	i += DIVK(d, r);
	real v = i * n->r_total;
	n->v_cap_f += (v - 0 - n->v_cap_f) * n->exp_c_f;
	v = n->v_cap_f;
	n->v_cap_amp += (v - n->v_cap_amp) * n->exp_c_amp;
	v -= n->v_cap_amp;
	return v;
}

// ---------------------------------------------------------------------------
// the network

static struct {
	// the inputs
	uint8_t dac;                        // DS_DAC (DISCRETE_INPUT_BUFFER)
	real sound0_inv, sound1_inv, sound2_inv, discharge_inv;     // DISCRETE_INPUT_NOT: 1 while the bit is 0
	// stomp
	lfsr_t n11; counter_t n12; rcdmod_t n15; rcd2_t n17; rcint_t n22;
	// jump
	invosc_t n25; custom_t n28; astbl_t n29; rcdmod_t n33; rcd2_t n35; rcint_t n39;
	// walk
	invosc_t n51; custom_t n54; astbl_t n55; rcdmod_t n60; crf_t n61;
	// the DAC
	rcdisc_t n70; sk_t n73;
	// the amplifier
	mixer_t n288; rcint_t n294; crf_t n295, n296;
} N;

static void net_reset(void) {
	N.dac = 0;
	N.sound0_inv = N.sound1_inv = N.sound2_inv = N.discharge_inv = 1;
	lfsr_reset(&N.n11);
	memset(&N.n12, 0, sizeof N.n12);
	rcdmod_reset(&N.n15, DK_R10, 0, 0, DK_R9, DK_C21, DK_SUP_V);
	rcd2_reset(&N.n17, DK_R8 + DK_R7, DK_R7, DK_C20);
	rcint_reset(&N.n22, DK_R5, RES_2_PARALLEL(DK_R4 + DK_R3, DK_R6), 0, DK_C19, DK_SUP_V, 0);
	invosc_reset(&N.n25, DK_R38, DK_R39, DK_C26, 1);
	custom_reset(&N.n28, DK_R32, DK_R50, DK_R51, DK_R49, DK_C24, DK_SUP_V);
	astbl_reset(&N.n29, RES_K(47), RES_K(27), CAP_N(47));
	rcdmod_reset(&N.n33, DK_R32, 0, 0, DK_R31, DK_C18, DK_SUP_V);
	rcd2_reset(&N.n35, DK_R30 + DK_R29, DK_R29, DK_C17);
	rcint_reset(&N.n39, DK_R27, RES_2_PARALLEL(DK_R28, DK_R26 + DK_R25), 0, DK_C16, DK_SUP_V, 0);
	invosc_reset(&N.n51, DK_R47, DK_R48, DK_C30, 2);
	custom_reset(&N.n54, DK_R36, DK_R45, DK_R46, DK_R44, DK_C29, DK_SUP_V);
	astbl_reset(&N.n55, RES_K(47), RES_K(27), CAP_N(33));
	rcdmod_reset(&N.n60, DK_R36, DK_R18, DK_R35, DK_R17, DK_C25, DK_SUP_V);
	crf_reset(&N.n61, DK_R15 + DK_R16, DK_C23);
	rcdisc_reset(&N.n70, DK_R20, DK_C32);
	sk_reset(&N.n73, RES_K(5.6), RES_K(5.6), CAP_N(22), CAP_N(10));
	mixer_reset(&N.n288);
	rcint_reset(&N.n294, 0, 150, 1000, CAP_U(33), DK_SUP_V, 2);
	crf_reset(&N.n295, RES_K(50), DK_C13);
	crf_reset(&N.n296, RES_K(1), CAP_U(4.7));
}

// one step of every node, in the network's order: NODE_296's output
static DKSND_HOT real net_step(void) {
	// stomp
	real n11 = lfsr_step(&N.n11);
	real n12 = counter_step(&N.n12, n11);
	real n13 = (n12 > 3) * (real)DK_SUP_V;
	real n15 = rcdmod_step(&N.n15, N.sound2_inv, 0);
	real n16 = n15 > (real)0.6;
	real n17 = rcd2_step(&N.n17, n16, (real)DK_SUP_V, (real)0.0);
	real n20 = diode_mix2(n17, n13);
	real n22 = rcint_step(&N.n22, n20);
	real out_stomp = n22 * (real)(DK_R3 / (DK_R3 + DK_R4));
	// jump
	real n25 = invosc_step(&N.n25);
	real n28 = custom_step(&N.n28, N.sound1_inv, n25);
	real n29 = astbl_step(&N.n29, n28);
	real n33 = rcdmod_step(&N.n33, N.sound1_inv, 0);
	real n34 = n33 > (real)0.6;
	real n35 = rcd2_step(&N.n35, n34, (real)DK_SUP_V, (real)0.0);
	real n38 = diode_mix2(n35, n29);
	real n39 = rcint_step(&N.n39, n38);
	real out_jump = n39 * (real)(DK_R25 / (DK_R26 + DK_R25));
	// walk
	real n51 = invosc_step(&N.n51);
	real n54 = custom_step(&N.n54, N.sound0_inv, n51);
	real n55 = astbl_step(&N.n55, n54);
	real n60 = rcdmod_step(&N.n60, N.sound0_inv, n55);
	real n61 = crf_step(&N.n61, n60);
	real out_walk = n61 * (real)(DK_R15 / (DK_R15 + DK_R16));
	// the DAC
	real n70 = rcdisc_step(&N.n70, N.discharge_inv, 1);
	real n71 = (real)N.dac * (real)(DK_SUP_V / 256.0) * ((N.discharge_inv == 0) + n70);
	real n73 = sk_step(&N.n73, n71);
	real out_dac = n73 * (real)0.9;       // VR2 at 90 of 100
	// the amplifier
	real n288 = mixer_step(&N.n288, out_stomp, out_jump, out_dac, out_walk);
	real n289 = n288 + (real)(5.0 * 43.0 / (100.0 + 43.0));
	real n294 = rcint_step(&N.n294, n289);
	real n295 = crf_step(&N.n295, n294);
	return crf_step(&N.n296, n295);
}

// ---------------------------------------------------------------------------
// the events, and the steps between them

#ifndef DKSND_QUEUE
#define DKSND_QUEUE 8192
#endif
// (in three arrays: 10 bytes an event, not a struct's 16)
static uint64_t q_t[DKSND_QUEUE];
static uint8_t q_node[DKSND_QUEUE], q_value[DKSND_QUEUE];
static uint32_t q_head, q_tail, dropped;
static uint64_t hw;                     // the latest change or update (MAME's stream is made to there)
static uint64_t bound_q16;              // the next step's start, ticks << 16
static int over;

void dk_snd_event(uint64_t when, int node, uint8_t value) {
	if (when < hw) when = hw;
	hw = when;
	if (node == DK_SNDSYNC) return;
	if (q_tail - q_head >= DKSND_QUEUE) { dropped++; return; }
	uint32_t i = q_tail++ % DKSND_QUEUE;
	q_t[i] = when;
	q_node[i] = (uint8_t)node;
	q_value[i] = value;
}

static void apply(int node, uint8_t value) {
	switch (node) {
	case DK_DAC: N.dac = value; break;
	case DK_DISCHARGE: N.discharge_inv = value ? 0 : 1; break;
	case DK_WALK: N.sound0_inv = value ? 0 : 1; break;
	case DK_JUMP: N.sound1_inv = value ? 0 : 1; break;
	case DK_STOMP: N.sound2_inv = value ? 0 : 1; break;
	case DK_SNDRESET: net_reset(); break;
	}
}

uint32_t dksnd_dropped(void) { return dropped; }

void dksnd_init(uint32_t rate_q8, int over_) {
	int o = over_ < 1 ? 1 : over_;
	dksnd_init_step(((uint64_t)DKSND_TICK_HZ << 24) / ((uint64_t)rate_q8 * (uint64_t)o), o);
}

void dksnd_init_step(uint64_t step_q16_, int over_) {
	over = over_ < 1 ? 1 : over_;
	step_q16 = step_q16_;
	st = (double)step_q16 / 65536.0 / (double)DKSND_TICK_HZ;
	st_r = (real)st;
	inv_st_r = (real)(1.0 / st);
	q_head = q_tail = dropped = 0;
	hw = 0;
	bound_q16 = 0;
	net_reset();
}

DKSND_HOT int dksnd_take(int16_t *out, int max) {
	uint64_t limit_q16 = dk_snd_tick() << 16;
	// (MAME's -1..1: the output node's gain, 32767 / 5 * 3.41 a volt, over
	// 32768, and the speaker route's 0.9)
	const real scale = (real)(32767.0 / 5.0 * 3.41 / 32768.0 * 0.9 * 32767.0) / (real)over;
	int n = 0;
	while (n < max && bound_q16 + step_q16 * (uint64_t)(over - 1) <= limit_q16) {
		real sum = 0;
		for (int k = 0; k < over; k++) {
			// (a change at time 0 counts from the first sample: MAME makes none at time 0)
			while (q_head != q_tail && ((q_t[q_head % DKSND_QUEUE] << 16) < bound_q16 || q_t[q_head % DKSND_QUEUE] == 0)) {
				uint32_t i = q_head++ % DKSND_QUEUE;
				apply(q_node[i], q_value[i]);
			}
			sum += net_step();
			bound_q16 += step_q16;
		}
		real v = sum * scale;
		v = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
		out[n++] = (int16_t)(v < 0 ? v - (real)0.5 : v + (real)0.5);
	}
	return n;
}
