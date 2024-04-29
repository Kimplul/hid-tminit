// SPDX-License-Identifier: GPL-2.0
/*
 * When connected to the machine, the Thrustmaster wheels appear as
 * a «generic» USB device "Thrustmaster FFB Wheel".
 *
 * When in this mode not every functionality of the wheel, like the force feedback,
 * are available. To enable all functionalities of a Thrustmaster wheel we have to send
 * to it a specific USB CONTROL request with a code different for each wheel.
 *
 * This driver tries to understand which model of Thrustmaster wheel the generic
 * "Thrustmaster FFB Wheel" really is and then sends the appropriate control code.
 *
 * Copyright (c) 2020-2021 Dario Pagani <dario.pagani.146+linuxk@gmail.com>
 * Copyright (c) 2020-2024 Kim Kuparinen <kimi.h.kuparinen@gmail.com>
 */
#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/module.h>
#include "tminit.h"

/*
 * These interrupts are used to prevent a nasty crash when initializing the
 * T300RS. Used in thrustmaster_interrupts().
 */
static const u8 setup_0[] = { 0x42, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const u8 setup_1[] = { 0x0a, 0x04, 0x90, 0x03, 0x00, 0x00, 0x00, 0x00 };
static const u8 setup_2[] = { 0x0a, 0x04, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00 };
static const u8 setup_3[] = { 0x0a, 0x04, 0x12, 0x10, 0x00, 0x00, 0x00, 0x00 };
static const u8 setup_4[] = { 0x0a, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00 };
static const u8 *const setup_arr[] = { setup_0, setup_1, setup_2, setup_3, setup_4 };
static const unsigned int setup_arr_sizes[] = {
	ARRAY_SIZE(setup_0),
	ARRAY_SIZE(setup_1),
	ARRAY_SIZE(setup_2),
	ARRAY_SIZE(setup_3),
	ARRAY_SIZE(setup_4)
};
/*
 * This struct contains for each type of
 * Thrustmaster wheel
 *
 * Note: The values are stored in the CPU
 * endianness, the USB protocols always use
 * little endian; the macro cpu_to_le[BIT]()
 * must be used when preparing USB packets
 * and vice-versa
 */
struct tm_wheel_info {
	uint8_t model;
	uint8_t attachment;

	/**
	 * See when the USB control out packet is prepared...
	 * @TODO The TMX seems to require multiple control codes to switch.
	 */
	uint16_t switch_value;

	char const *const wheel_name;
};

/*
 * Known wheels.
 * Note: TMX does not work as it requires 2 control packets
 */
static const struct tm_wheel_info tm_wheels_infos[] = {
	{0x00, 0x02, 0x0002, "Thrustmaster T500RS"},
	{0x02, 0x00, 0x0005, "Thrustmaster T300RS (Missing Attachment)"},
	{0x02, 0x03, 0x0005, "Thrustmaster T300RS (F1 attachment)"},
	{0x02, 0x04, 0x0005, "Thrustmaster T300 Ferrari Alcantara Edition"},
	{0x02, 0x06, 0x0005, "Thrustmaster T300RS"},
	{0x02, 0x09, 0x0005, "Thrustmaster T300RS (Open Wheel Attachment)"},
	{0x03, 0x06, 0x0006, "Thrustmaster T150RS"},
	{0x00, 0x09, 0x000b, "Thrustmaster T128"},
	{0x07, 0x0a, 0x000a, "Thrustmaster TS-XW"}
	//{0x04, 0x07, 0x0001, "Thrustmaster TMX"}
};

static const uint8_t tm_wheels_infos_length = 9;


/* The control packet to send to wheel */
static const struct usb_ctrlrequest model_request = {
	.bRequestType = 0xc1,
	.bRequest = 73,
	.wValue = 0,
	.wIndex = 0,
	.wLength = cpu_to_le16(0x0010)
};

static const struct usb_ctrlrequest change_request = {
	.bRequestType = 0x41,
	.bRequest = 83,
	.wValue = 0, // Will be filled by the driver
	.wIndex = 0,
	.wLength = 0
};

/*
 * On some setups initializing the T300RS crashes the kernel,
 * these interrupts fix that particular issue. So far they haven't caused any
 * adverse effects in other wheels.
 */
static void thrustmaster_interrupts(struct usb_device *udev, struct usb_interface *interface)
{
	int ret, trans, i, b_ep;
	u8 *send_buf = kmalloc(256, GFP_KERNEL);
	struct usb_host_endpoint *ep;

	if (!send_buf) {
		dev_err(&interface->dev, "failed allocating send buffer\n");
		return;
	}

	if (interface->cur_altsetting->desc.bNumEndpoints < 2) {
		kfree(send_buf);
		dev_err(&interface->dev, "Wrong number of endpoints?\n");
		return;
	}

	ep = &interface->cur_altsetting->endpoint[1];
	b_ep = ep->desc.bEndpointAddress;

	for (i = 0; i < ARRAY_SIZE(setup_arr); ++i) {
		memcpy(send_buf, setup_arr[i], setup_arr_sizes[i]);

		ret = usb_interrupt_msg(udev,
			usb_sndintpipe(udev, b_ep),
			send_buf,
			setup_arr_sizes[i],
			&trans,
			USB_CTRL_SET_TIMEOUT);

		if (ret) {
			dev_err(&interface->dev, "setup data couldn't be sent\n");
			kfree(send_buf);
			return;
		}
	}

	kfree(send_buf);
}

static void thrustmaster_change_handler(struct urb *urb)
{
	struct tm_wheel *tm_wheel = urb->context;
	struct usb_interface *interface = tm_wheel->interface;

	// The wheel seems to kill itself before answering the host and therefore
	// is violating the USB protocol...
	if (urb->status == 0 || urb->status == -EPROTO || urb->status == -EPIPE)
		dev_info(&interface->dev,
				"Success, the wheel should have been initialized!\n");
	else
		dev_warn(&interface->dev,
				"URB to change wheel mode seems to have failed,"
				" error code %d\n", urb->status);
}

static int thrustmaster_submit_change(struct tm_wheel *tm_wheel, uint16_t switch_value)
{
	tm_wheel->change_request->wValue = cpu_to_le16(switch_value);
	usb_fill_control_urb(
		tm_wheel->urb,
		tm_wheel->usb_dev,
		usb_sndctrlpipe(tm_wheel->usb_dev, 0),
		(char *)tm_wheel->change_request,
		NULL, 0, // We do not expect any response from the wheel
		thrustmaster_change_handler,
		tm_wheel
	);

	int ret = usb_submit_urb(tm_wheel->urb, GFP_ATOMIC);
	if (ret)
		dev_err(&tm_wheel->interface->dev,
				"Error %d while submitting the change URB."
				" Unable to initialize this wheel.\n",
				ret);

	return ret;
}

/*
 * Called by the USB subsystem when the wheel responses to our request
 * to get [what it seems to be] the wheel's model.
 *
 * If the model id is recognized then we send an opportune USB CONTROL REQUEST
 * to switch the wheel to its full capabilities
 */
static void thrustmaster_model_handler(struct urb *urb)
{
	uint8_t model = 0;
	uint8_t attachment = 0;
	uint8_t attachment_found;
	int i;
	const struct tm_wheel_info *twi = NULL;
	struct tm_wheel *tm_wheel = urb->context;
	struct usb_interface *interface = tm_wheel->interface;

	if (urb->status) {
		dev_err(&interface->dev, "URB to get model id failed with error %d\n", urb->status);
		return;
	}

	if (tm_wheel->response->type == cpu_to_le16(0x49)) {
		model = tm_wheel->response->data.a.model;
		attachment = tm_wheel->response->data.a.attachment;
	} else if (tm_wheel->response->type == cpu_to_le16(0x47)) {
		model = tm_wheel->response->data.b.model;
		attachment = tm_wheel->response->data.b.attachment;
	} else {
		dev_err(&interface->dev,
				"Unknown packet type 0x%x, unable to proceed further\n",
				tm_wheel->response->type);
		return;
	}
	
	for (i = 0; i < tm_wheels_infos_length && !twi; i++)
		if (tm_wheels_infos[i].model == model)
			twi = tm_wheels_infos + i;

	if (twi) {
		// Trying to find the best attachment
		for (attachment_found = twi->attachment == attachment; !attachment_found && i < tm_wheels_infos_length && tm_wheels_infos[i].model == model; i++)
			if (tm_wheels_infos[i].attachment == attachment) {
				twi = tm_wheels_infos + i;
				attachment_found = 1;
			}

		dev_info(&interface->dev,
				"Wheel with (model, attachment) = (0x%x, 0x%x) is a %s."
				" attachment_found=%u\n",
				model, attachment, twi->wheel_name, attachment_found);
	} else {
		dev_err(&interface->dev, "Unknown wheel's model id 0x%x,"
				" unable to proceed further\n", model);
		return;
	}

	thrustmaster_submit_change(tm_wheel, twi->switch_value);
}

void thrustmaster_disconnect(struct tm_wheel *tm_wheel)
{
	usb_kill_urb(tm_wheel->urb);

	kfree(tm_wheel->change_request);
	kfree(tm_wheel->response);
	kfree(tm_wheel->model_request);
	usb_free_urb(tm_wheel->urb);
	kfree(tm_wheel);
}

/*
 * Function called by USB when a usb Thrustmaster FFB wheel is connected to the host.
 * This function tries to allocate the tm_wheel data structure and
 * finally send an USB CONTROL REQUEST to the wheel to get [what it seems to be] its
 * model type.
 */
int thrustmaster_probe(struct tm_wheel *tm_wheel, struct usb_interface *interface)
{
	int ret = 0;
	struct usb_device *udev = NULL;

	udev = usb_get_dev(interface_to_usbdev(interface));


	tm_wheel->urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!tm_wheel->urb) {
		ret = -ENOMEM;
		goto error2;
	}

	tm_wheel->model_request = kmemdup(&model_request,
					  sizeof(struct usb_ctrlrequest),
					  GFP_KERNEL);
	if (!tm_wheel->model_request) {
		ret = -ENOMEM;
		goto error3;
	}

	tm_wheel->response = kzalloc(sizeof(struct tm_wheel_response), GFP_KERNEL);
	if (!tm_wheel->response) {
		ret = -ENOMEM;
		goto error4;
	}

	tm_wheel->change_request = kmemdup(&change_request,
					   sizeof(struct usb_ctrlrequest),
					   GFP_KERNEL);
	if (!tm_wheel->change_request) {
		ret = -ENOMEM;
		goto error5;
	}

	tm_wheel->usb_dev = udev;
	tm_wheel->interface = usb_get_intf(interface);

	switch (le16_to_cpu(udev->descriptor.idProduct)) {
	case 0xb69c:
		/* T128 resets itself for whatever reason, try to
		 * circumvent it. Ugly magic constant, should probably add a
		 * define or something */
		ret = thrustmaster_submit_change(tm_wheel, 0x000b);
		if (ret)
			goto error6;

		return ret;
	}

	thrustmaster_interrupts(udev, interface);

	usb_fill_control_urb(
		tm_wheel->urb,
		tm_wheel->usb_dev,
		usb_rcvctrlpipe(tm_wheel->usb_dev, 0),
		(char *)tm_wheel->model_request,
		tm_wheel->response,
		sizeof(struct tm_wheel_response),
		thrustmaster_model_handler,
		tm_wheel
	);

	ret = usb_submit_urb(tm_wheel->urb, GFP_ATOMIC);
	if (ret) {
		dev_err(&interface->dev,
				"Error %d while submitting the URB."
				" Unable to initialize this wheel.\n", ret);
		goto error6;
	}

	return ret;

error6: kfree(tm_wheel->change_request);
error5: kfree(tm_wheel->response);
	usb_put_intf(tm_wheel->interface);

error4: kfree(tm_wheel->model_request);
error3: usb_free_urb(tm_wheel->urb);
error2: usb_put_dev(udev);
	return ret;
}
