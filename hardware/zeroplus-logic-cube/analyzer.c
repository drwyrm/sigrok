/*
 Copyright (c) 2010 Sven Peter <sven@fail0verflow.com>
 Copyright (c) 2010 Haxx Enterprises <bushing@gmail.com>
 All rights reserved.

 Redistribution and use in source and binary forms, with or
 without modification, are permitted provided that the following
 conditions are met:

 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.

*/
#include <assert.h>

#include "analyzer.h"
#include "gl_usb.h"

enum {
	HARD_DATA_CHECK_SUM = 0x00,
	PASS_WORD,

	DEVICE_ID0 			= 0x10,
	DEVICE_ID1,

	START_STATUS		= 0x20,
	DEVICE_STATUS		= 0x21,
	FREQUENCY_REG0		= 0x30,
	FREQUENCY_REG1,
	FREQUENCY_REG2,
	FREQUENCY_REG3,
	FREQUENCY_REG4,
	MEMORY_LENGTH,
	CLOCK_SOURCE,

	TRIGGER_STATUS0		= 0x40,
	TRIGGER_STATUS1,
	TRIGGER_STATUS2,
	TRIGGER_STATUS3,
	TRIGGER_STATUS4,
	TRIGGER_STATUS5,
	TRIGGER_STATUS6,
	TRIGGER_STATUS7,
	TRIGGER_STATUS8,

	TRIGGER_COUNT0		= 0x50,
	TRIGGER_COUNT1,

	TRIGGER_LEVEL0		= 0x55,
	TRIGGER_LEVEL1,
	TRIGGER_LEVEL2,
	TRIGGER_LEVEL3,

	RAMSIZE_TRIGGERBAR_ADDRESS0	= 0x60,
	RAMSIZE_TRIGGERBAR_ADDRESS1,
	RAMSIZE_TRIGGERBAR_ADDRESS2,
	TRIGGERBAR_ADDRESS0,
	TRIGGERBAR_ADDRESS1,
	TRIGGERBAR_ADDRESS2,
	DONT_CARE_TRIGGERBAR,

	FILTER_ENABLE			= 0x70,
	FILTER_STATUS,

	ENABLE_DELAY_TIME0	= 0x7A,
	ENABLE_DELAY_TIME1,

	ENABLE_INSERT_DATA0 = 0x80,
	ENABLE_INSERT_DATA1,
	ENABLE_INSERT_DATA2,
	ENABLE_INSERT_DATA3,
	COMPRESSION_TYPE0,
	COMPRESSION_TYPE1,

	TRIGGER_ADDRESS0	= 0x90,
	TRIGGER_ADDRESS1,
	TRIGGER_ADDRESS2,

	NOW_ADDRESS0		= 0x96,
	NOW_ADDRESS1,
	NOW_ADDRESS2,

	STOP_ADDRESS0		= 0x9B,
	STOP_ADDRESS1,
	STOP_ADDRESS2,

	READ_RAM_STATUS		= 0xA0
};

static int g_trigger_status[9] = {0};
static int g_trigger_count = 1;

static int g_filter_status[8] = {0};
static int g_filter_enable = 0;

static int g_freq_value = 100;
static int g_freq_scale = FREQ_SCALE_MHZ;
static int g_memory_size = MEMORY_SIZE_512K;
static int g_ramsize_triggerbar_addr = 0x80000>>2;
static int g_triggerbar_addr = 0x3fe;
static int g_compression = COMPRESSION_NONE;

// maybe unk specifies an "endpoint" or "register" of sorts
static int analyzer_write_status(libusb_device_handle *devh, unsigned char unk, unsigned char flags)
{
	assert(unk <= 3);
	return gl_reg_write(devh, START_STATUS, unk << 6 | flags);
}

