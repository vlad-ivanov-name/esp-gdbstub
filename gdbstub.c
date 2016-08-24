/******************************************************************************
 * Copyright 2015 Espressif Systems
 *
 * Description: A stub to make the ESP8266 debuggable by GDB over the serial 
 * port.
 *
 * License: ESPRESSIF MIT License
 *******************************************************************************/

#include "gdbstub.h"
#include "ets_sys.h"
#include "eagle_soc.h"
//#include "gpio.h"

#include <xtensa/config/specreg.h>
#include <xtensa/config/core-isa.h>
#include <xtensa/corebits.h>

#include "gdbstub.h"
#include "gdbstub-entry.h"
#include "gdbstub-cfg.h"

#include <stdlib.h>
#include <stdint.h>

#define BIT(X) (1<<(X))

//From xtruntime-frames.h
struct xtensa_exception_frame_t {
	uint32_t pc;
	uint32_t ps;
	uint32_t sar;
	uint32_t vpri;
	uint32_t a0;
	uint32_t a[14]; //a2..a15
//These are added manually by the exception code; the HAL doesn't set these on an exception.
	uint32_t litbase;
	uint32_t sr176;
	uint32_t sr208;
	uint32_t a1;
	 //'reason' is abused for both the debug and the exception vector: if bit 7 is set,
	//this contains an exception reason, otherwise it contains a debug vector bitmap.
	uint32_t reason;
};

struct xtensa_rtos_int_frame_t {
	uint32_t exitPtr;
	uint32_t pc;
	uint32_t ps;
	uint32_t a[16];
	uint32_t sar;
};

#include <string.h>
#include <stdio.h>

void _xt_isr_attach(int inum, void *fn);
void sdk__xt_isr_unmask(int inum);
void sdk_os_install_putc1(void (*p)(char c));

#define os_printf(...) printf(__VA_ARGS__)
#define os_memcpy(a,b,c) memcpy(a,b,c)

typedef void wdtfntype();
static wdtfntype *ets_wdt_disable=(wdtfntype *)0x400030f0;
static wdtfntype *ets_wdt_enable=(wdtfntype *)0x40002fa0;

#define EXCEPTION_GDB_SP_OFFSET 0x100

#define ETS_UART_INUM 							5
#define REG_UART_BASE(i)						(0x60000000+(i)*0xf00)
#define UART_STATUS(i)							(REG_UART_BASE(i) + 0x1C)
#define UART_RXFIFO_CNT							0x000000FF
#define UART_RXFIFO_CNT_S						0
#define UART_TXFIFO_CNT							0x000000FF
#define UART_TXFIFO_CNT_S						16
#define UART_FIFO(i)							(REG_UART_BASE(i) + 0x0)
#define UART_INT_ENA(i)							(REG_UART_BASE(i) + 0xC)
#define UART_INT_CLR(i)							(REG_UART_BASE(i) + 0x10)
#define UART_RXFIFO_TOUT_INT_ENA				(BIT(8))
#define UART_RXFIFO_FULL_INT_ENA				(BIT(0))
#define UART_RXFIFO_TOUT_INT_CLR				(BIT(8))
#define UART_RXFIFO_FULL_INT_CLR				(BIT(0))

// Length of buffer used to reserve GDB commands. Has to be at least able to fit the G command, which
// implies a minimum size of about 190 bytes.
#define PBUFLEN 256
// Length of gdb stdout buffer, for console redirection
#define OBUFLEN 32

//The asm stub saves the Xtensa registers here when a debugging exception happens.
struct xtensa_exception_frame_t gdbstub_savedRegs;

//This is the debugging exception stack.
int exceptionStack[256];

static unsigned char cmd[PBUFLEN];		// GDB command input buffer
static char gdbstub_packet_crc;			// Checksum of the output packet
static unsigned char obuf[OBUFLEN];		// GDB stdout buffer
static int obufpos=0;					// Current position in the buffer

static int32_t singleStepPs = -1;			// Stores ps when single-stepping instruction. -1 when not in use.

void gdbstub_icount_ena_single_step() {
	__asm volatile (
		"wsr %0, ICOUNTLEVEL" "\n"
		"wsr %1, ICOUNT" "\n"
	:: "a" (XCHAL_DEBUGLEVEL), "a" (-2));

	__asm volatile ("isync");
}

