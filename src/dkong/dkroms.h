// dkroms.h -- where the ROMs go: MAME's "dkong" set, each file known by its
// size and CRC-32, never by its name. Portable: the plugin and the Mac tests
// share it. (Pac-Man's pmroms.h.)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "dk.h"

typedef struct {
    const char *name;                   // MAME's name, for messages
    uint32_t size, crc;
    uint8_t *place;                     // where its bytes go
} dk_rom_file_t;

#define DK_ROM_FILES 15
#define DKROMS_ALL   ((1u << DK_ROM_FILES) - 1)
extern const dk_rom_file_t dk_rom_files[DK_ROM_FILES];

typedef struct {
    uint8_t maincpu[0x4000], sound[0x800], tune[0x800];
    uint8_t gfx1[0x1000] __attribute__((aligned(4))), gfx2[0x2000];   // (the plugin's room for dk_roms_t.rows)
    uint8_t proms[0x300];
} dk_rom_store_t;
extern dk_rom_store_t dk_rom_store;

uint32_t dk_crc32(uint32_t crc, const uint8_t *p, uint32_t n);
// which of the files still to find (not in found) has this size and CRC-32 (-1: none)
int dkroms_wanted(uint32_t size, uint32_t crc, uint32_t found);
bool dkroms_size_ok(uint32_t size);     // one of the files' sizes
// a file's bytes (size of them, its CRC-32 right) to its place
void dkroms_put(int i, const uint8_t *data);
// the board's ROMs from the store, the graphics to be decoded where they
// are (dk_roms_t.rows): first copied to gfx_copy (DK_ROWS_BYTES), which
// dk_init reads them from and then leaves alone
void dkroms_roms(dk_roms_t *r, uint8_t *gfx_copy);
