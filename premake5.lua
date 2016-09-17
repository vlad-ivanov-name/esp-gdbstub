local gcc_prefix = "xtensa-lx106-elf"
local esp_open_rtos = os.getenv("ESP_OPEN_RTOS")

workspace "esp-gdbstub"
	kind "StaticLib"
	language "C"
	configurations { "default" }
	gccprefix (gcc_prefix .. "-")
	buildoptions { "-mlongcalls -std=c11 -Os -g" }
	postbuildcommands {
		"mkdir -p ./include",
		"cp gdbstub.h include"
	}

project "esp-gdbstub"
	targetdir "lib"
	includedirs { 
		esp_open_rtos .. "/include/espressif/esp8266/",
		esp_open_rtos .. "/lwip/lwip/espressif/include/",
		esp_open_rtos .. "/core/include"
	}
	files { "*.c", "*.S" }
