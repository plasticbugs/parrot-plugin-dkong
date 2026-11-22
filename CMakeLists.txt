# dkong (DKONG.CVP): Nintendo's 1981 Donkey Kong arcade game on the
# ColecoVision. The cart runs the arcade board (a Z80, the 8257 DMA and the
# sound board's MB8884 and discrete circuits: src/dkong/dk.c and src/dkong/dksnd.c, from
# MAME 0.288) from the user's ROMs on the card (/DKONG), and dkcv sends the
# picture and the sound to the console as the Game Boy kernel's lists.
# RESET goes back to the menu.
#
#   cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
#   ninja -C build                      -> build/DKONG.CVP
#
# Copy DKONG.CVP into the card's /PLUGINS folder.
cmake_minimum_required(VERSION 3.13)

# the cart's SDK: the copy in sdk/ (sdk/README.md)
set(PARROTCART_SDK ${CMAKE_CURRENT_LIST_DIR}/sdk/firmware/revc)
set(PICO_BOARD_HEADER_DIRS ${PARROTCART_SDK}/cvcart_revc/boards)
set(PICO_BOARD cvcart_revc)
set(PICO_PLATFORM rp2350-arm-s)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# the USB host: sdk/tinyusb (TinyUSB 0.21 and its fixes since), not the pico-sdk's 0.18
include(${PARROTCART_SDK}/sdk/cvpad_tinyusb.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(dkong C CXX ASM)
pico_sdk_init()
include(${PARROTCART_SDK}/sdk/cvplugin.cmake)

# The same source makes the same file, whenever it's built: the compiler's
# __DATE__ and __TIME__ (the build date in the binary info) come from
# SOURCE_DATE_EPOCH, fixed here at the tested build's day, 2026-11-21.
set(BUILD_EPOCH 1795262400)
set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})
set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})

# the plugin's code (the cart's firmware/revc/dkong/, with plugin/ beside it)
set(S ${CMAKE_CURRENT_LIST_DIR}/src/dkong)

set(OURS ${S}/dkong.c ${S}/dk.c ${S}/z80.c ${S}/mcs48.c ${S}/dksnd.c ${S}/dkcv.c ${S}/dkrun.c ${S}/dkcard.c ${S}/dkroms.c
         ${PARROTCART_SDK}/sdk/cvscale.c ${CMAKE_CURRENT_LIST_DIR}/src/starwars/swzip.c)
set_source_files_properties(${OURS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wno-unused-function;-fstack-usage")
# the emulation's state is many small statics: in one section each, every
# use needs a separate address loaded, so not. The picture (dkcv.c) -O2:
# -O3 makes it 19K bigger, more than the room left, for 2% of the time in
# play (test/armcount.sh builds each file as here)
set_source_files_properties(${S}/dk.c ${S}/z80.c ${S}/mcs48.c ${S}/dksnd.c PROPERTIES COMPILE_OPTIONS
                            "-Wall;-Wno-unused-function;-fstack-usage;-fno-data-sections")
set_source_files_properties(${S}/dkcv.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wno-unused-function;-fstack-usage;-O2")
# the card's code runs once, at the start: built for size
set_source_files_properties(${S}/dkcard.c ${S}/dkroms.c ${CMAKE_CURRENT_LIST_DIR}/src/starwars/swzip.c PROPERTIES COMPILE_OPTIONS
                            "-Wall;-Wno-unused-function;-fstack-usage;-Os")
set_source_files_properties(${PARROTCART_SDK}/sdplayer/fsro.c PROPERTIES COMPILE_OPTIONS "-Os")

add_executable(dkong ${OURS} ${PARROTCART_SDK}/midihello/vdp.c)
target_include_directories(dkong PRIVATE ${S} ${PARROTCART_SDK}/midihello
                           ${PARROTCART_SDK}/sdk)
# the sound's circuits in floats (the M33 has a single-precision FPU), a
# queue of their inputs for a few lists (219 at most in cart_host's game); core 0's stack: all of scratch Y
# (4K); core 1's as before (scratch X also holds its bus loop)
target_compile_definitions(dkong PRIVATE PICO_PRINTF_SUPPORT_FLOAT=0 PICO_PRINTF_SUPPORT_EXPONENTIAL=0
    DKSND_FLOAT DKSND_QUEUE=1024 DK_NO_TRACE PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x800)
cvframes_use(dkong)
cvframes_serve2(dkong)      # core 1's bus loop acts on two samples of the pins that agree (sdk/cvfserve.S)
cvsd_use(dkong)
cvpad_use(dkong)                            # USB game pads: the port is a host
pico_enable_stdio_usb(dkong 0)
pico_enable_stdio_uart(dkong 0)
cvcart_plugin(dkong ID parrotcart.dkong NAME "Donkey Kong" VERSION 0.1.2 BY "Parrot Cart"
              HOME /DKONG USES usb-host ICON src/dkong/dkong_icon.txt FILE DKONG.CVP README CART-README.md)
