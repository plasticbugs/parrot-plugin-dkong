# What in this repository came from elsewhere

The plugin's own code, in `src/`, is under the BSD 3-Clause license
(`LICENSE`). The copy of the cart's SDK in `sdk/` is under the MIT license
(`sdk/LICENSE`). These parts came from elsewhere and keep their own terms.

| Where | What | License |
|---|---|---|
| `sdk/firmware/revc/sdk/tinyusb/` | [TinyUSB](https://github.com/hathach/tinyusb) (Ha Thach and contributors), with the cart's fixes, listed in its `VENDORED.md` | MIT (`sdk/firmware/revc/sdk/tinyusb/LICENSE`) |
| `cmake/pico_sdk_import.cmake` | from the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | BSD 3-Clause |

The built plugin also contains the Raspberry Pi Pico SDK's runtime (BSD
3-Clause) and TinyUSB (MIT).

## MAME

Parts of the code in `src/` are ports of MAME's code, or were written with
MAME's code as the reference. That code is under the BSD 3-Clause license
(`LICENSE`). It comes from MAME (<https://github.com/mamedev/mame>, tag
`mame0288`), and its copyright holders are these.

| Here | From MAME | Copyright holders |
|---|---|---|
| `src/dkong/dk.c`: the memory map, inputs, NMI, palette, tiles, sprites, the sound board's glue | `src/mame/nintendo/dkong.cpp`, `dkong_v.cpp`, `dkong_a.cpp` | Couriersud |
| `src/dkong/dk.c`: the DMA controller | `src/devices/machine/i8257.cpp` | Curt Coder, Carl |
| `src/dkong/dk.c`: the sound latches, the palette's resistor networks | `src/devices/machine/latch8.cpp`, `src/emu/video/resnet.cpp` | Couriersud |
| `src/dkong/dk.c`: timers, timeslices, VBLANK, the watchdog | `src/emu/schedule.cpp`, `diexec.cpp`, `screen.cpp`, `src/devices/machine/watchdog.cpp` | Aaron Giles |
| `src/dkong/dk.c`: the streams' updates | `src/emu/sound.cpp` | O. Galibert, Aaron Giles |
| `src/dkong/mcs48.c` | `src/devices/cpu/mcs48/mcs48.cpp` | Dan Boris, Mirko Buffoni, Aaron Giles, Couriersud |
| `src/dkong/dksnd.c`: the circuits | `src/mame/nintendo/dkong_a.cpp`, `src/devices/sound/discrete.cpp`, `disc_*.hxx` | K.Wilkins, Couriersud, Derrick Renaud, Frank Palazzolo |
| `src/dkong/z80.c`, `src/dkong/z80_ops.h`, `src/dkong/tools/z80gen.py` | `src/devices/cpu/z80` (`z80.cpp`, `z80.lst`, `z80make.py`) | Juergen Buchmueller, Andrei I. Holub |
| `src/dkong/dkpad.h`: the 4-way stick | `src/emu/ioport.cpp` | Aaron Giles |

The zip reader in `src/starwars/swzip.c` follows RFC 1951 and the method of
zlib's `contrib/puff` by Mark Adler (zlib license). It was written afresh,
for the cart's Star Wars plugin.

Galagino's Donkey Kong (GPL-3.0) was read while planning the sound. None
of its code or its recorded samples is in this plugin: the walk, jump and
stomp come from MAME's circuits.

Donkey Kong is a trademark of Nintendo. Parrot Cart isn't connected with
them. This plugin doesn't include any game ROMs.

The 8x8 screen font's letter shapes come from the 1982 arcade game Mr. Do!
