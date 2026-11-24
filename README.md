# Donkey Kong

This plugin plays Nintendo's 1981 Donkey Kong arcade game on the ColecoVision. It needs the game's ROMs from MAME, which you supply.

## Setup

1. Copy `DKONG.CVP` into the `/PLUGINS` folder on the card.
2. Make a folder named `/DKONG` and put MAME's `dkong.zip` in it as it is, or the files from it loose. You can also put the zip in the card's top folder as `/DKONG.ZIP`.
3. Start Donkey Kong from the Plugins page in the menu.

The files can have any names, because the plugin knows each ROM by its size and contents. If a ROM is missing, the screen names it.

## Controls

On the ColecoVision controller:

- The joystick moves Mario.
- Either fire button jumps.
- Keypad * or # adds a coin.
- Keypad 1 starts a one-player game, and keypad 2 a two-player game.
- Press RESET on the console to go back to the menu, or hold both fire buttons and \* for 2 seconds (Select and Start on a USB pad).

On a USB pad, through a powered hub:

- The D-pad or the left stick moves Mario.
- Any face button jumps.
- Minus adds a coin, and Plus starts a one-player game.

The arcade stick has four ways. A diagonal moves Mario the way you just pressed, as in MAME.

## Picture and sound

The arcade screen stands on its side and is 224 by 256 pixels. The ColecoVision shows 256 by 192, so the plugin scales it to three quarters of its size, 168 by 192. The scores, the lives, the level and the bonus move to the left and right of the playfield.

Mario, the barrels and the fireballs are the ColecoVision's sprites while there is room on their lines, so they don't flicker. Donkey Kong, Pauline and anything that doesn't fit are drawn into the picture, where two colors share each 8 pixels.

Colors are the nearest of the ColecoVision's 15.

The sound board's music and its walk, jump and stomp effects are worked out on the cart as MAME works them out, and play through the ColecoVision's sound chip as 5,962 samples a second.

High scores last until you go back to the menu, as on the arcade machine when it is switched off.

## Credits

The arcade emulation is a port of MAME 0.288's code, under the BSD-3-Clause license. Its authors are:

- Couriersud, for the Donkey Kong driver, its sound board and the latches.
- K.Wilkins, Couriersud and Derrick Renaud, for the discrete sound circuits.
- Curt Coder and Carl, for the 8257 DMA controller.
- Dan Boris, Mirko Buffoni, Aaron Giles and Couriersud, for the 8035 sound CPU.
- Juergen Buchmueller and Andrei I. Holub, for the Z80 CPU.
- Aaron Giles, for MAME's scheduler and the watchdog.

You can find MAME at https://www.mamedev.org. The zip reader follows Mark Adler's puff, from zlib. USB support comes from TinyUSB by Ha Thach, under the MIT license.

The plugin itself is by Parrot Cart, and it's free. Its source is in this repository, https://github.com/plasticbugs/parrot-plugin-dkong.

Donkey Kong is a trademark of Nintendo. Parrot Cart isn't connected with them. This plugin doesn't include any game ROMs.

## Build it yourself

You need the Raspberry Pi Pico SDK 2.1.1, an Arm GNU toolchain, CMake,
Ninja, Python 3, sjasmplus and picotool. Releases are built with the Arm
GNU Toolchain 9-2019-q4 (GCC 9.2.1) and picotool 2.3.1. With those you get
the same file, byte for byte.

```sh
cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
ninja -C build
```

This makes `build/DKONG.CVP`. Copy it into the `/PLUGINS` folder on the card.

On Ubuntu, `tools/ci/setup.sh` installs the same tools that GitHub uses to
build each release.

`CART-README.md` is the text the cart shows on the plugin's page. This
README starts with the same text.

## License

The plugin is under the BSD 3-Clause license (`LICENSE`). `THIRD-PARTY.md`
lists the parts that came from elsewhere, and their licenses.