// Small function to feed the hardware watchdog. Needed to stop the ESP from resetting
// due to a watchdog timeout while reading a command.
static void ATTR_GDBFN wdt_keep_alive() {
	uint64_t * wdtval = (uint64_t*) 0x3ff21048;
	uint64_t * wdtovf = (uint64_t*) 0x3ff210cc;

	int * wdtctl= (int*) 0x3ff210c8;
	*wdtovf = * wdtval + 1600000;
	*wdtctl |= (1 << 31);
}

// Receive a char from the uart. Uses polling and feeds the watchdog.
static int ATTR_GDBFN gdb_recv_char() {
	int i;
	while (((READ_PERI_REG(UART_STATUS(0)) >> UART_RXFIFO_CNT_S) & UART_RXFIFO_CNT) == 0) {
		wdt_keep_alive();
	}
	i = READ_PERI_REG(UART_FIFO(0));
	return i;
}

// Send a char to the uart.
static void ATTR_GDBFN gdb_send_char(char c) {
	while (((READ_PERI_REG(UART_STATUS(0)) >> UART_TXFIFO_CNT_S) & UART_TXFIFO_CNT) >= 126);
	WRITE_PERI_REG(UART_FIFO(0), c);
}

// Send the start of a packet; reset checksum calculation.
static void ATTR_GDBFN gdb_packet_start() {
	gdbstub_packet_crc = 0;
	gdb_send_char('$');
}

// Send a char as part of a packet
static void ATTR_GDBFN gdb_packet_char(char c) {
	if (c=='#' || c=='$' || c=='}' || c=='*') {
		gdb_send_char('}');
		gdb_send_char(c ^ 0x20);
		gdbstub_packet_crc += (c ^ 0x20) + '}';
	} else {
		gdb_send_char(c);
		gdbstub_packet_crc += c;
	}
}

// Send a string as part of a packet
static void ATTR_GDBFN gdb_packet_str(char *c) {
	while (*c != 0) {
		gdb_packet_char(*c);
		c++;
	}
}

// Send a hex val as part of a packet. 'bits'/4 dictates the number of hex chars sent.
static void ATTR_GDBFN gdb_packet_hex(int val, int bits) {
	char hexChars[]="0123456789abcdef";
	int i;
	for (i = bits; i > 0; i -= 4) {
		gdb_packet_char(hexChars[(val>>(i-4))&0xf]);
	}
}

// Finish sending a packet.
static void ATTR_GDBFN gdb_packet_end() {
	gdb_send_char('#');
	gdb_packet_hex(gdbstub_packet_crc, 8);
}

// Error states used by the routines that grab stuff from the incoming gdb packet
#define ST_ENDPACKET	-1
#define ST_ERR			-2
#define ST_OK			-3
#define ST_CONT			-4

// Grab a hex value from the gdb packet. Ptr will get positioned on the end
// of the hex string, as far as the routine has read into it. Bits/4 indicates
// the max amount of hex chars it gobbles up. Bits can be -1 to eat up as much
// hex chars as possible.
static long ATTR_GDBFN gdb_get_hex_val(uint8_t **ptr, size_t bits) {
	int i;
	int no;
	unsigned int v = 0;
	char c;
	no = bits / 4;
	if (bits == -1) {
		no = 64;
	}
	for (i = 0; i < no; i++) {
		c = **ptr;
		(*ptr)++;
		if (c >= '0' && c <= '9') {
			v <<= 4;
			v |= (c - '0');
		} else if (c >= 'A' && c <= 'F') {
			v <<= 4;
			v |= (c - 'A') + 10;
		} else if (c >= 'a' && c <= 'f') {
			v <<= 4;
			v |= (c - 'a') + 10;
		} else if (c == '#') {
			if (bits == -1) {
				(*ptr)--;
				return v;
			}
			return ST_ENDPACKET;
		} else {
			if (bits == -1) {
				(*ptr)--;
				return v;
			}
			return ST_ERR;
		}
	}
	return v;
}

// Swap an int into the form gdb wants it
static uint32_t ATTR_GDBFN bswap32(uint32_t i) {
	uint32_t r;
	r =  ((i>>24) & 0xff);
	r |= ((i>>16) & 0xff) << 8;
	r |= ((i>>8)  & 0xff) << 16;
	r |= ((i>>0)  & 0xff) << 24;
	return r;
}

