// dkrun.c -- see dkrun.h.
#include <string.h>

#include "dkrun.h"

static dkrun_t *running;

// each VBLANK's board, for this list or (past its end) the next
static void vblank(void) {
    dkrun_t *r = running;
    if (!r) return;
    int k = r->cur ^ (dk_time() > r->target - DKRUN_LEAD);
    dkcv_board(&r->board[k]);
    r->have[k] = true;
}

void dkrun_sound_init(void) { dksnd_init_step(DKCV_STEP_Q16(DKRUN_OVER), DKRUN_OVER); }

void dkrun_start(dkrun_t *r, dkcv_t *cv) {
    memset(r, 0, sizeof *r);
    r->cv = cv;
    running = r;
    dk_on_vblank = vblank;
    r->target = dk_time();
}

size_t dkrun_list(dkrun_t *r, uint8_t *out) {
    r->lists++;
    r->target = ((uint64_t)r->lists * DKCV_LIST_CYCLES_Q16 >> 16) + DKRUN_LEAD;
    if (r->have[r->cur ^ 1]) r->cur ^= 1;       // the last run passed this list's VBLANK
    dk_run_to(r->target);
    if (r->have[r->cur] && r->rest) {          // after a picture that left tiles, none
        r->have[r->cur] = false;
        r->rest = false;
        r->rested++;
    }
    if (r->have[r->cur]) {
        r->cv->tiles_most = r->behind ? DKRUN_TILES_BEHIND : 0;
        r->rest = !dkcv_picture(r->cv, &r->board[r->cur]);
        if (r->rest && r->behind) r->hurried++;
        r->have[r->cur] = false;
        r->shown = &r->board[r->cur];
        r->pictures++;
    }
    int n = dksnd_take(r->pcm, DKCV_PCM_N);
    if (n < DKCV_PCM_N) {
        r->underruns += (uint32_t)(DKCV_PCM_N - n);
        for (int k = n; k < DKCV_PCM_N; k++) r->pcm[k] = n ? r->pcm[n - 1] : 0;
    }
    dkcv_sound(r->cv, r->pcm, DKCV_PCM_N);
    return dkcv_tv_frame(r->cv, out);
}