static int __analyzer_set_freq(libusb_device_handle *devh, int freq, int scale)
{
	int reg0=0, divisor=0, reg2=0;
	switch (scale) {
		case FREQ_SCALE_MHZ: // MHz
			if (freq >= 100 && freq <= 200) {
				reg0 = freq * 0.1;
				divisor = 1;
				reg2 = 0;
				break;
			}
			if (freq >= 50 && freq < 100) {
				reg0 = freq * 0.2;
				divisor = 2;
				reg2 = 0;
				break;
			}
			if (freq >= 10 && freq < 50) {
				if (freq == 25) {
					reg0 = 25;
					divisor = 5;
					reg2 = 1;
					break;
				} else {
					reg0 = freq * 0.5;
					divisor = 5;
					reg2 = 1;
					break;
				}
			}
			if (freq >= 2 && freq < 10) {
				divisor = 5;
				reg0 = freq * 2;
				reg2 = 2;
				break;
			}
			if (freq == 1) {
				divisor = 5;
				reg2 = 16;
				reg0 = 5;
				break;
			}
			divisor = 5;
			reg0 = 5;
			reg2 = 64;
			break;
		case FREQ_SCALE_HZ: // Hz
			if (freq >= 500 && freq < 1000) {
				reg0 = freq * 0.01;
				divisor = 10;
				reg2 = 64;
				break;
			}
			if (freq >= 300 && freq < 500) {
				reg0 = freq * 0.005 * 8;
				divisor = 5;
				reg2 = 67;
				break;
			}
			if (freq >= 100 && freq < 300) {
				reg0 = freq * 0.005 * 16;
				divisor = 5;
				reg2 = 68;
				break;
			}
			divisor = 5;
			reg0 = 5;
			reg2 = 64;
			break;
		case FREQ_SCALE_KHZ: // KHz
			if (freq >= 500 && freq < 1000) {
				reg0 = freq * 0.01;
				divisor = 5;
				reg2 = 17;
				break;
			}
			if (freq >= 100 && freq < 500) {
				reg0 = freq * 0.05;
				divisor = 5;
				reg2 = 32;
				break;
			}
			if (freq >= 50 && freq < 100) {
				reg0 = freq * 0.1;
				divisor = 5;
				reg2 = 33;
				break;
			}
			if (freq >= 10 && freq < 50) {
				if (freq == 25) {
					reg0 = 25;
					divisor = 5;
					reg2 = 49;
					break;
				}
				reg0 = freq * 0.5;
				divisor = 5;
				reg2 = 48;
				break;
			}
			if (freq >= 2 && freq < 10) {
				divisor = 5;
				reg0 = freq * 2;
				reg2 = 50;
				break;
			}
			divisor = 5;
			reg0 = 5;
			reg2 = 64;
			break;
		default:
			divisor = 5;
			reg0 = 5;
			reg2 = 64;
			break;
	}
	if (gl_reg_write(devh, FREQUENCY_REG0, divisor) < 0)
		return -1; // divisor maybe?

	if (gl_reg_write(devh, FREQUENCY_REG1, reg0) < 0)
		return -1; // 10 / 0.2

	if (gl_reg_write(devh, FREQUENCY_REG2, 0x02) < 0)
		return -1; // always 2

	if (gl_reg_write(devh, FREQUENCY_REG4, reg2) < 0)
		return -1;

	return 0;
}

static void __analyzer_set_ramsize_trigger_address(libusb_device_handle *devh, unsigned int address)
{
	gl_reg_write(devh, RAMSIZE_TRIGGERBAR_ADDRESS0, (address >>  0) & 0xFF);
	gl_reg_write(devh, RAMSIZE_TRIGGERBAR_ADDRESS1, (address >>  8) & 0xFF);
	gl_reg_write(devh, RAMSIZE_TRIGGERBAR_ADDRESS2, (address >> 16) & 0xFF);
}

static void __analyzer_set_triggerbar_address(libusb_device_handle *devh, unsigned int address)
{
	gl_reg_write(devh, TRIGGERBAR_ADDRESS0, (address >>  0) & 0xFF);
	gl_reg_write(devh, TRIGGERBAR_ADDRESS1, (address >>  8) & 0xFF);
	gl_reg_write(devh, TRIGGERBAR_ADDRESS2, (address >> 16) & 0xFF);
}

static void __analyzer_set_compression(libusb_device_handle *devh, unsigned int type)
{
	gl_reg_write(devh, COMPRESSION_TYPE0, (type >> 0) & 0xFF);
	gl_reg_write(devh, COMPRESSION_TYPE1, (type >> 8) & 0xFF);
}

static void __analyzer_set_trigger_count(libusb_device_handle *devh, unsigned int count)
{
	gl_reg_write(devh, TRIGGER_COUNT0, (count >> 0) & 0xFF);
	gl_reg_write(devh, TRIGGER_COUNT1, (count >> 8) & 0xFF);
}

static void analyzer_write_enable_insert_data(libusb_device_handle *devh)
{
	gl_reg_write(devh, ENABLE_INSERT_DATA0, 0x12);
	gl_reg_write(devh, ENABLE_INSERT_DATA1, 0x34);
	gl_reg_write(devh, ENABLE_INSERT_DATA2, 0x56);
	gl_reg_write(devh, ENABLE_INSERT_DATA3, 0x78);
}

