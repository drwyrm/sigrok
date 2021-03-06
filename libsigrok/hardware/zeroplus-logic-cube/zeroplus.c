/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2010 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <inttypes.h>
#include <glib.h>
#include <libusb.h>
#include <sigrok.h>
#include "config.h"
#include "analyzer.h"

#define USB_VENDOR			0x0c12
#define USB_VENDOR_NAME			"Zeroplus"
#define USB_MODEL_NAME			"Logic Cube"
#define USB_MODEL_VERSION		""

#define USB_INTERFACE			0
#define USB_CONFIGURATION		1
#define NUM_TRIGGER_STAGES		4
#define TRIGGER_TYPES			"01"

#define PACKET_SIZE			2048	/* ?? */

typedef struct {
	unsigned short pid;
	char model_name[64];
	unsigned int channels;
	unsigned int sample_depth;	/* In Ksamples/channel */
	unsigned int max_sampling_freq;
} model_t;

/*
 * Note -- 16032, 16064 and 16128 *usually* -- but not always -- have the
 * same 128K sample depth.
 */
model_t zeroplus_models[] = {
	{0x7009, "LAP-C(16064)",  16, 64,   100},
	{0x700A, "LAP-C(16128)",  16, 128,  200},
	{0x700B, "LAP-C(32128)",  32, 128,  200},
	{0x700C, "LAP-C(321000)", 32, 1024, 200},
	{0x700D, "LAP-C(322000)", 32, 2048, 200},
	{0x700E, "LAP-C(16032)",  16, 32,   100},
	{0x7016, "LAP-C(162000)", 16, 2048, 200},
};

static int trigger_types[] = {
	TRIGGER_TYPE_LOGIC,
	0,
};

static int capabilities[] = {
	HWCAP_LOGIC_ANALYZER,
	HWCAP_SAMPLERATE,
	HWCAP_PROBECONFIG,
	HWCAP_TRIGGERCONFIG,
	HWCAP_CAPTURE_RATIO,

	/* These are really implemented in the driver, not the hardware. */
	HWCAP_LIMIT_SAMPLES,
	0,
};

/* List of struct sigrok_device_instance, maintained by opendev()/closedev(). */
static GSList *device_instances = NULL;

static libusb_context *usb_context = NULL;

/*
 * The hardware supports more samplerates than these, but these are the
 * options hardcoded into the vendor's Windows GUI.
 */

/*
 * TODO: We shouldn't support 150MHz and 200MHz on devices that don't go up
 * that high.
 */
static uint64_t supported_samplerates[] = {
	100,
	500,
	KHZ(1),
	KHZ(5),
	KHZ(25),
	KHZ(50),
	KHZ(100),
	KHZ(200),
	KHZ(400),
	KHZ(800),
	MHZ(1),
	MHZ(10),
	MHZ(25),
	MHZ(50),
	MHZ(80),
	MHZ(100),
	MHZ(150),
	MHZ(200),
	0,
};

static struct samplerates samplerates = {
	0, 0, 0,
	supported_samplerates,
};

/* TODO: All of these should go in a device-specific struct. */
static uint64_t cur_samplerate = 0;
static uint64_t limit_samples = 0;
int num_channels = 32; /* TODO: This isn't initialized before it's needed :( */
uint64_t memory_size = 0;
static uint8_t probe_mask = 0;
static uint8_t trigger_mask[NUM_TRIGGER_STAGES] = { 0 };
static uint8_t trigger_value[NUM_TRIGGER_STAGES] = { 0 };

// static uint8_t trigger_buffer[NUM_TRIGGER_STAGES] = { 0 };

static int hw_set_configuration(int device_index, int capability, void *value);

static unsigned int get_memory_size(int type)
{
	if (type == MEMORY_SIZE_8K)
		return 8 * 1024;
	else if (type == MEMORY_SIZE_64K)
		return 64 * 1024;
	else if (type == MEMORY_SIZE_128K)
		return 128 * 1024;
	else if (type == MEMORY_SIZE_512K)
		return 512 * 1024;
	else
		return 0;
}

static int opendev4(struct sigrok_device_instance **sdi, libusb_device *dev,
		    struct libusb_device_descriptor *des)
{
	unsigned int i;
	int err;

	if ((err = libusb_get_device_descriptor(dev, des))) {
		g_warning("failed to get device descriptor: %d", err);
		return -1;
	}

	if (des->idVendor != USB_VENDOR)
		return 0;

