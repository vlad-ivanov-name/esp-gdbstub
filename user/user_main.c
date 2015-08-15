/******************************************************************************
 * Copyright 2013-2014 Espressif Systems (Wuxi)
 *
 * FileName: user_main.c
 *
 * Description: entry file of user application
 *
 * Modification history:
 *     2014/1/1, v1.0 create this file.
*******************************************************************************/
#include "ets_sys.h"
#include "osapi.h"

#include "user_interface.h"
#include "../gdbstub/gdbstub.h"


void user_rf_pre_init(void)
{
}

void user_init(void)
{
	uart_div_modify(0, UART_CLK_FREQ/115200);
	os_printf("SDK version:%s\n", system_get_sdk_version());
	gdbstub_init();
	wifi_set_opmode(STATION_MODE);
}