// Read a byte from the ESP8266 memory.
static uint8_t ATTR_GDBFN mem_read_byte(uintptr_t p) {
	int * i = (int *) (p & (~3));

	// TODO: better address range check?
	if (p < 0x20000000 || p >= 0x60000000) {
		return -1;
	}

	return * i >> ((p & 3) * 8);
}

// Write a byte to the ESP8266 memory.
static void ATTR_GDBFN mem_write_byte(uintptr_t p, uint8_t d) {
	int *i = (int*) (p & (~3));

	if (p < 0x20000000 || p >= 0x60000000) {
		return;
	}

	if ((p & 3) == 0) {
		*i = (*i & 0xffffff00) | (d << 0);
	}

	if ((p & 3) == 1) {
		*i = (*i & 0xffff00ff) | (d << 8);
	}

	if ((p & 3) == 2) {
		*i = (*i & 0xff00ffff) | (d << 16);
	}

	if ((p & 3) == 3) {
		*i = (*i & 0x00ffffff) | (d << 24);
	}
}

// Returns 1 if it makes sense to write to addr p
static uint8_t ATTR_GDBFN mem_addr_valid(uintptr_t p) {
	if (p >= 0x3ff00000 && p < 0x40000000) {
		return 1;
	}

	if (p >= 0x40100000 && p < 0x40140000) {
		return 1;
	}

	if (p >= 0x60000000 && p < 0x60002000) {
		return 1;
	}

	return 0;
}

/* 
 Register file in the format lx106 gdb port expects it.
 Inspired by gdb/regformats/reg-xtensa.dat from
 https://github.com/jcmvbkbc/crosstool-NG/blob/lx106-g%2B%2B/overlays/xtensa_lx106.tar
 As decoded by Cesanta.
*/
struct regfile {
	uint32_t a[16];
	uint32_t pc;
	uint32_t sar;
	uint32_t litbase;
	uint32_t sr176;
	uint32_t sr208;
	uint32_t ps;
};

// Send the reason execution is stopped to GDB.
static void ATTR_GDBFN gdb_send_reason() {
	// exception-to-signal mapping
	uint8_t exceptionSignal[] = { 4, 31, 11, 11, 2, 6, 8, 0, 6, 7, 0, 0, 7, 7, 7, 7 };
	size_t i=0;

	gdb_packet_start();
	gdb_packet_char('T');

	if (gdbstub_savedRegs.reason == 0xff) {
		gdb_packet_hex(2, 8); // sigint
	} else if (gdbstub_savedRegs.reason & 0x80) {
		// We stopped because of an exception. Convert exception code to a signal number and send it.
		i = gdbstub_savedRegs.reason & 0x7f;
		if (i < sizeof(exceptionSignal)) {
			gdb_packet_hex(exceptionSignal[i], 8);
		} else {
			gdb_packet_hex(11, 8);
		}
	} else {
		// We stopped because of a debugging exception.
		gdb_packet_hex(5, 8); // sigtrap
		// Current Xtensa GDB versions don't seem to request this, so let's leave it off.
#if 0
		if (gdbstub_savedRegs.reason&(1<<0)) {
			reason="break";
		}
		if (gdbstub_savedRegs.reason&(1<<1)) {
			reason="hwbreak";
		}
		if (gdbstub_savedRegs.reason&(1<<2)) {
			reason="watch";
		}
		if (gdbstub_savedRegs.reason&(1<<3)) {
			reason="swbreak";
		}
		if (gdbstub_savedRegs.reason&(1<<4)) {
			reason="swbreak";
		}

		gdb_packet_str(reason);
		gdb_packet_char(':');
		//TODO: watch: send address
#endif
	}
	gdb_packet_end();
}