	if (libusb_get_bus_number(dev) == (*sdi)->usb->bus
	    && libusb_get_device_address(dev) == (*sdi)->usb->address) {

		for (i = 0; i < ARRAY_SIZE(zeroplus_models); i++) {
			if (!(des->idProduct == zeroplus_models[i].pid))
				continue;

			g_message("Found PID=%04X (%s)", des->idProduct,
				  zeroplus_models[i].model_name);
			num_channels = zeroplus_models[i].channels;
			memory_size = zeroplus_models[i].sample_depth * 1024;
			break;
		}

		if (num_channels == 0) {
			g_warning("Unknown ZeroPlus device %04X",
				  des->idProduct);
			return -2;
		}

		/* Found it. */
		if (!(err = libusb_open(dev, &((*sdi)->usb->devhdl)))) {
			(*sdi)->status = ST_ACTIVE;
			g_message("opened device %d on %d.%d interface %d",
			     (*sdi)->index, (*sdi)->usb->bus,
			     (*sdi)->usb->address, USB_INTERFACE);
		} else {
			g_warning("failed to open device: %d", err);
			*sdi = NULL;
		}
	}

	return 0;
}

struct sigrok_device_instance *zp_open_device(int device_index)
{
	struct sigrok_device_instance *sdi;
	libusb_device **devlist;
	struct libusb_device_descriptor des;
	int err, i;

	if (!(sdi = get_sigrok_device_instance(device_instances, device_index)))
		return NULL;

	libusb_get_device_list(usb_context, &devlist);
	if (sdi->status == ST_INACTIVE) {
		/* Find the device by vendor, product, bus and address. */
		libusb_get_device_list(usb_context, &devlist);
		for (i = 0; devlist[i]; i++) {
			/* TODO: Error handling. */
			err = opendev4(&sdi, devlist[i], &des);
		}
	} else {
		/* Status must be ST_ACTIVE, i.e. already in use... */
		sdi = NULL;
	}
	libusb_free_device_list(devlist, 1);

	if (sdi && sdi->status != ST_ACTIVE)
		sdi = NULL;

	return sdi;
}

static void close_device(struct sigrok_device_instance *sdi)
{
	if (!sdi->usb->devhdl)
		return;

	g_message("closing device %d on %d.%d interface %d", sdi->index,
		  sdi->usb->bus, sdi->usb->address, USB_INTERFACE);
	libusb_release_interface(sdi->usb->devhdl, USB_INTERFACE);
	libusb_close(sdi->usb->devhdl);
	sdi->usb->devhdl = NULL;
	sdi->status = ST_INACTIVE;
}

static int configure_probes(GSList *probes)
{
	struct probe *probe;
	GSList *l;

	probe_mask = 0;
	for (l = probes; l; l = l->next) {
		probe = (struct probe *)l->data;
		if (!probe->enabled)
			continue;
		probe_mask |= 1 << (probe->index - 1);
	}
	return SIGROK_OK;

}

static int configure_triggers(GSList *triggers)
{
	struct trigger *trigger;
	GSList *l;
	int i;

	for (i = 0; i < NUM_TRIGGER_STAGES; i++) {
		trigger_mask[i] = 0;
		trigger_value[i] = 0;
	}

	for (l = triggers; l; l = l->next) {
		trigger = (struct trigger *)l->data;

		switch (trigger->type) {
		case TRIGGER_TYPE_LOGIC:
			if (trigger->logic->n > NUM_TRIGGER_STAGES)
				return SIGROK_ERR;
			for (i = 0; i < trigger->logic->n; i++) {
				trigger_value[i] =
					trigger->logic->value[i];
				trigger_mask[i] =
					trigger->logic->mask[i];
			}
			break;
		default:
			return SIGROK_ERR;
		}
	}

	return SIGROK_OK;
}

/*
 * API callbacks
 */

