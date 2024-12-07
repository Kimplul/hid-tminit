#include <linux/hid.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "tminit.h"

static int thrustmaster_hid_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret = 0;
	struct tm_wheel *tm_wheel = kzalloc(sizeof(struct tm_wheel), GFP_KERNEL);
	struct usb_interface *interface = to_usb_interface(hdev->dev.parent);

	if (!tm_wheel) {
		return -ENOMEM;
	}

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed with error %d\n", ret);
		goto error;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT & ~HID_CONNECT_FF);
	if (ret) {
		hid_err(hdev, "hw start failed with error %d\n", ret);
		goto error;
	}

	hid_set_drvdata(hdev, tm_wheel);
	ret = thrustmaster_probe(tm_wheel, interface);
	if (ret) {
		goto error1;
	}

	return ret;

error1: hid_hw_stop(hdev);
error:  kfree(tm_wheel);
	return ret;
}

static void thrustmaster_hid_disconnect(struct hid_device *hdev)
{
	struct tm_wheel *tm_wheel = hid_get_drvdata(hdev);
	thrustmaster_disconnect(tm_wheel);
	hid_hw_stop(hdev);
}

static const struct hid_device_id thrustmaster_hid_devices[] = {
	{ HID_USB_DEVICE(0x044f, 0xb65d) },
	{ HID_USB_DEVICE(0x044f, 0xb664) },
	{ HID_USB_DEVICE(0x044f, 0xb69c) },
	{}
};

MODULE_DEVICE_TABLE(hid, thrustmaster_hid_devices);

static struct hid_driver thrustmaster_driver = {
	.name = "hid-tminit-new",
	.id_table = thrustmaster_hid_devices,
	.probe = thrustmaster_hid_probe,
	.remove = thrustmaster_hid_disconnect,
};

module_hid_driver(thrustmaster_driver);

MODULE_AUTHOR("Dario Pagani <dario.pagani.146+linuxk@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver to initialize some steering wheel joysticks from Thrustmaster");
