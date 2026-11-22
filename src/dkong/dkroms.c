// dkroms.c -- see dkroms.h. The sizes and CRC-32s are MAME 0.288's
// (dkong.cpp's dkong).
#include "dkroms.h"

#include <string.h>

dk_rom_store_t dk_rom_store;

#define S dk_rom_store
const dk_rom_file_t dk_rom_files[DK_ROM_FILES] = {
    {"c_5et_g.bin", 0x1000, 0xba70b88bu, S.maincpu},
    {"c_5ct_g.bin", 0x1000, 0x5ec461ecu, S.maincpu + 0x1000},
    {"c_5bt_g.bin", 0x1000, 0x1c97d324u, S.maincpu + 0x2000},
    {"c_5at_g.bin", 0x1000, 0xb9005ac0u, S.maincpu + 0x3000},
    {"s_3i_b.bin", 0x0800, 0x45a4ed06u, S.sound},
    {"s_3j_b.bin", 0x0800, 0x4743fe92u, S.tune},
    {"v_5h_b.bin", 0x0800, 0x12c8c95du, S.gfx1},
    {"v_3pt.bin", 0x0800, 0x15e9c5e9u, S.gfx1 + 0x800},
    {"l_4m_b.bin", 0x0800, 0x59f8054du, S.gfx2},
    {"l_4n_b.bin", 0x0800, 0x672e4714u, S.gfx2 + 0x800},
    {"l_4r_b.bin", 0x0800, 0xfeaa59eeu, S.gfx2 + 0x1000},
    {"l_4s_b.bin", 0x0800, 0x20f2ef7eu, S.gfx2 + 0x1800},
    {"c-2k.bpr", 0x0100, 0xe273ede5u, S.proms},
    {"c-2j.bpr", 0x0100, 0xd6412358u, S.proms + 0x100},
    {"v-5e.bpr", 0x0100, 0xb869b8f5u, S.proms + 0x200},
};
#undef S

uint32_t dk_crc32(uint32_t crc, const uint8_t *p, uint32_t n) {
    static const uint32_t t[16] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
                                   0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
                                   0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ t[crc & 15];
        crc = (crc >> 4) ^ t[crc & 15];
    }
    return ~crc;
}

int dkroms_wanted(uint32_t size, uint32_t crc, uint32_t found) {
    for (int i = 0; i < DK_ROM_FILES; i++)
        if (dk_rom_files[i].size == size && dk_rom_files[i].crc == crc && !(found & 1u << i)) return i;
    return -1;
}

bool dkroms_size_ok(uint32_t size) {
    for (int i = 0; i < DK_ROM_FILES; i++)
        if (dk_rom_files[i].size == size) return true;
    return false;
}

void dkroms_put(int i, const uint8_t *data) {
    memcpy(dk_rom_files[i].place, data, dk_rom_files[i].size);
}

void dkroms_roms(dk_roms_t *r, uint8_t *gfx_copy) {
    r->maincpu = dk_rom_store.maincpu;
    r->sound = dk_rom_store.sound;
    r->tune = dk_rom_store.tune;
    memcpy(gfx_copy, dk_rom_store.gfx1, DK_ROWS_BYTES);     // (gfx1 and gfx2, one after the other)
    r->gfx1 = gfx_copy;
    r->gfx2 = gfx_copy + sizeof dk_rom_store.gfx1;
    r->proms = dk_rom_store.proms;
    r->rows = dk_rom_store.gfx1;
}
