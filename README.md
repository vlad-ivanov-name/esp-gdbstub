
esp-gdbstub
=======

Intro
-----

ESP8266 debugging tool intended to be used with and <a href="https://github.com/SuperHouse/esp-open-rtos">esp-open-rtos</a>. Based on <a href="https://github.com/Espressif/esp-gdbstub">Espressif/esp-gdbstub</a>.

<img width="50%" src="https://cloud.githubusercontent.com/assets/2057191/19023093/df5226c2-88f6-11e6-89d0-cd47b09781cc.png" />

Usage
-----

1. Build requires premake5 and xtensa-lx106-elf toolchain. Make sure both are in your PATH.
2. Configure with `premake5 gmake`. The following options are supported:
    * `--with-eor=/path/to/esp-open-rtos`: this option is required. It should point to an actual esp-open-rtos location.
	* `--with-threads`: enable RTOS task debugging. As for now, there is very basic support of threads, you will be able 
	to see multiple tasks when a breakpoint is hit, but GDB will most likely crash shorlty after. This option also has 
	a performance impact because a lot of data will be transferred through serial connection — consider increasing 
	the baudrate.
	
	When thread support is enabled, you need to add the following definitions to CFLAGS of your project before compiling 
	FreeRTOS libs:
	
	`CFLAGS+=-DportREMOVE_STATIC_QUALIFIER -DINCLUDE_pcTaskGetTaskName=1`
	
	Run `make clean` after switching the state of this flag.
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
