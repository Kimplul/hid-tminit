#include "tminit.h"

static int thrustmaster_usb_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	int ret = 0;
	struct tm_wheel *tm_wheel = kzalloc(sizeof(struct tm_wheel), GFP_KERNEL);
	if (!tm_wheel) {
		return -ENOMEM;
	}

	usb_set_intfdata(interface, tm_wheel);
	ret = thrustmaster_probe(tm_wheel, interface);
	if (ret) {
		goto error;
	}

	return ret;

error:
	kfree(tm_wheel);
	return ret;
}

static void thrustmaster_usb_disconnect(struct usb_interface *interface)
{
	struct tm_wheel *tm_wheel = usb_get_intfdata(interface);
	usb_put_intf(tm_wheel->interface);
	usb_put_dev(tm_wheel->usb_dev);
	thrustmaster_disconnect(tm_wheel);
}

static const struct usb_device_id thrustmaster_usb_devices[] = {
	{ USB_DEVICE(0x044f, 0xb69c) },
	{ USB_DEVICE(0x044f, 0xb691) },
	{}
};

MODULE_DEVICE_TABLE(usb, thrustmaster_usb_devices);

static struct usb_driver thrustmaster_driver = {
	.name = "usb-thrustmaster",
	.id_table = thrustmaster_usb_devices,
	.probe = thrustmaster_usb_probe,
	.disconnect = thrustmaster_usb_disconnect,
};

module_usb_driver(thrustmaster_driver);

MODULE_AUTHOR("Dario Pagani <dario.pagani.146+linuxk@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver to initialize some steering wheel joysticks from Thrustmaster");
