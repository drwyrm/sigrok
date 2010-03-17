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

#include "config.h"
#include "sigrok.h"
#include "hwcommon.h"
#include "hwplugin.h"
#include "session.h"

#include <libusb.h>
#include <glib.h>

#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/time.h>

#define USB_VENDOR				0x0925
#define USB_PRODUCT			0x3881
#define USB_INTERFACE			0
#define USB_CONFIGURATION		1
#define NUM_PROBES				8
#define NUM_TRIGGER_STAGES		4
#define TRIGGER_TYPES			"01"
#define FIRMWARE FIRMWARE_DIR	"/saleae-logic.firmware"
/* delay in ms */
#define FIRMWARE_RENUM_DELAY	3000
#define NUM_SIMUL_TRANSFERS	10

/* software trigger implementation: positive values indicate trigger stage */
#define TRIGGER_FIRED			-1


extern GMainContext *gmaincontext;


/* there is only one model Saleae Logic, and this is what it supports */
int capabilities[] = {
	HWCAP_LOGIC_ANALYZER,
	HWCAP_SAMPLERATE,

	/* these are really implemented in the driver, not the hardware */
	HWCAP_LIMIT_SECONDS,
	HWCAP_LIMIT_SAMPLES,
	0
};

/* list of struct usb_device_instance, maintained by opendev() and closedev() */
GSList *usb_devices = NULL;

/* since we can't keep track of a Saleae Logic device after upgrading the
 * firmware -- it re-enumerates into a different device address after the
 * upgrade -- this is like a global lock. No device will open until a proper
 * delay after the last device was upgraded.
 */
GTimeVal firmware_updated = {0};

libusb_context *usb_context = NULL;

float supported_sample_rates[] = {
	0.2,
	0.25,
	0.50,
	1,
	2,
	4,
	8,
	12,
	16,
	24,
	0
};

/* TODO: all of these should go in a device-specific struct */
float cur_sample_rate = 0;
int limit_seconds = 0;
int limit_samples = 0;
uint8_t probe_mask = 0, \
		trigger_mask[NUM_TRIGGER_STAGES] = {0}, \
		trigger_value[NUM_TRIGGER_STAGES] = {0}, \
		trigger_buffer[NUM_TRIGGER_STAGES] = {0};;
int trigger_stage = TRIGGER_FIRED;



/* returns 1 if the device's configuration profile match the Logic firmware's
 * configuration, 0 otherwise
 */
int check_conf_profile(libusb_device *dev)
{
	struct libusb_device_descriptor des;
	struct libusb_config_descriptor *conf_dsc;
	const struct libusb_interface_descriptor *intf_dsc;
	int ret;

	ret = -1;
	conf_dsc = NULL;
	while(ret == -1)
	{
		/* assume it's not a Saleae Logic unless proven wrong */
		ret = 0;

		if(libusb_get_device_descriptor(dev, &des) != 0)
			break;

		if(des.bNumConfigurations != 1)
			/* need exactly 1 configuration */
			break;

		if(libusb_get_config_descriptor(dev, 0, &conf_dsc) != 0)
			break;

		if(conf_dsc->bNumInterfaces != 1)
			/* need exactly 1 interface */
			break;

		if(conf_dsc->interface[0].num_altsetting != 1)
			/* need just one alternate setting */
			break;

		intf_dsc = &(conf_dsc->interface[0].altsetting[0]);
		if(intf_dsc->bNumEndpoints != 2)
			/* need 2 endpoints */
			break;

		if((intf_dsc->endpoint[0].bEndpointAddress & 0x8f) != (1 | LIBUSB_ENDPOINT_OUT))
			/* first endpoint should be 1 (outbound) */
			break;

		if((intf_dsc->endpoint[1].bEndpointAddress & 0x8f) != (2 | LIBUSB_ENDPOINT_IN))
			/* first endpoint should be 2 (inbound) */
			break;

		/* if we made it here, it must be a Saleae Logic */
		ret = 1;
	}
	if(conf_dsc)
		libusb_free_config_descriptor(conf_dsc);

	return ret;
}


struct usb_device_instance *sl_open_device(int device_index)
{
	struct usb_device_instance *udi;
	libusb_device **devlist;
	struct libusb_device_descriptor des;
	int err, skip, i;

	if(!(udi = get_usb_device_instance(usb_devices, device_index)))
		return NULL;