// Handle a command as received from GDB.
static int ATTR_GDBFN gdb_handle_command(uint8_t * cmd, size_t len) {
	// Handle a command
	int i, j, k;
	uint8_t * data = cmd + 1;

	if (cmd[0] == 'g') {
		// send all registers to gdb
		gdb_packet_start();
		gdb_packet_hex(bswap32(gdbstub_savedRegs.a0), 32);
		gdb_packet_hex(bswap32(gdbstub_savedRegs.a1), 32);

		for (i = 2; i < 16; i++) {
			gdb_packet_hex(bswap32(gdbstub_savedRegs.a[i - 2]), 32);
		}

		gdb_packet_hex(bswap32(gdbstub_savedRegs.pc), 32);
		gdb_packet_hex(bswap32(gdbstub_savedRegs.sar), 32);
		gdb_packet_hex(bswap32(gdbstub_savedRegs.litbase), 32);
		gdb_packet_hex(bswap32(gdbstub_savedRegs.sr176), 32);
		gdb_packet_hex(0, 32);
		gdb_packet_hex(bswap32(gdbstub_savedRegs.ps), 32);
		gdb_packet_end();
	} else if (cmd[0]=='G') {
		// receive content for all registers from gdb
		gdbstub_savedRegs.a0=bswap32(gdb_get_hex_val(&data, 32));
		gdbstub_savedRegs.a1=bswap32(gdb_get_hex_val(&data, 32));

		for (i = 2; i < 16; i++) {
			gdbstub_savedRegs.a[i - 2] = bswap32(gdb_get_hex_val(&data, 32));
		}

		gdbstub_savedRegs.pc=bswap32(gdb_get_hex_val(&data, 32));
		gdbstub_savedRegs.sar=bswap32(gdb_get_hex_val(&data, 32));
		gdbstub_savedRegs.litbase=bswap32(gdb_get_hex_val(&data, 32));
		gdbstub_savedRegs.sr176=bswap32(gdb_get_hex_val(&data, 32));

		gdb_get_hex_val(&data, 32);

		gdbstub_savedRegs.ps=bswap32(gdb_get_hex_val(&data, 32));
		gdb_packet_start();
		gdb_packet_str("OK");
		gdb_packet_end();
	} else if (cmd[0]=='m') {
		// read memory to gdb
		i=gdb_get_hex_val(&data, -1);
		data++;
		j=gdb_get_hex_val(&data, -1);
		gdb_packet_start();
		for (k=0; k<j; k++) {
			gdb_packet_hex(mem_read_byte(i++), 8);
		}
		gdb_packet_end();
	} else if (cmd[0]=='M') {
		// write memory from gdb
		// addr
		i = gdb_get_hex_val(&data, -1);
		// skip
		data++;
		//length
		j = gdb_get_hex_val(&data, -1);
		data++;
		// skip
		if (mem_addr_valid(i) && mem_addr_valid(i+j)) {
			for (k = 0; k < j; k++) {
				mem_write_byte(i, gdb_get_hex_val(&data, 8));
				i++;
			}

			// Make sure caches are up-to-date. Procedure according to Xtensa ISA document, ISYNC inst desc.
			__asm volatile (
				"isync \n"
				"isync \n"
			);

			gdb_packet_start();
			gdb_packet_str("OK");
			gdb_packet_end();
		} else {
			//Trying to do a software breakpoint on a flash proc, perhaps?
			gdb_packet_start();
			gdb_packet_str("E01");
			gdb_packet_end();
		}
	} else if (cmd[0] == '?') {
		// Reply with stop reason
		gdb_send_reason();
	} else if (strncmp(cmd, "vCont?", 6) == 0) {
		gdb_packet_start();
		gdb_packet_str("vCont;c;s;r");
		gdb_packet_end();
	} else if (strncmp((char*)cmd, "vCont;c", 7) == 0 || cmd[0]=='c') {
		// continue execution
		return ST_CONT;
	} else if (strncmp((char*)cmd, "vCont;s", 7) == 0 || cmd[0]=='s') {
		// single-step instruction
		// Single-stepping can go wrong if an interrupt is pending, especially when it is e.g. a task switch:
		// the ICOUNT register will overflow in the task switch code. That is why we disable interupts when
		// doing single-instruction stepping.
		singleStepPs = gdbstub_savedRegs.ps;
		gdbstub_savedRegs.ps = (gdbstub_savedRegs.ps & ~0xf) | (XCHAL_DEBUGLEVEL - 1);
		gdbstub_icount_ena_single_step();
		return ST_CONT;
	} else if (cmd[0]=='q') {
		// Extended query
		if (strncmp((char*)&cmd[1], "Supported", 9)==0) {
			// Capabilities query
			gdb_packet_start();
			gdb_packet_str("swbreak+;hwbreak+;PacketSize=255");
			gdb_packet_end();
		} else {
			// We don't support other queries.
			gdb_packet_start();
			gdb_packet_end();
			return ST_ERR;
		}
	} else if (cmd[0] == 'Z') {
		// Set hardware break/watchpoint.
		// skip 'x,'
		data += 2;
		i = gdb_get_hex_val(&data, -1);
		// skip ','
		data += 1;
		j = gdb_get_hex_val(&data, -1);
		gdb_packet_start();

		// Set breakpoint
		if (cmd[1] == '1') {
			if (gdbstub_set_hw_breakpoint(i, j)) {
				gdb_packet_str("OK");
			} else {
				gdb_packet_str("E01");
			}
		} else if (cmd[1] == '2' || cmd[1] == '3' || cmd[1] == '4') {
			// Set watchpoint
			int access = 0;
			int mask = 0;

			switch (cmd[1]) {
			case '2':
				// write
				access = 2;
				break;
			case '3':
				// read
				access = 1;
				break;
			case '4':
				// access
				access = 3;
				break;
			}

			switch (j) {
			case 1:
				mask = 0x3F;
				break;
			case 2:
				mask = 0x3E;
				break;
			case 4:
				mask = 0x3C;
				break;
			case 8:
				mask = 0x38;
				break;
			case 16:
				mask = 0x30;
				break;
			case 32:
				mask = 0x20;
				break;
			case 64:
				mask = 0x00;
				break;
			}

			if (mask != 0 && gdbstub_set_hw_watchpoint(i, mask, access)) {
				gdb_packet_str("OK");
			} else {
				gdb_packet_str("E01");
			}
		}

		gdb_packet_end();
	} else if (cmd[0] == 'z') {
		// Clear hardware break/watchpoint
		// skip 'x,'
		data += 2;
		i = gdb_get_hex_val(&data, -1);
		// skip ','
		data++;
		j = gdb_get_hex_val(&data, -1);
		gdb_packet_start();

		if (cmd[1]=='1') {
			// hardware breakpoint
			if (gdbstub_del_hw_breakpoint(i)) {
				gdb_packet_str("OK");
			} else {
				gdb_packet_str("E01");
			}
		} else if (cmd[1]=='2' || cmd[1]=='3' || cmd[1]=='4') {
			// hardware watchpoint
			if (gdbstub_del_hw_watchpoint(i)) {
				gdb_packet_str("OK");
			} else {
				gdb_packet_str("E01");
			}
		}

		gdb_packet_end();
	} else {
		// We don't recognize or support whatever GDB just sent us.
		gdb_packet_start();
		gdb_packet_end();
		return ST_ERR;
	}
	return ST_OK;
}


