// dkong -- Nintendo's Donkey Kong (1981) on the ColecoVision: the cart
// runs the arcade board (../dk.c, MAME 0.288's emulation: the Z80, the
// 8257 DMA and the sound board's MB8884, with its discrete circuits in
// ../dksnd.c) from the user's ROMs on the card and sends the picture and
// the sound to the console as the Game Boy kernel's lists (../dkcv.c).
// RESET goes back to the menu.
//
//   core 1  serves the kernel and a list a TV frame (cvframes.c)
//   core 0  this loop: a list at a time (../dkrun.c), the board run for the
//           list's time, the changes to the picture of its last VBLANK and
//           the list's samples, handed to core 1 as soon as it will take it
//
// The console runs the game's lists back to back (their sound is a sample
// stream), and each list runs the board for just the list's time (16.77
// ms), so the game keeps the arcade's speed. A list that isn't ready when
// the console wants it makes the console run the last one again (its sound
// once more) and puts the game a list behind. The note counts those.
//
// The ROMs (MAME's "dkong" set) are read from the card's /DKONG folder,
// loose or zipped, or from /DKONG.ZIP, each known by its size and CRC-32.
//
// Controls (../dkpad.h): the joystick or a USB pad's d-pad or left stick,
// as the arcade's 4-way stick in MAME; either fire button or a pad's face
// button jumps; keypad * or #, or -, is a coin; keypad 1 or + one player,
// keypad 2 two players.
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include "cvdiag.h"
#include "cvframes.h"
#include "cvpad.h"
#include "cvsd.h"
#include "vdp.h"
#include "gbkernel.h"
#include "handoff.h"
#include "dkcard.h"
#include "dkcv.h"
#include "dkpad.h"
#include "dkrun.h"

#define OPEN_US     20000           // serve the console this long before the card's (blocking) start
#define WATCHDOG_MS 4000            // a hang reboots into the menu
#define NOTE_US     1000000         // the menu's note is brought up to date this often
#define GAME        "Donkey Kong"       // in the menu's notes
#define TAG         "DK"

static dkcv_t enc;
static dkrun_t run;
static dkpad_t pad;
static dk_roms_t roms;
static uint8_t list[DKCV_LIST_MAX];
static size_t list_len;
static bool list_pending;                   // built, not published yet
static uint32_t seen_frames;
static uint8_t *scratch;                    // dkcv's room before the game: a message's canvas, the card's reads
static size_t scratch_n;

#define CANVAS      (*(canvas_t *)(void *)scratch)

// ---- the console ---------------------------------------------------------------------
static void tasks(void) {
    cvpad_task();
    cvf_task(time_us_32());
    watchdog_update();
}

static void publish(void) {
    if (list_pending && cvf_publish(list, list_len)) list_pending = false;
}

// TV frames the console has started since the last call (0: none yet, or
// the last list isn't out): for the messages' lists, a TV frame each
static uint32_t new_frames(void) {
    tasks();
    publish();
    if (list_pending) return 0;
    uint32_t f = cvf_frames(), n = f - seen_frames;
    seen_frames = f;
    return n;
}

static void send(void) {
    list_len = dkcv_tv_frame(&enc, list);
    list_pending = true;
    publish();
}

// ---- messages ------------------------------------------------------------------------
static void center(int y, const char *s, uint8_t color, int scale) {
    int w = (int)strlen(s) * 8 * scale;
    canvas_text(&CANVAS, w < VDP_W ? (VDP_W - w) / 2 : 0, y, s, color, scale);
}

static void message(const char *head, const char *l1, const char *l2, const char *l3) {
    canvas_init(&CANVAS);
    center(24, "DONKEY", C_LRED, 3);
    center(52, "KONG", C_LRED, 3);
    if (head) center(80, head, C_WHITE, 1);
    if (l1) center(100, l1, C_GRAY, 1);
    if (l2) center(112, l2, C_GRAY, 1);
    if (l3) center(124, l3, C_GRAY, 1);
    center(176, "RESET: BACK TO THE MENU", C_LBLUE, 1);
    dkcv_canvas(&enc, &CANVAS.px[0][0]);
}

// serve the console, a list a frame, for us microseconds (forever: 0)
static void serve_for(uint32_t us) {
    uint32_t t0 = time_us_32();
    while (!us || time_us_32() - t0 < us) {
        if (!new_frames()) continue;
        dkcv_sound(&enc, NULL, 0);
        send();
    }
}

static void __attribute__((noreturn)) fail(const char *why, const char *l2, const char *l3, const char *menu_note) {
    message("IT CAN'T START", why, l2, l3);
    cvf_note(menu_note);
    serve_for(0);
    for (;;) tight_loop_contents();
}

// between a file's sectors: the console keeps its picture
static void card_tick(void) {
    if (!new_frames()) return;
    dkcv_sound(&enc, NULL, 0);
    send();
}