	libusb_get_device_list(usb_context, &devlist);
	if(udi->status == ST_INITIALIZING)
	{
		/* this device was renumerating last time we touched it. opendev() guarantees we've
		 * waited long enough for it to have booted properly, so now we need to find it on
		 * the bus and record its new address.
		 */
		skip = 0;
		for(i = 0; devlist[i]; i++)
		{
			if( (err = libusb_get_device_descriptor(devlist[i], &des)) )
			{
				g_warning("failed to get device descriptor: %d", err);
				continue;
			}

			if(des.idVendor == USB_VENDOR && des.idProduct == USB_PRODUCT)
			{
				if(skip != device_index)
				{
					/* skip past devices of this type that aren't the one we want */
					skip++;
					continue;
				}

				/* should check the bus here, since we know that already... but what
				 * are we going to do if it doesn't match after the right number of skips?
				 */

				if( !(err = libusb_open(devlist[i], &(udi->devhdl))) )
				{
					udi->address = libusb_get_device_address(devlist[i]);
					udi->status = ST_ACTIVE;
					g_message("opened device %d on %d.%d interface %d", udi->index, udi->bus,
							udi->address, USB_INTERFACE);
				}
				else
				{
					g_warning("failed to open device: %d", err);
					udi = NULL;
				}
			}
		}
	}
	else if(udi->status == ST_INACTIVE)
	{
		/* this device is fully enumerated, so we need to find this device by
		 * vendor, product, bus and address */
		libusb_get_device_list(usb_context, &devlist);
		for(i = 0; devlist[i]; i++)
		{
			if( (err = libusb_get_device_descriptor(devlist[i], &des)) )
			{
				g_warning("failed to get device descriptor: %d", err);
				continue;
			}

			if(des.idVendor == USB_VENDOR && des.idProduct == USB_PRODUCT)
			{
				if(libusb_get_bus_number(devlist[i]) == udi->bus &&
						libusb_get_device_address(devlist[i]) == udi->address)
				{
					/* found it */
					if( !(err = libusb_open(devlist[i], &(udi->devhdl))) )
					{
						udi->status = ST_ACTIVE;
						g_message("opened device %d on %d.%d interface %d", udi->index, udi->bus,
								udi->address, USB_INTERFACE);
					}
					else
					{
						g_warning("failed to open device: %d", err);
						udi = NULL;
					}
				}
			}
		}
	}
	else
	{
		/* status must be ST_ACTIVE, i.e. already in use... */
		udi = NULL;
	}
	libusb_free_device_list(devlist, 1);

	if(udi && udi->status != ST_ACTIVE)
		udi = NULL;

	return udi;
}


int upload_firmware(libusb_device *dev)
{
	struct libusb_device_handle *hdl;
	int err;

	g_message("uploading firmware to device on %d.%d",
			libusb_get_bus_number(dev), libusb_get_device_address(dev));

	err = libusb_open(dev, &hdl);
	if(err != 0)
	{
		g_warning("failed to open device: %d", err);
		return 1;
	}

	err = libusb_set_configuration(hdl, USB_CONFIGURATION);
	if(err != 0)
	{
		g_warning("Unable to set configuration: %d", err);
		return 1;
	}

	if((ezusb_reset(hdl, 1)) < 0)
		return 1;

	if(ezusb_install_firmware(hdl, FIRMWARE) != 0)
		return 1;

	if((ezusb_reset(hdl, 0)) < 0)
		return 1;

	libusb_close(hdl);

	/* remember when the last firmware update was done */
	g_get_current_time(&firmware_updated);

	return 0;
}


void close_device(struct usb_device_instance *udi)
{

	if(udi->devhdl)
	{
		g_message("closing device %d on %d.%d interface %d", udi->index, udi->bus,
				udi->address, USB_INTERFACE);
		libusb_release_interface(udi->devhdl, USB_INTERFACE);
		libusb_close(udi->devhdl);
		udi->devhdl = NULL;
		udi->status = ST_INACTIVE;
	}

}


