
# esp-gdbstub

## Intro

ESP8266 debugging tool intended to be used with <a href="https://github.com/SuperHouse/esp-open-rtos">esp-open-rtos</a>. Based on <a href="https://github.com/Espressif/esp-gdbstub">Espressif/esp-gdbstub</a>.

<img width="50%" src="https://cloud.githubusercontent.com/assets/2057191/19023093/df5226c2-88f6-11e6-89d0-cd47b09781cc.png" />

## Usage

1. Build requires premake5 and xtensa-lx106-elf toolchain. Make sure both are in your PATH.
1. Configure with `premake5 gmake`. The following options are supported:
    * `--with-eor=/path/to/esp-open-rtos`: this option is required. It should point to an actual esp-open-rtos location.
	* `--with-threads`: enable RTOS task debugging. As for now, there is very basic support of threads, you will be able 
	to see multiple tasks when a breakpoint is hit, but GDB will most likely crash shorlty after. This option also has 
	a performance impact because a lot of data will be transferred through serial connection — consider increasing 
	the baudrate.
	
	When thread support is enabled, you need to add the following definitions to CFLAGS of your project before compiling 
	FreeRTOS libs:
	
	`CFLAGS+=-DportREMOVE_STATIC_QUALIFIER -DINCLUDE_pcTaskGetTaskName=1`
	
	Run `make clean` after switching the state of this flag.
1. Run `make`
1. Add library to your project:
	```Makefile
	PROGRAM=blink
	
	# Order is important!
	
	EXTRA_CFLAGS+=-I../../../esp-gdbstub/include
	EXTRA_LDFLAGS+=-L../../../esp-gdbstub/lib
	
	include ../../common.mk
	
	LIBS+=esp-gdbstub
	PROGRAM_CFLAGS+=-O0
	```
1. Call `gdbstub_init()` after configuring UART speed.

### Optimization, stack trace resolving and GDB

It has been observed that GDB may crash when user requests step over source in&nbsp;a&nbsp;file compiled with optimization turned&nbsp;on. I&nbsp;haven't figured out why exactly this happens, but it has something to do with stack frame resolving. Turning optimization off for the whole project would impact performance and code size — that's why only `PROGRAM_CFLAGS` is&nbsp;modified.

## Eclipse CDT setup

To use Eclipse CDT for debugging, open Eclipse, create a new project and create a debug configuration for C/C++ Remote Application.

You need to set the following options:

1. Main → Application: Firmware ELF
2. Debugger: Uncheck “Stop on startup at: main”
3. Debugger → Main: select GDB binary and .gdbinit file provided with esp-gdbstub
4. Debugger → Connection → Type: Serial
5. Debugger → Connection → Speed: 115200

Note that upon launching the debug session gdb will send “continue” command if the target is paused at `gdbstub_do_break`. If you want to stop right after debug session launch, place `gdbstub_do_break` macro twice in your code.

## Notes

 * Using software breakpoints ('br') only works on code that's in RAM. Code in flash can only have a hardware breakpoint ('hbr'). If you know where you want to break before downloading the program to the target, you can use gdbstub_do_break() macro as much as you want.
 * Due to hardware limitations, only one hardware breakpount and one hardware watchpoint are available.