static int hw_init(char *deviceinfo)
{
	struct sigrok_device_instance *sdi;
	struct libusb_device_descriptor des;
	libusb_device **devlist;
	int err, devcnt, i;

	/* Avoid compiler warnings. */
	deviceinfo = deviceinfo;

	if (libusb_init(&usb_context) != 0) {
		g_warning("Failed to initialize USB.");
		return 0;
	}

	/* Find all ZeroPlus analyzers and add them to device list. */
	devcnt = 0;
	libusb_get_device_list(usb_context, &devlist);

	for (i = 0; devlist[i]; i++) {
		err = libusb_get_device_descriptor(devlist[i], &des);
		if (err != 0) {
			g_warning("failed to get device descriptor: %d", err);
			continue;
		}

		if (des.idVendor == USB_VENDOR) {
			/*
			 * Definitely a Zeroplus.
			 * TODO: Any way to detect specific model/version in
			 * the zeroplus range?
			 */
			sdi = sigrok_device_instance_new(devcnt,
					ST_INACTIVE, USB_VENDOR_NAME,
					USB_MODEL_NAME, USB_MODEL_VERSION);
			if (!sdi)
				return 0;
			device_instances =
			    g_slist_append(device_instances, sdi);
			sdi->usb = usb_device_instance_new(
				libusb_get_bus_number(devlist[i]),
				libusb_get_device_address(devlist[i]), NULL);
			devcnt++;
		}
	}
	libusb_free_device_list(devlist, 1);

	return devcnt;
}

static int hw_opendev(int device_index)
{
	struct sigrok_device_instance *sdi;
	int err;

	if (!(sdi = zp_open_device(device_index))) {
		g_warning("unable to open device");
		return SIGROK_ERR;
	}

	err = libusb_claim_interface(sdi->usb->devhdl, USB_INTERFACE);
	if (err != 0) {
		g_warning("Unable to claim interface: %d", err);
		return SIGROK_ERR;
	}
	analyzer_reset(sdi->usb->devhdl);
	analyzer_initialize(sdi->usb->devhdl);

	analyzer_set_memory_size(MEMORY_SIZE_512K);
	// analyzer_set_freq(g_freq, g_freq_scale);
	analyzer_set_trigger_count(1);
	// analyzer_set_ramsize_trigger_address((((100 - g_pre_trigger)
	// * get_memory_size(g_memory_size)) / 100) >> 2);
	analyzer_set_ramsize_trigger_address(
		(100 * get_memory_size(MEMORY_SIZE_512K) / 100) >> 2);

#if 0
	if (g_double_mode == 1)
		analyzer_set_compression(COMPRESSION_DOUBLE);
	else if (g_compression == 1)
		analyzer_set_compression(COMPRESSION_ENABLE);
	else
#endif
	analyzer_set_compression(COMPRESSION_NONE);

	if (cur_samplerate == 0) {
		/* Samplerate hasn't been set. Default to the slowest one. */
		if (hw_set_configuration(device_index, HWCAP_SAMPLERATE,
		     &samplerates.low) == SIGROK_ERR)
			return SIGROK_ERR;
	}

	return SIGROK_OK;
}

static void hw_closedev(int device_index)
{
	struct sigrok_device_instance *sdi;

	if ((sdi = get_sigrok_device_instance(device_instances, device_index)))
		close_device(sdi);
}

static void hw_cleanup(void)
{
	GSList *l;

	/* Properly close all devices... */
	for (l = device_instances; l; l = l->next)
		close_device((struct sigrok_device_instance *)l->data);

	/* ...and free all their memory. */
	for (l = device_instances; l; l = l->next)
		g_free(l->data);
	g_slist_free(device_instances);
	device_instances = NULL;

	if (usb_context)
		libusb_exit(usb_context);
	usb_context = NULL;
}

static void *hw_get_device_info(int device_index, int device_info_id)
{
	struct sigrok_device_instance *sdi;
	void *info = NULL;

	if (!(sdi = get_sigrok_device_instance(device_instances, device_index)))
		return NULL;

	switch (device_info_id) {
	case DI_INSTANCE:
		info = sdi;
		break;
	case DI_NUM_PROBES:
		info = GINT_TO_POINTER(num_channels);
		break;
	case DI_SAMPLERATES:
		info = &samplerates;
		break;
	case DI_TRIGGER_TYPES:
		info = &trigger_types;
		break;
	case DI_CUR_SAMPLERATE:
		info = &cur_samplerate;
		break;
	}

	return info;
}

static int hw_get_status(int device_index)
{
	struct sigrok_device_instance *sdi;

	sdi = get_sigrok_device_instance(device_instances, device_index);
	if (sdi)
		return sdi->status;
	else
		return ST_NOT_FOUND;
}

static int *hw_get_capabilities(void)
{
	return capabilities;
}