int configure_probes(GSList *probes)
{
	struct probe *probe;
	GSList *l;
	int probe_bit, stage, i;
	char *tc;

	probe_mask = 0;
	for(i = 0; i < NUM_TRIGGER_STAGES; i++)
	{
		trigger_mask[i] = 0;
		trigger_value[i] = 0;
	}

	stage = -1;
	for(l = probes; l; l = l->next)
	{
		probe = (struct probe *) l->data;
		if(probe->enabled == FALSE)
			continue;
		probe_bit = 1 << (probe->index - 1);
		probe_mask |= probe_bit;
		if(probe->trigger)
		{
			stage = 0;
			for(tc = probe->trigger; *tc; tc++)
			{
				trigger_mask[stage] |= probe_bit;
				if(*tc == '1')
					trigger_value[stage] |= probe_bit;
				stage++;
				if(stage > NUM_TRIGGER_STAGES)
					return SIGROK_NOK;
			}
		}
	}

	if(stage == -1)
		/* we didn't configure any triggers, make sure acquisition doesn't wait for any */
		trigger_stage = TRIGGER_FIRED;
	else
		trigger_stage = 0;

	return SIGROK_OK;
}



/*
 * API callbacks
 */

int hw_init(char *deviceinfo)
{
	struct usb_device_instance *udi;
	struct libusb_device_descriptor des;
	libusb_device **devlist;
	int err, devcnt, i;

	if(libusb_init(&usb_context) != 0)
	{
		g_warning("Failed to initialize USB.");
		return 0;
	}
	libusb_set_debug(usb_context, 3);

	/* find all Saleae Logic devices and upload firmware to all of them */
	devcnt = 0;
	libusb_get_device_list(usb_context, &devlist);
	for(i = 0; devlist[i]; i++)
	{
		err = libusb_get_device_descriptor(devlist[i], &des);
		if(err != 0)
		{
			g_warning("failed to get device descriptor: %d", err);
			continue;
		}
		if(des.idVendor == USB_VENDOR && des.idProduct == USB_PRODUCT)
		{
			/* definitely a Saleae Logic */
			if(check_conf_profile(devlist[i]) == 0)
			{
				if(upload_firmware(devlist[i]) > 0)
					g_warning("firmware upload failed for device %d", devcnt);
				udi = usb_device_instance_new(devcnt, ST_INITIALIZING,
						libusb_get_bus_number(devlist[i]), 0, NULL);
				usb_devices = g_slist_append(usb_devices, udi);
			}
			else
			{
				/* already has the firmware on it, so fix the address */
				udi = usb_device_instance_new(devcnt, ST_INACTIVE,
						libusb_get_bus_number(devlist[i]),
						libusb_get_device_address(devlist[i]), NULL);
				usb_devices = g_slist_append(usb_devices, udi);
			}
			devcnt++;
		}
	}
	libusb_free_device_list(devlist, 1);

	return devcnt;
}


int hw_opendev(int device_index)
{
	GTimeVal cur_time;
	struct usb_device_instance *udi;
	int timediff, err;
	unsigned int cur, upd;

	if(firmware_updated.tv_sec > 0)
	{
		/* firmware was recently uploaded */
		g_get_current_time(&cur_time);
		cur = cur_time.tv_sec * 1000 + cur_time.tv_usec / 1000;
		upd = firmware_updated.tv_sec * 1000 + firmware_updated.tv_usec / 1000;
		timediff = cur - upd;
		if(timediff < FIRMWARE_RENUM_DELAY)
		{
			timediff = FIRMWARE_RENUM_DELAY - timediff;
			g_message("waiting %d ms for device to reset", timediff);
			g_usleep(timediff * 1000);
			firmware_updated.tv_sec = 0;
		}
	}

	if( !(udi = sl_open_device(device_index)) )
	{
		g_warning("unable to open device");
		return SIGROK_NOK;
	}

	err = libusb_claim_interface(udi->devhdl, USB_INTERFACE);
	if(err != 0)
	{
		g_warning("Unable to claim interface: %d", err);
		return SIGROK_NOK;
	}

	return SIGROK_OK;
}


void hw_closedev(int device_index)
{
	struct usb_device_instance *udi;

	if( (udi = get_usb_device_instance(usb_devices, device_index)) )
		close_device(udi);

}


void hw_cleanup(void)
{
	GSList *l;

	/* properly close all devices */
	for(l = usb_devices; l; l = l->next)
		close_device( (struct usb_device_instance *) l->data);

	/* and free all their memory */
	for(l = usb_devices; l; l = l->next)
		g_free(l->data);
	g_slist_free(usb_devices);
	usb_devices = NULL;

	if(usb_context)
		libusb_exit(usb_context);
	usb_context = NULL;

}