// Lower layer: grab a command packet and check the checksum
// Calls gdbHandleCommand on the packet if the checksum is OK
// Returns ST_OK on success, ST_ERR when checksum fails, a
// character if it is received instead of the GDB packet
// start char.
static int ATTR_GDBFN gdb_read_command() {
	uint8_t c;
	uint8_t chsum=0, rchsum;
	uint8_t sentchs[2];

	size_t p = 0;
	uint8_t * ptr;
	c = gdb_recv_char();

	if (c != '$') {
		return c;
	}

	while(1) {
		c = gdb_recv_char();
		if (c == '#') {	//end of packet, checksum follows
			cmd[p] = 0;
			break;
		}
		chsum += c;
		if (c == '$') {
			// Wut, restart packet?
			chsum = 0;
			p = 0;
			continue;
		}
		if (c == '}') {
			// escape the next char
			c = gdb_recv_char();
			chsum += c;
			c ^= 0x20;
		}
		cmd[p++] = c;
		if (p >= PBUFLEN) {
			return ST_ERR;
		}
	}

	// A # has been received. Get and check the received chsum.
	sentchs[0] = gdb_recv_char();
	sentchs[1] = gdb_recv_char();
	ptr = &sentchs[0];

	rchsum = gdb_get_hex_val(&ptr, 8);
//	os_printf("c %x r %x\n", chsum, rchsum);

	if (rchsum != chsum) {
		gdb_send_char('-');
		return ST_ERR;
	} else {
		gdb_send_char('+');
		return gdb_handle_command(cmd, p);
	}
}

//Get the value of one of the A registers
static uint32_t ATTR_GDBFN get_reg_val(size_t reg) {
	if (reg == 0) {
		return gdbstub_savedRegs.a0;
	}
	if (reg == 1) {
		return gdbstub_savedRegs.a1;
	}
	return gdbstub_savedRegs.a[reg - 2];
}

//Set the value of one of the A registers
static void ATTR_GDBFN set_reg_val(size_t reg, uint32_t val) {
	// os_printf("%x -> %x\n", val, reg);
	if (reg == 0) {
		gdbstub_savedRegs.a0 = val;
	}
	if (reg == 1) {
		gdbstub_savedRegs.a1 = val;
	}

	gdbstub_savedRegs.a[reg - 2] = val;
}

