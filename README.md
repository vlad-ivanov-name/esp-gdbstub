
esp-gdbstub
=======

Intro
-----

This is a fork of <a href="/Espressif/esp-gdbstub">Espressif/esp-gdbstub</a> intended for use with <a href="/SuperHouse/esp-open-rtos">esp-open-rtos</a>. As of now, you need to apply a patch to esp-open-rtos in order to use gdbstub.

Usage
-----

1. Build requires premake5 and xtensa-lx106-elf toolchain. Make sure both are in your PATH.
2. Run `ESP_OPEN_RTOS=~/esp8266/esp-open-rtos premake5 gmake` (`ESP_OPEN_RTOS` variable should contain actual esp-open-rtos location).
3. Run `make`
4. Add `esp-gdbstub/include` to the list of include paths of your project
5. Add `lib/libesp-gdbstub.a` to the list of linked libraries.
6. Call `gdbstub_init()` after configuring UART speed.

Eclipse CDT setup
-----------------

To use Eclipse CDT for debugging, open Eclipse, create a new project and create a debug configuration for C/C++ Remote Application.

You need to set the following options:

1. Main → Application: Firmware ELF
2. Debugger: Uncheck “Stop on startup at: main”
3. Debugger → Main: select GDB binary and .gdbinit file provided with esp-gdbstub
4. Debugger → Connection → Type: Serial
5. Debugger → Connection → Speed: 115200

Note that upon launching the debug session gdb will send “continue” command if the target is paused at `gdbstub_do_break`. If you want to stop right after debug session launch, place `gdbstub_do_break` macro twice in your code.

Notes
-----
 * Using software breakpoints ('br') only works on code that's in RAM. Code in flash can only have a hardware breakpoint ('hbr'). If you know where you want to break before downloading the program to the target, you can use gdbstub_do_break() macro as much as you want.
 * Due to hardware limitations, only one hardware breakpount and one hardware watchpoint are available.