char *hw_get_device_info(int device_index, int device_info_id)
{
	char *info;

	info = NULL;
	switch(device_info_id)
	{
	case DI_IDENTIFIER:
		info = g_malloc(16);
		snprintf(info, 16, "unit %d", device_index);
		break;
	case DI_NUM_PROBES:
		info = GINT_TO_POINTER(NUM_PROBES);
		break;
	case DI_SAMPLE_RATES:
		info = (char *) supported_sample_rates;
	case DI_TRIGGER_TYPES:
		info = (char *) TRIGGER_TYPES;
		break;
	}

	return info;
}


int hw_get_status(int device_index)
{
	struct usb_device_instance *udi;

	udi = get_usb_device_instance(usb_devices, device_index);
	if(udi)
		return udi->status;
	else
		return ST_NOT_FOUND;
}


int *hw_get_capabilities(void)
{

	return capabilities;
}


int set_configuration_samplerate(struct usb_device_instance *udi, float rate)
{
	uint8_t divider;
	int ret, result, i;
	unsigned char buf[2];

	for(i = 0; supported_sample_rates[i]; i++)
	{
		if(supported_sample_rates[i] == rate)
			break;
	}
	if(supported_sample_rates[i] == 0)
		return SIGROK_ERR_BADVALUE;

	divider = (uint8_t) (48 / rate) - 1;

	g_message("setting sample rate to %.3f Mhz (divider %d)", rate, divider);
	buf[0] = 0x01;
	buf[1] = divider;
	ret = libusb_bulk_transfer(udi->devhdl, 1 | LIBUSB_ENDPOINT_OUT, buf, 2, &result, 500);
	if(ret != 0)
	{
		g_warning("failed to set rate: %d", ret);
		return SIGROK_NOK;
	}
	cur_sample_rate = rate;

	return SIGROK_OK;
}


int hw_set_configuration(int device_index, int capability, char *value)
{
	struct usb_device_instance *udi;
	int ret;

	if( !(udi = get_usb_device_instance(usb_devices, device_index)) )
		return SIGROK_NOK;

	if(capability == HWCAP_SAMPLERATE)
		ret = set_configuration_samplerate(udi, atof(value));
	else if(capability == HWCAP_PROBECONFIG)
		ret = configure_probes( (GSList *) value);
	else if(capability == HWCAP_LIMIT_SECONDS)
	{
		limit_seconds = atoi(value);
		ret = SIGROK_OK;
	}
	else if(capability == HWCAP_LIMIT_SAMPLES)
	{
		limit_samples = atoi(value);
		ret = SIGROK_OK;
	}
	else
		ret = SIGROK_NOK;

	return ret;
}


int receive_data(GSource *source, gpointer data)
{
	struct timeval tv;

	tv.tv_sec = tv.tv_usec = 0;
	libusb_handle_events_timeout(usb_context, &tv);

	return TRUE;
}


void receive_transfer(struct libusb_transfer *transfer)
{
	static int num_samples = 0;
	struct datafeed_packet packet;
	void *user_data;
	int cur_buflen, trigger_offset, i;
	unsigned char *cur_buf, *new_buf;

	if(transfer == NULL)
	{
		/* hw_stop_acquisition() telling us to stop */
		num_samples = -1;
	}

	if(num_samples == -1)
	{
		/* acquisition has already ended, just free any queued up transfer that come in */
		libusb_free_transfer(transfer);
	}
	else
	{
		g_message("receive_transfer(): status %d received %d bytes", transfer->status, transfer->actual_length);

		/* save the incoming transfer before reusing the transfer struct */
		cur_buf = transfer->buffer;
		cur_buflen = transfer->actual_length;
		user_data = transfer->user_data;

		/* fire off a new request */
		new_buf = g_malloc(4096);
		transfer->buffer = new_buf;
		transfer->length = 4096;
		if(libusb_submit_transfer(transfer) != 0)
		{
			/* TODO: stop session? */
			g_warning("eek");
		}

		trigger_offset = 0;
		if(trigger_stage >= 0)
		{
			for(i = 0; i < cur_buflen; i++)
			{
				if((cur_buf[i] & trigger_mask[trigger_stage]) == trigger_value[trigger_stage])
				{
					/* match on this trigger stage */
					trigger_buffer[trigger_stage] = cur_buf[i];
					trigger_stage++;
					if(trigger_stage == NUM_TRIGGER_STAGES || trigger_mask[trigger_stage] == 0)
					{
						trigger_offset = i+1;
						/* match on all trigger stages, we're done */
						trigger_stage = TRIGGER_FIRED;

						/* TODO: send pre-trigger buffer to session bus */

						/* tell the frontend we hit the trigger here */
						packet.type = DF_TRIGGER;
						packet.length = 0;
						session_bus(user_data, &packet);

						/* send the samples that triggered it, since we're skipping past them */
						packet.type = DF_LOGIC8;
						packet.length = trigger_stage;
						packet.payload = trigger_buffer;
						session_bus(user_data, &packet);
						break;
					}
				}
				else if(trigger_stage > 0)
				{
					/* we had a match before, but not in the next sample. however, we may
					 * have a match on this stage in the next bit -- trigger on 0001 will
					 * fail on seeing 00001, so we need to go back to stage 0 -- but at
					 * the next sample from the one that matched originally, which the
					 * counter increment at the end of the loop takes care of.
					 */
					i -= trigger_stage;
					if(i < -1)
						/* oops, went back past this buffer */
						i = -1;
					/* reset trigger stage */
					trigger_stage = 0;
				}
			}
		}

		if(trigger_stage == TRIGGER_FIRED)
		{
			/* send the incoming transfer to the session bus */
			packet.type = DF_LOGIC8;
			packet.length = cur_buflen - trigger_offset;
			packet.payload = cur_buf + trigger_offset;
			session_bus(user_data, &packet);
			g_free(cur_buf);

			num_samples += cur_buflen;
			if(num_samples > limit_samples)
			{
				/* end the acquisition */
				packet.type = DF_END;
				session_bus(user_data, &packet);
				num_samples = -1;
			}
		}
		else
		{
			/* TODO: buffer pre-trigger data in capture ratio-sized buffer */

		}
	}

}