// Emulate the l32i/s32i instruction we've stopped at.
static void ATTR_GDBFN emulLdSt() {
	uint8_t i0 = mem_read_byte(gdbstub_savedRegs.pc);
	uint8_t i1 = mem_read_byte(gdbstub_savedRegs.pc + 1);
	uint8_t i2 = mem_read_byte(gdbstub_savedRegs.pc + 2);

	uintptr_t * p;
	if ((i0 & 0xf) == 2 && (i1 & 0xf0) == 0x20) {
		// l32i
		p = (uintptr_t *) get_reg_val(i1 & 0xf) + (i2 * 4);
		set_reg_val(i0 >> 4, *p);
		gdbstub_savedRegs.pc += 3;
	} else if ((i0 & 0xf) == 0x8) {
		// l32i.n
		p = (uintptr_t *) get_reg_val(i1 & 0xf) + ((i1 >> 4) * 4);
		set_reg_val(i0 >> 4, *p);
		gdbstub_savedRegs.pc += 2;
	} else if ((i0 & 0xf) == 2 && (i1 & 0xf0) == 0x60) {
		// s32i
		p = (uintptr_t *) get_reg_val(i1 & 0xf) + (i2 * 4);
		*p = get_reg_val(i0 >> 4);
		gdbstub_savedRegs.pc += 3;
	} else if ((i0 & 0xf) == 0x9) {
		// s32i.n
		p = (uintptr_t *) get_reg_val(i1 & 0xf) + ((i1 >> 4) * 4);
		*p = get_reg_val(i0 >> 4);
		gdbstub_savedRegs.pc += 2;
	} else {
		// os_printf("GDBSTUB: No l32i/s32i instruction: %x %x %x. Huh?", i2, i1, i0);
	}
}

// We just caught a debug exception and need to handle it. This is called from an assembly
// routine in gdbstub-entry.S
void ATTR_GDBFN gdbstub_handle_debug_exception() {
	ets_wdt_disable();

	if (singleStepPs != -1) {
		// We come here after single-stepping an instruction. Interrupts are disabled
		// for the single step. Re-enable them here.
		gdbstub_savedRegs.ps = (gdbstub_savedRegs.ps & ~0xf) | (singleStepPs & 0xf);
		singleStepPs = -1;
	}

	gdb_send_reason();
	while (gdb_read_command() != ST_CONT);

	if ((gdbstub_savedRegs.reason & 0x84) == 0x4) {
		// We stopped due to a watchpoint. We can't re-execute the current instruction
		// because it will happily re-trigger the same watchpoint, so we emulate it
		// while we're still in debugger space.
		emulLdSt();
	} else if ((gdbstub_savedRegs.reason & 0x88) == 0x8) {
		// We stopped due to a BREAK instruction. Skip over it.
		// Check the instruction first; gdb may have replaced it with the original instruction
		// if it's one of the breakpoints it set.
		if (mem_read_byte(gdbstub_savedRegs.pc + 2) == 0
				&& (mem_read_byte(gdbstub_savedRegs.pc + 1) & 0xf0) == 0x40
				&& (mem_read_byte(gdbstub_savedRegs.pc) & 0x0f) == 0x00) {
			gdbstub_savedRegs.pc += 3;
		}
	} else if ((gdbstub_savedRegs.reason & 0x90) == 0x10) {
		// We stopped due to a BREAK.N instruction. Skip over it, after making sure the instruction
		// actually is a BREAK.N
		if ((mem_read_byte(gdbstub_savedRegs.pc + 1) & 0xf0) == 0xf0
				&& mem_read_byte(gdbstub_savedRegs.pc) == 0x2d) {
			gdbstub_savedRegs.pc += 3;
		}
	}

	ets_wdt_enable();
}

// Freetos exception. This routine is called by an assembly routine in gdbstub-entry.S
void ATTR_GDBFN gdbstub_handle_user_exception() {
	ets_wdt_disable();

	// mark as an exception reason
	gdbstub_savedRegs.reason |= 0x80;
	gdb_send_reason();

	while (gdb_read_command() != ST_CONT);

	ets_wdt_enable();
}