static void analyzer_set_filter(libusb_device_handle *devh)
{
	int i;
	gl_reg_write(devh, FILTER_ENABLE, g_filter_enable);
	for (i = 0; i < 8; i++)
		gl_reg_write(devh, FILTER_STATUS + i, g_filter_status[i]);
}

void analyzer_reset(libusb_device_handle *devh)
{
	analyzer_write_status(devh, 3, STATUS_FLAG_NONE); // reset device
	analyzer_write_status(devh, 3, STATUS_FLAG_RESET); // reset device
}

void analyzer_initialize(libusb_device_handle *devh)
{
	analyzer_write_status(devh, 1, STATUS_FLAG_NONE);
	analyzer_write_status(devh, 1, STATUS_FLAG_INIT);
	analyzer_write_status(devh, 1, STATUS_FLAG_NONE);
}

void analyzer_wait(libusb_device_handle *devh, int set, int unset)
{
	int status;
	while(1) {
		status = gl_reg_read(devh, DEVICE_STATUS);
		if ((status & set) && ((status & unset) == 0))
			return;
	}
}

int analyzer_read(libusb_device_handle *devh, void *buffer, unsigned int size)
{
	int i;

	if (size > 0x80000)
		size = 0x80000;

	analyzer_write_status(devh, 3, STATUS_FLAG_20 | STATUS_FLAG_READ);

	for (i = 0; i < 8; i++)
		(void)gl_reg_read(devh, 0xa0);

	int res = gl_read_bulk(devh, buffer, size);

	analyzer_write_status(devh, 3, STATUS_FLAG_20);
	analyzer_write_status(devh, 3, STATUS_FLAG_NONE);

	return res;
}

void analyzer_start(libusb_device_handle *devh)
{
	analyzer_write_status(devh, 1, STATUS_FLAG_NONE);
	analyzer_write_status(devh, 1, STATUS_FLAG_INIT);
	analyzer_write_status(devh, 1, STATUS_FLAG_NONE);
	analyzer_write_status(devh, 1, STATUS_FLAG_GO);
}

void analyzer_configure(libusb_device_handle *devh)
{
	// Write_Start_Status
	analyzer_write_status(devh, 1, STATUS_FLAG_RESET);
	analyzer_write_status(devh, 1, STATUS_FLAG_NONE);

	// Start_Config_Outside_Device ?
	analyzer_write_status(devh, 1, STATUS_FLAG_INIT);
	analyzer_write_status(devh, 1, STATUS_FLAG_NONE);

	//SetData_To_Frequence_Reg
	__analyzer_set_freq(devh, g_freq_value, g_freq_scale);

	//SetMemory_Length
	gl_reg_write(devh, MEMORY_LENGTH, g_memory_size);

	//Sele_Inside_Outside_Clock
	gl_reg_write(devh, CLOCK_SOURCE, 0x03);

	//Set_Trigger_Status
	int i;
	for (i = 0; i < 9; i++)
		gl_reg_write(devh, TRIGGER_STATUS0 + i, g_trigger_status[i]);

	__analyzer_set_trigger_count(devh, g_trigger_count);

	//Set_Trigger_Level
	gl_reg_write(devh, TRIGGER_LEVEL0, 0x31);
	gl_reg_write(devh, TRIGGER_LEVEL1, 0x31);
	gl_reg_write(devh, TRIGGER_LEVEL2, 0x31);
	gl_reg_write(devh, TRIGGER_LEVEL3, 0x31);

	__analyzer_set_ramsize_trigger_address(devh, g_ramsize_triggerbar_addr); // size of actual memory >> 2
	__analyzer_set_triggerbar_address(devh, g_triggerbar_addr);

	//Set_Dont_Care_TriggerBar
	gl_reg_write(devh, DONT_CARE_TRIGGERBAR, 0x01);

	//Enable_Status
	analyzer_set_filter(devh);

	//Set_Enable_Delay_Time
	gl_reg_write(devh, 0x7a, 0x00);
	gl_reg_write(devh, 0x7b, 0x00);
	analyzer_write_enable_insert_data(devh);
	__analyzer_set_compression(devh,g_compression);
}