/* TODO: This will set the same samplerate for all devices. */
static int set_configuration_samplerate(uint64_t samplerate)
{
	g_message("%s(%" PRIu64 ")", __FUNCTION__, samplerate);
	if (samplerate > MHZ(1))
		analyzer_set_freq(samplerate / MHZ(1), FREQ_SCALE_MHZ);
	else if (samplerate > KHZ(1))
		analyzer_set_freq(samplerate / KHZ(1), FREQ_SCALE_KHZ);
	else
		analyzer_set_freq(samplerate, FREQ_SCALE_HZ);

	cur_samplerate = samplerate;

	return SIGROK_OK;
}

static int hw_set_configuration(int device_index, int capability, void *value)
{
	struct sigrok_device_instance *sdi;
	uint64_t *tmp_u64;

	if (!(sdi = get_sigrok_device_instance(device_instances, device_index)))
		return SIGROK_ERR;

	switch (capability) {
	case HWCAP_SAMPLERATE:
		tmp_u64 = value;
		return set_configuration_samplerate(*tmp_u64);
	case HWCAP_PROBECONFIG:
		return configure_probes((GSList *) value);
	case HWCAP_TRIGGERCONFIG:
		return configure_triggers((GSList *) value);
	case HWCAP_LIMIT_SAMPLES:
		tmp_u64 = value;
		limit_samples = *tmp_u64;
		return SIGROK_OK;
	default:
		return SIGROK_ERR;
	}
}

static int hw_start_acquisition(int device_index, gpointer session_device_id)
{
	struct sigrok_device_instance *sdi;
	struct datafeed_packet packet;
	struct datafeed_header header;
	int res;
	unsigned int packet_num;
	unsigned char *buf;

	if (!(sdi = get_sigrok_device_instance(device_instances, device_index)))
		return SIGROK_ERR;

	/* push configured settings to device */
	analyzer_configure(sdi->usb->devhdl);

	analyzer_start(sdi->usb->devhdl);
	g_message("Waiting for data");
	analyzer_wait_data(sdi->usb->devhdl);

	g_message("Stop address    = 0x%x",
		  analyzer_get_stop_address(sdi->usb->devhdl));
	g_message("Now address     = 0x%x",
		  analyzer_get_now_address(sdi->usb->devhdl));
	g_message("Trigger address = 0x%x",
		  analyzer_get_trigger_address(sdi->usb->devhdl));

	packet.type = DF_HEADER;
	packet.length = sizeof(struct datafeed_header);
	packet.payload = (unsigned char *)&header;
	header.feed_version = 1;
	gettimeofday(&header.starttime, NULL);
	header.samplerate = cur_samplerate;
	header.protocol_id = PROTO_RAW;
	header.num_logic_probes = num_channels;
	header.num_analog_probes = 0;
	session_bus(session_device_id, &packet);

	buf = g_malloc(PACKET_SIZE);
	if (!buf)
		return SIGROK_ERR;
	analyzer_read_start(sdi->usb->devhdl);
	/* Send the incoming transfer to the session bus. */
	for (packet_num = 0; packet_num < (memory_size * 4 / PACKET_SIZE);
	     packet_num++) {
		res = analyzer_read_data(sdi->usb->devhdl, buf, PACKET_SIZE);
#if 0
		g_message("Tried to read %llx bytes, actually read %x bytes",
			  PACKET_SIZE, res);
#endif

		packet.type = DF_LOGIC;
		packet.length = PACKET_SIZE;
		packet.unitsize = 4;
		packet.payload = buf;
		session_bus(session_device_id, &packet);
	}
	analyzer_read_stop(sdi->usb->devhdl);
	g_free(buf);

	packet.type = DF_END;
	session_bus(session_device_id, &packet);

	return SIGROK_OK;
}

/* This stops acquisition on ALL devices, ignoring device_index. */
static void hw_stop_acquisition(int device_index, gpointer session_device_id)
{
	struct datafeed_packet packet;
	struct sigrok_device_instance *sdi;

	packet.type = DF_END;
	session_bus(session_device_id, &packet);

	if (!(sdi = get_sigrok_device_instance(device_instances, device_index)))
		return; /* TODO: Cry? */

	analyzer_reset(sdi->usb->devhdl);
	/* TODO: Need to cancel and free any queued up transfers. */
}

struct device_plugin zeroplus_logic_cube_plugin_info = {
	"zeroplus-logic-cube",
	1,
	hw_init,
	hw_cleanup,
	hw_opendev,
	hw_closedev,
	hw_get_device_info,
	hw_get_status,
	hw_get_capabilities,
	hw_set_configuration,
	hw_start_acquisition,
	hw_stop_acquisition,
};