// Replacement putchar1 routine. Instead of spitting out the character directly, it will buffer up to
// OBUFLEN characters (or up to a \n, whichever comes earlier) and send it out as a gdb stdout packet.
static void ATTR_GDBFN gdb_semihost_putchar1(char c) {
	obuf[obufpos++] = c;

	if (c == '\n' || obufpos == OBUFLEN) {
		gdb_packet_start();
		gdb_packet_char('O');
		for (size_t i = 0; i < obufpos; i++) {
			gdb_packet_hex(obuf[i], 8);
		}
		gdb_packet_end();
		obufpos = 0;
	}
}

extern void gdbstub_user_exception_entry();
extern void gdbstub_debug_exception_entry();

static void ATTR_GDBINIT install_exceptions() {
	extern void (* debug_exception_handler)();

	debug_exception_handler = &gdbstub_debug_exception_entry;
}

// TODO: use gdbstub stack for this function too
void ATTR_GDBFN gdbstub_handle_uart_int() {
	uint8_t do_debug = 0;
	size_t fifolen = 0;

	fifolen = (READ_PERI_REG(UART_STATUS(0)) >> UART_RXFIFO_CNT_S) & UART_RXFIFO_CNT;

	while (fifolen != 0) {
		// Check if any of the chars is control-C. Throw away the rest.
		if ((READ_PERI_REG(UART_FIFO(0)) & 0xFF) == 0x3) {
			do_debug = 1;
			break;
		}
		fifolen--;
	}

	WRITE_PERI_REG(UART_INT_CLR(0), UART_RXFIFO_FULL_INT_CLR | UART_RXFIFO_TOUT_INT_CLR);

	// TODO: restore a0, a1 as well in esp-open-rtos
	// TODO: save and restore a14, a15, a16 in esp-open-rtos

	if (do_debug) {
		extern uint32_t debug_saved_ctx;

		uint32_t * isr_stack;
		const size_t isr_stack_reg_offset = 5;
		uint32_t * debug_saved_ctx_p = &debug_saved_ctx;

		__asm volatile (
			"rsr %0, %1"
		: "=r" (gdbstub_savedRegs.pc) : "i" (EPC + XCHAL_INT5_LEVEL));

		gdbstub_savedRegs.a0 = debug_saved_ctx_p[0];
		gdbstub_savedRegs.a1 = debug_saved_ctx_p[1];

		isr_stack = (uint32_t *) (gdbstub_savedRegs.a1 - 0x50);
		gdbstub_savedRegs.ps = isr_stack[2];

		for (size_t x = 2; x < 13; x++) {
			gdbstub_savedRegs.a[x - 2] =
				isr_stack[isr_stack_reg_offset - 2 + x];
		}

		gdbstub_savedRegs.reason = 0xff; // mark as user break reason

		ets_wdt_disable();

		gdb_send_reason();
		while (gdb_read_command() != ST_CONT);

		ets_wdt_enable();

		__asm volatile (
			"wsr %0, %1"
		: "=r" (gdbstub_savedRegs.pc) : "i" (EPC + XCHAL_INT5_LEVEL));

		isr_stack[2] = gdbstub_savedRegs.ps;

		debug_saved_ctx_p[0] = gdbstub_savedRegs.a0;
		debug_saved_ctx_p[1] = gdbstub_savedRegs.a1;

		for (size_t x = 2; x < 13; x++) {
			isr_stack[isr_stack_reg_offset - 2 + x] =
				gdbstub_savedRegs.a[x - 2];
		}
	}
}

static void ATTR_GDBINIT install_uart_hdlr() {
	_xt_isr_attach(ETS_UART_INUM, gdbstub_handle_uart_int);

	SET_PERI_REG_MASK(UART_INT_ENA(0), UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_TOUT_INT_ENA);

	// enable UART interrupt
	uint32_t intenable;
	__asm volatile (
		"rsr %0, intenable" "\n"
		"or %0, %0, %1" "\n"
		"wsr %0, intenable" "\n"
	:: "r" (intenable), "r" (BIT(ETS_UART_INUM)));
}

// gdbstub initialization routine.
void ATTR_GDBINIT gdbstub_init() {
	// install stdout wrapper
	// TODO: fix esp-open-sdk compat
	sdk_os_install_putc1(gdb_semihost_putchar1);

	// install UART interrupt handler
	install_uart_hdlr();

	// install debug exception wrapper
	install_exceptions();

	// gdbstub_init_debug_entry();
}