int hw_start_acquisition(int device_index, gpointer session_device_id)
{
	struct usb_device_instance *udi;
	struct datafeed_packet *packet;
	struct datafeed_header *header;
	struct libusb_transfer *transfer;
	const struct libusb_pollfd **lupfd;
	int size, i;
	unsigned char *buf;
	char tmp[16];

	if( !(udi = get_usb_device_instance(usb_devices, device_index)))
		return SIGROK_NOK;

	if(cur_sample_rate == 0)
	{
		/* sample rate hasn't been set; default to the slowest it has */
		snprintf(tmp, 15, "%15f", supported_sample_rates[0]);
		if(hw_set_configuration(device_index, HWCAP_SAMPLERATE, tmp) == SIGROK_NOK)
			return SIGROK_NOK;
	}

	packet = g_malloc(sizeof(struct datafeed_packet));
	header = g_malloc(sizeof(struct datafeed_header));
	if(!packet || !header)
		return SIGROK_NOK;

	if(limit_samples == 0 && limit_seconds > 0)
		/* no need to bother with a timer, just convert to samples instead */
		limit_samples = cur_sample_rate * 1000000 * limit_seconds;

	/* start with 2K transfer, subsequently increased to 4K */
	size = 2048;
	for(i = 0; i < NUM_SIMUL_TRANSFERS; i++)
	{
		buf = g_malloc(size);
		transfer = libusb_alloc_transfer(0);
		libusb_fill_bulk_transfer(transfer, udi->devhdl, 2 | LIBUSB_ENDPOINT_IN, buf, size,
				receive_transfer, session_device_id, 40);
		if(libusb_submit_transfer(transfer) != 0)
		{
			/* TODO: free them all */
			libusb_free_transfer(transfer);
			g_free(buf);
			return SIGROK_NOK;
		}
		size = 4096;
	}

	lupfd = libusb_get_pollfds(usb_context);
	for(i = 0; lupfd[i]; i++)
		add_source_fd(lupfd[i]->fd, lupfd[i]->events, receive_data, NULL);
	free(lupfd);

	packet->type = DF_HEADER;
	packet->length = sizeof(struct datafeed_header);
	packet->payload = (unsigned char *) header;
	header->feed_version = 1;
	gettimeofday(&header->starttime, NULL);
	header->rate = cur_sample_rate;
	header->protocol_id = PROTO_RAW;
	header->num_probes = NUM_PROBES;
	session_bus(session_device_id, packet);
	g_free(header);
	g_free(packet);

	return SIGROK_OK;
}


/* this stops acquisition on ALL devices, ignoring device_index */
void hw_stop_acquisition(int device_index, gpointer session_device_id)
{
	struct datafeed_packet packet;

	packet.type = DF_END;
	session_bus(session_device_id, &packet);

	receive_transfer(NULL);

	/* TODO: need to cancel and free any queued up transfers */

}



struct device_plugin plugin_info = {
	"saleae-logic",
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
	hw_stop_acquisition
};

