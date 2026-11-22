// dkcard.h -- the ROMs on the card, through fsro: loose or zipped in
// /DKONG (MAME's dkong.zip, as it is), or /DKONG.ZIP. Portable: the
// plugin and the Mac test (test/card.sh, on disk images) share it.
// (Pac-Man's pmcard.c, from Q*bert's qbcard.c.)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../sdplayer/fsro.h"
#include "dkroms.h"

#define DKCARD_HOME "/DKONG"
#define DKCARD_ZIP  "/DKONG.ZIP"

// Every file in DKCARD_HOME, then DKCARD_ZIP, until all are found: each
// ROM to its place in dk_rom_store. A zip is never read whole: its
// directory, then each wanted entry's packed bytes into buf and unpacked
// after them. buf: max bytes (the directory, and a packed ROM with the ROM
// after it, must fit); tick: called between sectors (the plugin serves the
// console), or NULL. Returns dkroms' found bits.
uint32_t dkcard_load_roms(fs_t *fs, uint8_t *buf, uint32_t max, void (*tick)(void));