// ---- the game ------------------------------------------------------------------------
static uint32_t published, frames_at_start, us_max, pics_at_note;

// one list: the controls, the board for the list's time, the list made,
// then handed over as soon as core 1 takes it
static void game_list(void) {
    uint32_t t0 = time_us_32();
    uint8_t joy, fire, key;
    cvf_input(&joy, &fire, &key);
    gamepad_t g;
    uint32_t dirs = 0, buttons = 0;
    if (cvpad_get(0, &g)) {
        dirs = cvpad_dirs(&g);
        buttons = g.buttons;
    }
    dkpad_frame(&pad, joy, fire, key, dirs, buttons, &dk_in.in0, &dk_in.in2);
    list_len = dkrun_list(&run, list);
    uint32_t us = time_us_32() - t0;
    if (us > us_max) us_max = us;
    // taken at once: the console already has the last one, so the next is
    // made with none waiting, and draws fewer tiles (dkrun.h)
    run.behind = cvf_publish(list, list_len);
    if (!run.behind)
        while (!cvf_publish(list, list_len)) tasks();
    published++;
    tasks();
}

// the menu's bottom line, should RESET come: pictures a second, lists the
// console ran again for want of a new one (since the start), the slowest
// list this second
static void update_note(void) {
    static uint32_t t_last;
    uint32_t t = time_us_32();
    if (t - t_last < NOTE_US) return;
    int32_t again = (int32_t)(cvf_frames() - frames_at_start) - (int32_t)(published - 1);
    if (again < 0) again = 0;
    unsigned per_s = MIN(run.pictures - pics_at_note, 99u), ms = MIN(us_max / 1000, 99u), tenths = us_max / 100 % 10;
    uint32_t unsel, early, unhdr;
    cvf_bus_counts(&unsel, &early, &unhdr);
    char n[48];
    snprintf(n, sizeof n, TAG " %u/s %ldL %u.%ums r%u s%u h%u", per_s, (long)MIN(again, 9999), ms, tenths,
             (unsigned)MIN(dk_resets, 9u), (unsigned)MIN(unsel, 999u), (unsigned)MIN(unhdr, 999u));
    cvf_note(n);
    t_last = t;
    pics_at_note = run.pictures;
    us_max = 0;
}

int main(void) {
    static const cvf_config_t cfg = {gbkernel_bin, GBKERNEL_BIN_LEN, handoff_bin, HANDOFF_BIN_LEN};
    cvf_start(&cfg);
    cvd_start();
    cvd_stage(1);
    cvpad_init();
    dkcv_init(&enc);
    scratch = dkcv_scratch(&scratch_n);
    message("LOOKING FOR THE ROMS", "IN " DKCARD_HOME " ON THE CARD", NULL, NULL);
    serve_for(OPEN_US);

    if (!cvsd_card_in() || !cvsd_init()) fail(cvsd_card_in() ? "THE CARD DIDN'T ANSWER" : "NO CARD IN THE SLOT", NULL, NULL, GAME ": no card");
    // the card's state and reads in the scratch too (no message is drawn
    // until they are done with: fail() draws over them)
    fs_t *fs = (fs_t *)(void *)scratch;
    size_t fs_room = (sizeof(fs_t) + 7) & ~(size_t)7;
    if (fs_mount(fs, cvsd_read) != FS_OK) fail("NO FAT OR EXFAT ON THE CARD", NULL, NULL, GAME ": the card didn't mount");
    cvd_stage(2);
    watchdog_enable(WATCHDOG_MS, true);
    uint32_t found = dkcard_load_roms(fs, scratch + fs_room, (uint32_t)(scratch_n - fs_room), card_tick);
    cvd_stage(3);
    if (found != DKROMS_ALL) {
        int missing = 0;
        const char *first = "";
        for (int i = 0; i < DK_ROM_FILES; i++)
            if (!(found & 1u << i) && !missing++) first = dk_rom_files[i].name;
        char l2[40], note[48];
        if (!found) snprintf(l2, sizeof l2, "NONE OF ITS %d FILES IS THERE", DK_ROM_FILES);
        else snprintf(l2, sizeof l2, "%d OF %d MISSING: %.14s", missing, DK_ROM_FILES, first);
        for (char *c = l2; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        snprintf(note, sizeof note, GAME ": %d of %d ROMs missing", missing, DK_ROM_FILES);
        fail("PUT MAME'S DKONG ROMS, OR", "DKONG.ZIP, IN " DKCARD_HOME, l2, note);
    }
    dkroms_roms(&roms, scratch);            // (the graphics' copy: the scratch is free until the first picture)
    dkrun_sound_init();
    dk_init(&roms);
    dkcv_prepare();
    dkrun_start(&run, &enc);
    cvd_stage(4);
    cvf_note(GAME);

    while (list_pending) tasks(), publish();          // the last message's list out first
    frames_at_start = cvf_frames();
    for (;;) {
        game_list();
        update_note();
    }
}
