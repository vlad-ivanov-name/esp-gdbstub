newoption {
	trigger = "with-threads",
	description = "Enable RTOS task debugging"
}

newoption {
	trigger = "with-eor",
	description = "Specify esp-open-rtos path"
}

local gcc_prefix = "xtensa-lx106-elf"
local esp_open_rtos = _OPTIONS["with-eor"]

if not esp_open_rtos then
   error("Please provide esp-open-rtos path with --with-eor=/path argument")
end

workspace "esp-gdbstub"
	kind "StaticLib"
	language "C"
	configurations { "default" }
	gccprefix (gcc_prefix .. "-")
	buildoptions { "-mlongcalls -std=gnu11 -Os -g" }
	postbuildcommands {
		"mkdir -p ./include",
		"cp gdbstub.h include"
	}

project "esp-gdbstub"
	targetdir "lib"
	includedirs { 
		esp_open_rtos .. "/include/espressif/esp8266/",
		esp_open_rtos .. "/lwip/lwip/espressif/include/",
		esp_open_rtos .. "/include",
		esp_open_rtos .. "/core/include",
		esp_open_rtos .. "/FreeRTOS/Source/include",
		esp_open_rtos .. "/FreeRTOS/Source/portable/esp8266"
	}
	files {
		"gdbstub.c",
		"gdbstub-entry.S"
	}
	configuration "with-threads"
		defines { "GDBSTUB_THREAD_AWARE=1" }
		files {
			"gdbstub-freertos.c"
		}
	configuration "not with-threads"
		defines { "GDBSTUB_THREAD_AWARE=0" }