void analyzer_add_trigger(int channel, int type)
{
	if ((channel & 0xf) >= 8)
		return;

	if (type == TRIGGER_HIGH || type == TRIGGER_LOW) {
		int i;
		if (channel & CHANNEL_A)
			i = 0;
		else if (channel & CHANNEL_B)
			i = 2;
		else if (channel & CHANNEL_C)
			i = 4;
		else if (channel & CHANNEL_D)
			i = 6;
		else
			return;
		if ((channel & 0xf) >= 4) {
			i++;
			channel -= 4;
		}
		g_trigger_status[i] |= 1 << ((2 * channel) + (type == TRIGGER_LOW ? 1 : 0));
	} else {
		if (type == TRIGGER_POSEDGE)
			g_trigger_status[8] = 0x40;
		else if(type == TRIGGER_NEGEDGE)
			g_trigger_status[8] = 0x80;
		else
			g_trigger_status[8] = 0xc0;

		// FIXME: just guessed the index; need to verify
		if (channel & CHANNEL_B)
			g_trigger_status[8] += 8;
		else if (channel & CHANNEL_C)
			g_trigger_status[8] += 16;
		else if (channel & CHANNEL_D)
			g_trigger_status[8] += 24;
		g_trigger_status[8] += channel % 8;
	}
}

void analyzer_add_filter(int channel, int type)
{
	if (type != FILTER_HIGH && type != FILTER_LOW)
		return;
	if ((channel & 0xf) >= 8)
		return;

	int i;
	if (channel & CHANNEL_A)
		i = 0;
	else if (channel & CHANNEL_B)
		i = 2;
	else if (channel & CHANNEL_C)
		i = 4;
	else if (channel & CHANNEL_D)
		i = 6;
	else
		return;
	if ((channel & 0xf) >= 4) {
		i++;
		channel -= 4;
	}
	g_filter_status[i] |= 1 << ((2 * channel) + (type == FILTER_LOW ? 1 : 0));
	g_filter_enable = 1;
}

void analyzer_set_trigger_count(int count)
{
	g_trigger_count = count;
}

void analyzer_set_freq(int freq, int scale)
{
	g_freq_value = freq;
	g_freq_scale = scale;
}

void analyzer_set_memory_size(unsigned int size)
{
	g_memory_size = size;
}


void analyzer_set_ramsize_trigger_address(unsigned int address)
{
	g_ramsize_triggerbar_addr = address;
}

void analyzer_set_triggerbar_address(unsigned int address)
{
	g_triggerbar_addr = address;
}

unsigned int analyzer_read_id(libusb_device_handle *devh)
{
	return gl_reg_read(devh, DEVICE_ID1) << 8 | gl_reg_read(devh, DEVICE_ID0);
}

unsigned int analyzer_get_stop_address(libusb_device_handle *devh)
{
	return gl_reg_read(devh, STOP_ADDRESS2) << 16 | gl_reg_read(devh, STOP_ADDRESS1) << 8 | gl_reg_read(devh, STOP_ADDRESS0);
}

unsigned int analyzer_get_now_address(libusb_device_handle *devh)
{
	return gl_reg_read(devh, NOW_ADDRESS2) << 16 | gl_reg_read(devh, NOW_ADDRESS1) << 8 | gl_reg_read(devh, NOW_ADDRESS0);
}

unsigned int analyzer_get_trigger_address(libusb_device_handle *devh)
{
	return gl_reg_read(devh, TRIGGER_ADDRESS2) << 16 | gl_reg_read(devh, TRIGGER_ADDRESS1) << 8 | gl_reg_read(devh, TRIGGER_ADDRESS0);
}

void analyzer_set_compression(unsigned int type)
{
	g_compression = type;
}

void analyzer_wait_button(libusb_device_handle *devh)
{
	analyzer_wait(devh, STATUS_BUTTON_PRESSED, 0);
}

void analyzer_wait_data(libusb_device_handle *devh)
{
	analyzer_wait(devh, STATUS_READY | 8, STATUS_BUSY);
}

int analyzer_decompress(void *input, unsigned int input_len, void *output, unsigned int output_len)
{
	unsigned char *in = input;
	unsigned char *out = output;
	unsigned int A, B, C, count;
	unsigned int written = 0;

	while (input_len > 0) {
		A = *in++;
		B = *in++;
		C = *in++;
		count = (*in++) + 1;

		if (count > output_len)
			count = output_len;
		output_len -= count;
		written += count;

		while (count--) {
			*out++ = A;
			*out++ = B;
			*out++ = C;
			*out++ = 0; // channel D
		}

		input_len -= 4;
		if (output_len == 0)
			break;
	}

	return written;
}
