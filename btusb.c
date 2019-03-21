/*
 *
 *  Generic Bluetooth USB driver
 *
 *  Copyright (C) 2005-2008  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/dmi.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/quirks.h>
#include <linux/firmware.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/suspend.h>
#include <linux/gpio/consumer.h>
#include <asm/unaligned.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "btrtl.h"

#define VERSION "0.8"

static bool disable_scofix;
static bool force_scofix;
static bool enable_autosuspend = IS_ENABLED(CONFIG_BT_HCIBTUSB_AUTOSUSPEND);

static bool reset = true;

static struct usb_driver btusb_driver;

#define BTUSB_IGNORE		0x01
#define BTUSB_DIGIANSWER	0x02
#define BTUSB_CSR		0x04
#define BTUSB_SNIFFER		0x08
#define BTUSB_BROKEN_ISOC	0x20
#define BTUSB_WRONG_SCO_MTU	0x40
#define BTUSB_REALTEK		0x20000
#define BTUSB_IFNUM_2		0x80000
#define BTUSB_CW6622		0x100000

static const struct usb_device_id btusb_table[] = {
	/* Generic Bluetooth USB device */
	{ USB_DEVICE_INFO(0xe0, 0x01, 0x01) },

	/* Generic Bluetooth USB interface */
	{ USB_INTERFACE_INFO(0xe0, 0x01, 0x01) },

	/* MediaTek MT76x0E */
	{ USB_DEVICE(0x0e8d, 0x763f) },

	/* Bluetooth Ultraport Module from IBM */
	{ USB_DEVICE(0x04bf, 0x030a) },

	/* ALPS Modules with non-standard id */
	{ USB_DEVICE(0x044e, 0x3001) },
	{ USB_DEVICE(0x044e, 0x3002) },

	/* Ericsson with non-standard id */
	{ USB_DEVICE(0x0bdb, 0x1002) },

	/* Canyon CN-BTU1 with HID interfaces */
	{ USB_DEVICE(0x0c10, 0x0000) },

	{ }	/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, btusb_table);

static const struct usb_device_id blacklist_table[] = {
	/* CSR BlueCore devices */
	{ USB_DEVICE(0x0a12, 0x0001), .driver_info = BTUSB_CSR },

	/* Dell Wireless 370 and 410 devices */
	{ USB_DEVICE(0x413c, 0x8152), .driver_info = BTUSB_WRONG_SCO_MTU },
	{ USB_DEVICE(0x413c, 0x8156), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* Belkin F8T012 and F8T013 devices */
	{ USB_DEVICE(0x050d, 0x0012), .driver_info = BTUSB_WRONG_SCO_MTU },
	{ USB_DEVICE(0x050d, 0x0013), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* Asus WL-BTD202 device */
	{ USB_DEVICE(0x0b05, 0x1715), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* Kensington Bluetooth USB adapter */
	{ USB_DEVICE(0x047d, 0x105e), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* RTX Telecom based adapters with buggy SCO support */
	{ USB_DEVICE(0x0400, 0x0807), .driver_info = BTUSB_BROKEN_ISOC },
	{ USB_DEVICE(0x0400, 0x080a), .driver_info = BTUSB_BROKEN_ISOC },

	/* CONWISE Technology based adapters with buggy SCO support */
	{ USB_DEVICE(0x0e5e, 0x6622),
	  .driver_info = BTUSB_BROKEN_ISOC | BTUSB_CW6622},

	/* Digianswer devices */
	{ USB_DEVICE(0x08fd, 0x0001), .driver_info = BTUSB_DIGIANSWER },
	{ USB_DEVICE(0x08fd, 0x0002), .driver_info = BTUSB_IGNORE },

	/* CSR BlueCore Bluetooth Sniffer */
	{ USB_DEVICE(0x0a12, 0x0002),
	  .driver_info = BTUSB_SNIFFER | BTUSB_BROKEN_ISOC },

	/* Frontline ComProbe Bluetooth Sniffer */
	{ USB_DEVICE(0x16d3, 0x0002),
	  .driver_info = BTUSB_SNIFFER | BTUSB_BROKEN_ISOC },

	/* Other Intel Bluetooth devices */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x8087, 0xe0, 0x01, 0x01),
	  .driver_info = BTUSB_IGNORE },

	/* Realtek Bluetooth devices */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x0bda, 0xe0, 0x01, 0x01),
	  .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8723AE Bluetooth devices */
	{ USB_DEVICE(0x0930, 0x021d), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3394), .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8723BE Bluetooth devices */
	{ USB_DEVICE(0x0489, 0xe085), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x0489, 0xe08b), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3410), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3416), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3459), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3494), .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8723BU Bluetooth devices */
	{ USB_DEVICE(0x7392, 0xa611), .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8723DE Bluetooth devices */
	{ USB_DEVICE(0x0bda, 0xb009), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x2ff8, 0xb011), .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8821AE Bluetooth devices */
	{ USB_DEVICE(0x0b05, 0x17dc), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3414), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3458), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3461), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3462), .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8822BE Bluetooth devices */
	{ USB_DEVICE(0x13d3, 0x3526), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x0b05, 0x185c), .driver_info = BTUSB_REALTEK },

	{ }	/* Terminating entry */
};

/* The Bluetooth USB module build into some devices needs to be reset on resume,
 * this is a problem with the platform (likely shutting off all power) not with
 * the module itself. So we use a DMI list to match known broken platforms.
 */
static const struct dmi_system_id btusb_needs_reset_resume_table[] = {
	{}
};

#define BTUSB_MAX_ISOC_FRAMES	10

#define BTUSB_INTR_RUNNING	0
#define BTUSB_BULK_RUNNING	1
#define BTUSB_ISOC_RUNNING	2
#define BTUSB_SUSPENDING	3
#define BTUSB_DID_ISO_RESUME	4
#define BTUSB_BOOTLOADER	5
#define BTUSB_DOWNLOADING	6
#define BTUSB_FIRMWARE_LOADED	7
#define BTUSB_FIRMWARE_FAILED	8
#define BTUSB_BOOTING		9
#define BTUSB_DIAG_RUNNING	10
#define BTUSB_OOB_WAKE_ENABLED	11
#define BTUSB_HW_RESET_ACTIVE	12

struct btusb_data {
	struct hci_dev       *hdev;
	struct usb_device    *udev;
	struct usb_interface *intf;
	struct usb_interface *isoc;
	struct usb_interface *diag;
	unsigned isoc_ifnum;

	unsigned long flags;

	struct work_struct work;
	struct work_struct waker;

	struct usb_anchor deferred;
	struct usb_anchor tx_anchor;
	int tx_in_flight;
	spinlock_t txlock;

	struct usb_anchor intr_anchor;
	struct usb_anchor bulk_anchor;
	struct usb_anchor isoc_anchor;
	struct usb_anchor diag_anchor;
	spinlock_t rxlock;

	struct sk_buff *evt_skb;
	struct sk_buff *acl_skb;
	struct sk_buff *sco_skb;

	struct usb_endpoint_descriptor *intr_ep;
	struct usb_endpoint_descriptor *bulk_tx_ep;
	struct usb_endpoint_descriptor *bulk_rx_ep;
	struct usb_endpoint_descriptor *isoc_tx_ep;
	struct usb_endpoint_descriptor *isoc_rx_ep;
	struct usb_endpoint_descriptor *diag_tx_ep;
	struct usb_endpoint_descriptor *diag_rx_ep;

	struct gpio_desc *reset_gpio;

	__u8 cmdreq_type;
	__u8 cmdreq;

	unsigned int sco_num;
	int isoc_altsetting;
	int suspend_count;

	int (*recv_event)(struct hci_dev *hdev, struct sk_buff *skb);
	int (*recv_bulk)(struct btusb_data *data, void *buffer, int count);

	int (*setup_on_usb)(struct hci_dev *hdev);

	int oob_wake_irq;   /* irq for out-of-band wake-on-bt */
	unsigned cmd_timeout_cnt;
};


static inline void btusb_free_frags(struct btusb_data *data)
{
	unsigned long flags;

	spin_lock_irqsave(&data->rxlock, flags);

	kfree_skb(data->evt_skb);
	data->evt_skb = NULL;

	kfree_skb(data->acl_skb);
	data->acl_skb = NULL;

	kfree_skb(data->sco_skb);
	data->sco_skb = NULL;

	spin_unlock_irqrestore(&data->rxlock, flags);
}

static int btusb_recv_intr(struct btusb_data *data, void *buffer, int count)
{
	struct sk_buff *skb;
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&data->rxlock, flags);
	skb = data->evt_skb;

	while (count) {
		int len;

		if (!skb) {
			skb = bt_skb_alloc(HCI_MAX_EVENT_SIZE, GFP_ATOMIC);
			if (!skb) {
				err = -ENOMEM;
				break;
			}

			hci_skb_pkt_type(skb) = HCI_EVENT_PKT;
			hci_skb_expect(skb) = HCI_EVENT_HDR_SIZE;
		}

		len = min_t(uint, hci_skb_expect(skb), count);
		skb_put_data(skb, buffer, len);

		count -= len;
		buffer += len;
		hci_skb_expect(skb) -= len;

		if (skb->len == HCI_EVENT_HDR_SIZE) {
			/* Complete event header */
			hci_skb_expect(skb) = hci_event_hdr(skb)->plen;

			if (skb_tailroom(skb) < hci_skb_expect(skb)) {
				kfree_skb(skb);
				skb = NULL;

				err = -EILSEQ;
				break;
			}
		}

		if (!hci_skb_expect(skb)) {
			/* Complete frame */
			data->recv_event(data->hdev, skb);
			skb = NULL;
		}
	}

	data->evt_skb = skb;
	spin_unlock_irqrestore(&data->rxlock, flags);

	return err;
}

static int btusb_recv_bulk(struct btusb_data *data, void *buffer, int count)
{
	struct sk_buff *skb;
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&data->rxlock, flags);
	skb = data->acl_skb;

	while (count) {
		int len;

		if (!skb) {
			skb = bt_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_ATOMIC);
			if (!skb) {
				err = -ENOMEM;
				break;
			}

			hci_skb_pkt_type(skb) = HCI_ACLDATA_PKT;
			hci_skb_expect(skb) = HCI_ACL_HDR_SIZE;
		}

		len = min_t(uint, hci_skb_expect(skb), count);
		skb_put_data(skb, buffer, len);

		count -= len;
		buffer += len;
		hci_skb_expect(skb) -= len;

		if (skb->len == HCI_ACL_HDR_SIZE) {
			__le16 dlen = hci_acl_hdr(skb)->dlen;

			/* Complete ACL header */
			hci_skb_expect(skb) = __le16_to_cpu(dlen);

			if (skb_tailroom(skb) < hci_skb_expect(skb)) {
				kfree_skb(skb);
				skb = NULL;

				err = -EILSEQ;
				break;
			}
		}

		if (!hci_skb_expect(skb)) {
			/* Complete frame */
			hci_recv_frame(data->hdev, skb);
			skb = NULL;
		}
	}

	data->acl_skb = skb;
	spin_unlock_irqrestore(&data->rxlock, flags);

	return err;
}

static int btusb_recv_isoc(struct btusb_data *data, void *buffer, int count)
{
	struct sk_buff *skb;
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&data->rxlock, flags);
	skb = data->sco_skb;

	while (count) {
		int len;

		if (!skb) {
			skb = bt_skb_alloc(HCI_MAX_SCO_SIZE, GFP_ATOMIC);
			if (!skb) {
				err = -ENOMEM;
				break;
			}

			hci_skb_pkt_type(skb) = HCI_SCODATA_PKT;
			hci_skb_expect(skb) = HCI_SCO_HDR_SIZE;
		}

		len = min_t(uint, hci_skb_expect(skb), count);
		skb_put_data(skb, buffer, len);

		count -= len;
		buffer += len;
		hci_skb_expect(skb) -= len;

		if (skb->len == HCI_SCO_HDR_SIZE) {
			/* Complete SCO header */
			hci_skb_expect(skb) = hci_sco_hdr(skb)->dlen;

			if (skb_tailroom(skb) < hci_skb_expect(skb)) {
				kfree_skb(skb);
				skb = NULL;

				err = -EILSEQ;
				break;
			}
		}

		if (!hci_skb_expect(skb)) {
			/* Complete frame */
			hci_recv_frame(data->hdev, skb);
			skb = NULL;
		}
	}

	data->sco_skb = skb;
	spin_unlock_irqrestore(&data->rxlock, flags);

	return err;
}

static void btusb_intr_complete(struct urb *urb)
{
	struct hci_dev *hdev = urb->context;
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return;

	if (urb->status == 0) {
		hdev->stat.byte_rx += urb->actual_length;

		if (btusb_recv_intr(data, urb->transfer_buffer,
				    urb->actual_length) < 0) {
			bt_dev_err(hdev, "corrupted event packet");
			hdev->stat.err_rx++;
		}
	} else if (urb->status == -ENOENT) {
		/* Avoid suspend failed when usb_kill_urb */
		return;
	}

	if (!test_bit(BTUSB_INTR_RUNNING, &data->flags))
		return;

	usb_mark_last_busy(data->udev);
	usb_anchor_urb(urb, &data->intr_anchor);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		/* -EPERM: urb is being killed;
		 * -ENODEV: device got disconnected
		 */
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p failed to resubmit (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}
}

static int btusb_submit_intr_urb(struct hci_dev *hdev, gfp_t mem_flags)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;
	int err, size;

	BT_DBG("%s", hdev->name);

	if (!data->intr_ep)
		return -ENODEV;

	urb = usb_alloc_urb(0, mem_flags);
	if (!urb)
		return -ENOMEM;

	size = le16_to_cpu(data->intr_ep->wMaxPacketSize);

	buf = kmalloc(size, mem_flags);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	pipe = usb_rcvintpipe(data->udev, data->intr_ep->bEndpointAddress);

	usb_fill_int_urb(urb, data->udev, pipe, buf, size,
			 btusb_intr_complete, hdev, data->intr_ep->bInterval);

	urb->transfer_flags |= URB_FREE_BUFFER;

	usb_anchor_urb(urb, &data->intr_anchor);

	err = usb_submit_urb(urb, mem_flags);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p submission failed (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}

	usb_free_urb(urb);

	return err;
}

static void btusb_bulk_complete(struct urb *urb)
{
	struct hci_dev *hdev = urb->context;
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return;

	if (urb->status == 0) {
		hdev->stat.byte_rx += urb->actual_length;

		if (data->recv_bulk(data, urb->transfer_buffer,
				    urb->actual_length) < 0) {
			bt_dev_err(hdev, "corrupted ACL packet");
			hdev->stat.err_rx++;
		}
	} else if (urb->status == -ENOENT) {
		/* Avoid suspend failed when usb_kill_urb */
		return;
	}

	if (!test_bit(BTUSB_BULK_RUNNING, &data->flags))
		return;

	usb_anchor_urb(urb, &data->bulk_anchor);
	usb_mark_last_busy(data->udev);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		/* -EPERM: urb is being killed;
		 * -ENODEV: device got disconnected
		 */
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p failed to resubmit (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}
}

static int btusb_submit_bulk_urb(struct hci_dev *hdev, gfp_t mem_flags)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;
	int err, size = HCI_MAX_FRAME_SIZE;

	BT_DBG("%s", hdev->name);

	if (!data->bulk_rx_ep)
		return -ENODEV;

	urb = usb_alloc_urb(0, mem_flags);
	if (!urb)
		return -ENOMEM;

	buf = kmalloc(size, mem_flags);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	pipe = usb_rcvbulkpipe(data->udev, data->bulk_rx_ep->bEndpointAddress);

	usb_fill_bulk_urb(urb, data->udev, pipe, buf, size,
			  btusb_bulk_complete, hdev);

	urb->transfer_flags |= URB_FREE_BUFFER;

	usb_mark_last_busy(data->udev);
	usb_anchor_urb(urb, &data->bulk_anchor);

	err = usb_submit_urb(urb, mem_flags);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p submission failed (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}

	usb_free_urb(urb);

	return err;
}

static void btusb_isoc_complete(struct urb *urb)
{
	struct hci_dev *hdev = urb->context;
	struct btusb_data *data = hci_get_drvdata(hdev);
	int i, err;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return;

	if (urb->status == 0) {
		for (i = 0; i < urb->number_of_packets; i++) {
			unsigned int offset = urb->iso_frame_desc[i].offset;
			unsigned int length = urb->iso_frame_desc[i].actual_length;

			if (urb->iso_frame_desc[i].status)
				continue;

			hdev->stat.byte_rx += length;

			if (btusb_recv_isoc(data, urb->transfer_buffer + offset,
					    length) < 0) {
				bt_dev_err(hdev, "corrupted SCO packet");
				hdev->stat.err_rx++;
			}
		}
	} else if (urb->status == -ENOENT) {
		/* Avoid suspend failed when usb_kill_urb */
		return;
	}

	if (!test_bit(BTUSB_ISOC_RUNNING, &data->flags))
		return;

	usb_anchor_urb(urb, &data->isoc_anchor);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		/* -EPERM: urb is being killed;
		 * -ENODEV: device got disconnected
		 */
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p failed to resubmit (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}
}

static inline void __fill_isoc_descriptor(struct urb *urb, int len, int mtu)
{
	int i, offset = 0;

	BT_DBG("len %d mtu %d", len, mtu);

	for (i = 0; i < BTUSB_MAX_ISOC_FRAMES && len >= mtu;
					i++, offset += mtu, len -= mtu) {
		urb->iso_frame_desc[i].offset = offset;
		urb->iso_frame_desc[i].length = mtu;
	}

	if (len && i < BTUSB_MAX_ISOC_FRAMES) {
		urb->iso_frame_desc[i].offset = offset;
		urb->iso_frame_desc[i].length = len;
		i++;
	}

	urb->number_of_packets = i;
}

static int btusb_submit_isoc_urb(struct hci_dev *hdev, gfp_t mem_flags)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;
	int err, size;

	BT_DBG("%s", hdev->name);

	if (!data->isoc_rx_ep)
		return -ENODEV;

	urb = usb_alloc_urb(BTUSB_MAX_ISOC_FRAMES, mem_flags);
	if (!urb)
		return -ENOMEM;

	size = le16_to_cpu(data->isoc_rx_ep->wMaxPacketSize) *
						BTUSB_MAX_ISOC_FRAMES;

	buf = kmalloc(size, mem_flags);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	pipe = usb_rcvisocpipe(data->udev, data->isoc_rx_ep->bEndpointAddress);

	usb_fill_int_urb(urb, data->udev, pipe, buf, size, btusb_isoc_complete,
			 hdev, data->isoc_rx_ep->bInterval);

	urb->transfer_flags = URB_FREE_BUFFER | URB_ISO_ASAP;

	__fill_isoc_descriptor(urb, size,
			       le16_to_cpu(data->isoc_rx_ep->wMaxPacketSize));

	usb_anchor_urb(urb, &data->isoc_anchor);

	err = usb_submit_urb(urb, mem_flags);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p submission failed (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}

	usb_free_urb(urb);

	return err;
}

static void btusb_diag_complete(struct urb *urb)
{
	struct hci_dev *hdev = urb->context;
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (urb->status == 0) {
		struct sk_buff *skb;

		skb = bt_skb_alloc(urb->actual_length, GFP_ATOMIC);
		if (skb) {
			skb_put_data(skb, urb->transfer_buffer,
				     urb->actual_length);
			hci_recv_diag(hdev, skb);
		}
	} else if (urb->status == -ENOENT) {
		/* Avoid suspend failed when usb_kill_urb */
		return;
	}

	if (!test_bit(BTUSB_DIAG_RUNNING, &data->flags))
		return;

	usb_anchor_urb(urb, &data->diag_anchor);
	usb_mark_last_busy(data->udev);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		/* -EPERM: urb is being killed;
		 * -ENODEV: device got disconnected
		 */
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p failed to resubmit (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}
}

static int btusb_submit_diag_urb(struct hci_dev *hdev, gfp_t mem_flags)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;
	int err, size = HCI_MAX_FRAME_SIZE;

	BT_DBG("%s", hdev->name);

	if (!data->diag_rx_ep)
		return -ENODEV;

	urb = usb_alloc_urb(0, mem_flags);
	if (!urb)
		return -ENOMEM;

	buf = kmalloc(size, mem_flags);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	pipe = usb_rcvbulkpipe(data->udev, data->diag_rx_ep->bEndpointAddress);

	usb_fill_bulk_urb(urb, data->udev, pipe, buf, size,
			  btusb_diag_complete, hdev);

	urb->transfer_flags |= URB_FREE_BUFFER;

	usb_mark_last_busy(data->udev);
	usb_anchor_urb(urb, &data->diag_anchor);

	err = usb_submit_urb(urb, mem_flags);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p submission failed (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}

	usb_free_urb(urb);

	return err;
}

static void btusb_tx_complete(struct urb *urb)
{
	struct sk_buff *skb = urb->context;
	struct hci_dev *hdev = (struct hci_dev *)skb->dev;
	struct btusb_data *data = hci_get_drvdata(hdev);
	unsigned long flags;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		goto done;

	if (!urb->status)
		hdev->stat.byte_tx += urb->transfer_buffer_length;
	else
		hdev->stat.err_tx++;

done:
	spin_lock_irqsave(&data->txlock, flags);
	data->tx_in_flight--;
	spin_unlock_irqrestore(&data->txlock, flags);

	kfree(urb->setup_packet);

	kfree_skb(skb);
}

static void btusb_isoc_tx_complete(struct urb *urb)
{
	struct sk_buff *skb = urb->context;
	struct hci_dev *hdev = (struct hci_dev *)skb->dev;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		goto done;

	if (!urb->status)
		hdev->stat.byte_tx += urb->transfer_buffer_length;
	else
		hdev->stat.err_tx++;

done:
	kfree(urb->setup_packet);

	kfree_skb(skb);
}

static int btusb_open(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	BT_DBG("%s", hdev->name);

	err = usb_autopm_get_interface(data->intf);
	if (err < 0)
		return err;

	/* Patching USB firmware files prior to starting any URBs of HCI path
	 * It is more safe to use USB bulk channel for downloading USB patch
	 */
	if (data->setup_on_usb) {
		err = data->setup_on_usb(hdev);
		if (err < 0)
			return err;
	}

	data->intf->needs_remote_wakeup = 1;
	/* device specific wakeup source enabled and required for USB
	 * remote wakeup while host is suspended
	 */
	device_wakeup_enable(&data->udev->dev);

	if (test_and_set_bit(BTUSB_INTR_RUNNING, &data->flags))
		goto done;

	err = btusb_submit_intr_urb(hdev, GFP_KERNEL);
	if (err < 0)
		goto failed;

	err = btusb_submit_bulk_urb(hdev, GFP_KERNEL);
	if (err < 0) {
		usb_kill_anchored_urbs(&data->intr_anchor);
		goto failed;
	}

	set_bit(BTUSB_BULK_RUNNING, &data->flags);
	btusb_submit_bulk_urb(hdev, GFP_KERNEL);

	if (data->diag) {
		if (!btusb_submit_diag_urb(hdev, GFP_KERNEL))
			set_bit(BTUSB_DIAG_RUNNING, &data->flags);
	}

done:
	usb_autopm_put_interface(data->intf);
	return 0;

failed:
	clear_bit(BTUSB_INTR_RUNNING, &data->flags);
	usb_autopm_put_interface(data->intf);
	return err;
}

static void btusb_stop_traffic(struct btusb_data *data)
{
	usb_kill_anchored_urbs(&data->intr_anchor);
	usb_kill_anchored_urbs(&data->bulk_anchor);
	usb_kill_anchored_urbs(&data->isoc_anchor);
	usb_kill_anchored_urbs(&data->diag_anchor);
}

static int btusb_close(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	BT_DBG("%s", hdev->name);

	cancel_work_sync(&data->work);
	cancel_work_sync(&data->waker);

	clear_bit(BTUSB_ISOC_RUNNING, &data->flags);
	clear_bit(BTUSB_BULK_RUNNING, &data->flags);
	clear_bit(BTUSB_INTR_RUNNING, &data->flags);
	clear_bit(BTUSB_DIAG_RUNNING, &data->flags);

	btusb_stop_traffic(data);
	btusb_free_frags(data);

	err = usb_autopm_get_interface(data->intf);
	if (err < 0)
		goto failed;

	data->intf->needs_remote_wakeup = 0;
	device_wakeup_disable(&data->udev->dev);
	usb_autopm_put_interface(data->intf);

failed:
	usb_scuttle_anchored_urbs(&data->deferred);
	return 0;
}

static int btusb_flush(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);

	BT_DBG("%s", hdev->name);

	usb_kill_anchored_urbs(&data->tx_anchor);
	btusb_free_frags(data);

	return 0;
}

static struct urb *alloc_ctrl_urb(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct usb_ctrlrequest *dr;
	struct urb *urb;
	unsigned int pipe;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return ERR_PTR(-ENOMEM);

	dr = kmalloc(sizeof(*dr), GFP_KERNEL);
	if (!dr) {
		usb_free_urb(urb);
		return ERR_PTR(-ENOMEM);
	}

	dr->bRequestType = data->cmdreq_type;
	dr->bRequest     = data->cmdreq;
	dr->wIndex       = 0;
	dr->wValue       = 0;
	dr->wLength      = __cpu_to_le16(skb->len);

	pipe = usb_sndctrlpipe(data->udev, 0x00);

	usb_fill_control_urb(urb, data->udev, pipe, (void *)dr,
			     skb->data, skb->len, btusb_tx_complete, skb);

	skb->dev = (void *)hdev;

	return urb;
}

static struct urb *alloc_bulk_urb(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned int pipe;

	if (!data->bulk_tx_ep)
		return ERR_PTR(-ENODEV);

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return ERR_PTR(-ENOMEM);

	pipe = usb_sndbulkpipe(data->udev, data->bulk_tx_ep->bEndpointAddress);

	usb_fill_bulk_urb(urb, data->udev, pipe,
			  skb->data, skb->len, btusb_tx_complete, skb);

	skb->dev = (void *)hdev;

	return urb;
}

static struct urb *alloc_isoc_urb(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned int pipe;

	if (!data->isoc_tx_ep)
		return ERR_PTR(-ENODEV);

	urb = usb_alloc_urb(BTUSB_MAX_ISOC_FRAMES, GFP_KERNEL);
	if (!urb)
		return ERR_PTR(-ENOMEM);

	pipe = usb_sndisocpipe(data->udev, data->isoc_tx_ep->bEndpointAddress);

	usb_fill_int_urb(urb, data->udev, pipe,
			 skb->data, skb->len, btusb_isoc_tx_complete,
			 skb, data->isoc_tx_ep->bInterval);

	urb->transfer_flags  = URB_ISO_ASAP;

	__fill_isoc_descriptor(urb, skb->len,
			       le16_to_cpu(data->isoc_tx_ep->wMaxPacketSize));

	skb->dev = (void *)hdev;

	return urb;
}

static int submit_tx_urb(struct hci_dev *hdev, struct urb *urb)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	usb_anchor_urb(urb, &data->tx_anchor);

	err = usb_submit_urb(urb, GFP_KERNEL);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p submission failed (%d)",
				   urb, -err);
		kfree(urb->setup_packet);
		usb_unanchor_urb(urb);
	} else {
		usb_mark_last_busy(data->udev);
	}

	usb_free_urb(urb);
	return err;
}

static int submit_or_queue_tx_urb(struct hci_dev *hdev, struct urb *urb)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	unsigned long flags;
	bool suspending;

	spin_lock_irqsave(&data->txlock, flags);
	suspending = test_bit(BTUSB_SUSPENDING, &data->flags);
	if (!suspending)
		data->tx_in_flight++;
	spin_unlock_irqrestore(&data->txlock, flags);

	if (!suspending)
		return submit_tx_urb(hdev, urb);

	usb_anchor_urb(urb, &data->deferred);
	schedule_work(&data->waker);

	usb_free_urb(urb);
	return 0;
}

static int btusb_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct urb *urb;

	BT_DBG("%s", hdev->name);

	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT:
		urb = alloc_ctrl_urb(hdev, skb);
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		hdev->stat.cmd_tx++;
		return submit_or_queue_tx_urb(hdev, urb);

	case HCI_ACLDATA_PKT:
		urb = alloc_bulk_urb(hdev, skb);
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		hdev->stat.acl_tx++;
		return submit_or_queue_tx_urb(hdev, urb);

	case HCI_SCODATA_PKT:
		if (hci_conn_num(hdev, SCO_LINK) < 1)
			return -ENODEV;

		urb = alloc_isoc_urb(hdev, skb);
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		hdev->stat.sco_tx++;
		return submit_tx_urb(hdev, urb);
	}

	return -EILSEQ;
}

static void btusb_notify(struct hci_dev *hdev, unsigned int evt)
{
	struct btusb_data *data = hci_get_drvdata(hdev);

	BT_DBG("%s evt %d", hdev->name, evt);

	if (hci_conn_num(hdev, SCO_LINK) != data->sco_num) {
		data->sco_num = hci_conn_num(hdev, SCO_LINK);
		schedule_work(&data->work);
	}
}

static inline int __set_isoc_interface(struct hci_dev *hdev, int altsetting)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct usb_interface *intf = data->isoc;
	struct usb_endpoint_descriptor *ep_desc;
	int i, err;

	if (!data->isoc)
		return -ENODEV;

	err = usb_set_interface(data->udev, data->isoc_ifnum, altsetting);
	if (err < 0) {
		bt_dev_err(hdev, "setting interface failed (%d)", -err);
		return err;
	}

	data->isoc_altsetting = altsetting;

	data->isoc_tx_ep = NULL;
	data->isoc_rx_ep = NULL;

	for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		ep_desc = &intf->cur_altsetting->endpoint[i].desc;

		if (!data->isoc_tx_ep && usb_endpoint_is_isoc_out(ep_desc)) {
			data->isoc_tx_ep = ep_desc;
			continue;
		}

		if (!data->isoc_rx_ep && usb_endpoint_is_isoc_in(ep_desc)) {
			data->isoc_rx_ep = ep_desc;
			continue;
		}
	}

	if (!data->isoc_tx_ep || !data->isoc_rx_ep) {
		bt_dev_err(hdev, "invalid SCO descriptors");
		return -ENODEV;
	}

	return 0;
}

static void btusb_work(struct work_struct *work)
{
	struct btusb_data *data = container_of(work, struct btusb_data, work);
	struct hci_dev *hdev = data->hdev;
	int new_alts;
	int err;

	if (data->sco_num > 0) {
		if (!test_bit(BTUSB_DID_ISO_RESUME, &data->flags)) {
			err = usb_autopm_get_interface(data->isoc ? data->isoc : data->intf);
			if (err < 0) {
				clear_bit(BTUSB_ISOC_RUNNING, &data->flags);
				usb_kill_anchored_urbs(&data->isoc_anchor);
				return;
			}

			set_bit(BTUSB_DID_ISO_RESUME, &data->flags);
		}

		if (hdev->voice_setting & 0x0020) {
			static const int alts[3] = { 2, 4, 5 };

			new_alts = alts[data->sco_num - 1];
		} else {
			new_alts = data->sco_num;
		}

		if (data->isoc_altsetting != new_alts) {
			unsigned long flags;

			clear_bit(BTUSB_ISOC_RUNNING, &data->flags);
			usb_kill_anchored_urbs(&data->isoc_anchor);

			/* When isochronous alternate setting needs to be
			 * changed, because SCO connection has been added
			 * or removed, a packet fragment may be left in the
			 * reassembling state. This could lead to wrongly
			 * assembled fragments.
			 *
			 * Clear outstanding fragment when selecting a new
			 * alternate setting.
			 */
			spin_lock_irqsave(&data->rxlock, flags);
			kfree_skb(data->sco_skb);
			data->sco_skb = NULL;
			spin_unlock_irqrestore(&data->rxlock, flags);

			if (__set_isoc_interface(hdev, new_alts) < 0)
				return;
		}

		if (!test_and_set_bit(BTUSB_ISOC_RUNNING, &data->flags)) {
			if (btusb_submit_isoc_urb(hdev, GFP_KERNEL) < 0)
				clear_bit(BTUSB_ISOC_RUNNING, &data->flags);
			else
				btusb_submit_isoc_urb(hdev, GFP_KERNEL);
		}
	} else {
		clear_bit(BTUSB_ISOC_RUNNING, &data->flags);
		usb_kill_anchored_urbs(&data->isoc_anchor);

		__set_isoc_interface(hdev, 0);
		if (test_and_clear_bit(BTUSB_DID_ISO_RESUME, &data->flags))
			usb_autopm_put_interface(data->isoc ? data->isoc : data->intf);
	}
}

static void btusb_waker(struct work_struct *work)
{
	struct btusb_data *data = container_of(work, struct btusb_data, waker);
	int err;

	err = usb_autopm_get_interface(data->intf);
	if (err < 0)
		return;

	usb_autopm_put_interface(data->intf);
}

static int btusb_setup_csr(struct hci_dev *hdev)
{
	struct hci_rp_read_local_version *rp;
	struct sk_buff *skb;

	BT_DBG("%s", hdev->name);

	skb = __hci_cmd_sync(hdev, HCI_OP_READ_LOCAL_VERSION, 0, NULL,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		int err = PTR_ERR(skb);
		bt_dev_err(hdev, "CSR: Local version failed (%d)", err);
		return err;
	}

	if (skb->len != sizeof(struct hci_rp_read_local_version)) {
		bt_dev_err(hdev, "CSR: Local version length mismatch");
		kfree_skb(skb);
		return -EIO;
	}

	rp = (struct hci_rp_read_local_version *)skb->data;

	/* Detect controllers which aren't real CSR ones. */
	if (le16_to_cpu(rp->manufacturer) != 10 ||
	    le16_to_cpu(rp->lmp_subver) == 0x0c5c) {
		/* Clear the reset quirk since this is not an actual
		 * early Bluetooth 1.1 device from CSR.
		 */
		clear_bit(HCI_QUIRK_RESET_ON_CLOSE, &hdev->quirks);

		/* These fake CSR controllers have all a broken
		 * stored link key handling and so just disable it.
		 */
		set_bit(HCI_QUIRK_BROKEN_STORED_LINK_KEY, &hdev->quirks);
	}

	kfree_skb(skb);

	return 0;
}

#ifdef CONFIG_PM
static irqreturn_t btusb_oob_wake_handler(int irq, void *priv)
{
	struct btusb_data *data = priv;

	pm_wakeup_event(&data->udev->dev, 0);
	pm_system_wakeup();

	/* Disable only if not already disabled (keep it balanced) */
	if (test_and_clear_bit(BTUSB_OOB_WAKE_ENABLED, &data->flags)) {
		disable_irq_nosync(irq);
		disable_irq_wake(irq);
	}
	return IRQ_HANDLED;
}

static const struct of_device_id btusb_match_table[] = {
	{ .compatible = "usb1286,204e" },
	{ }
};
MODULE_DEVICE_TABLE(of, btusb_match_table);

/* Use an oob wakeup pin? */
static int btusb_config_oob_wake(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct device *dev = &data->udev->dev;
	int irq, ret;

	clear_bit(BTUSB_OOB_WAKE_ENABLED, &data->flags);

	if (!of_match_device(btusb_match_table, dev))
		return 0;

	/* Move on if no IRQ specified */
	irq = of_irq_get_byname(dev->of_node, "wakeup");
	if (irq <= 0) {
		bt_dev_dbg(hdev, "%s: no OOB Wakeup IRQ in DT", __func__);
		return 0;
	}

	ret = devm_request_irq(&hdev->dev, irq, btusb_oob_wake_handler,
			       0, "OOB Wake-on-BT", data);
	if (ret) {
		bt_dev_err(hdev, "%s: IRQ request failed", __func__);
		return ret;
	}

	ret = device_init_wakeup(dev, true);
	if (ret) {
		bt_dev_err(hdev, "%s: failed to init_wakeup", __func__);
		return ret;
	}

	data->oob_wake_irq = irq;
	disable_irq(irq);
	bt_dev_info(hdev, "OOB Wake-on-BT configured at IRQ %u", irq);
	return 0;
}
#endif

static int btusb_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_endpoint_descriptor *ep_desc;
	struct gpio_desc *reset_gpio;
	struct btusb_data *data;
	struct hci_dev *hdev;
	unsigned ifnum_base;
	int i, err;

	BT_DBG("intf %p id %p", intf, id);

	/* interface numbers are hardcoded in the spec */
	if (intf->cur_altsetting->desc.bInterfaceNumber != 0) {
		if (!(id->driver_info & BTUSB_IFNUM_2))
			return -ENODEV;
		if (intf->cur_altsetting->desc.bInterfaceNumber != 2)
			return -ENODEV;
	}

	ifnum_base = intf->cur_altsetting->desc.bInterfaceNumber;

	if (!id->driver_info) {
		const struct usb_device_id *match;

		match = usb_match_id(intf, blacklist_table);
		if (match)
			id = match;
	}

	if (id->driver_info == BTUSB_IGNORE)
		return -ENODEV;

	data = devm_kzalloc(&intf->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		ep_desc = &intf->cur_altsetting->endpoint[i].desc;

		if (!data->intr_ep && usb_endpoint_is_int_in(ep_desc)) {
			data->intr_ep = ep_desc;
			continue;
		}

		if (!data->bulk_tx_ep && usb_endpoint_is_bulk_out(ep_desc)) {
			data->bulk_tx_ep = ep_desc;
			continue;
		}

		if (!data->bulk_rx_ep && usb_endpoint_is_bulk_in(ep_desc)) {
			data->bulk_rx_ep = ep_desc;
			continue;
		}
	}

	if (!data->intr_ep || !data->bulk_tx_ep || !data->bulk_rx_ep)
		return -ENODEV;

	data->cmdreq_type = USB_TYPE_CLASS;
	data->cmdreq = 0x00;

	data->udev = interface_to_usbdev(intf);
	data->intf = intf;

	INIT_WORK(&data->work, btusb_work);
	INIT_WORK(&data->waker, btusb_waker);
	init_usb_anchor(&data->deferred);
	init_usb_anchor(&data->tx_anchor);
	spin_lock_init(&data->txlock);

	init_usb_anchor(&data->intr_anchor);
	init_usb_anchor(&data->bulk_anchor);
	init_usb_anchor(&data->isoc_anchor);
	init_usb_anchor(&data->diag_anchor);
	spin_lock_init(&data->rxlock);

	data->recv_event = hci_recv_frame;
	data->recv_bulk = btusb_recv_bulk;

	hdev = hci_alloc_dev();
	if (!hdev)
		return -ENOMEM;

	hdev->bus = HCI_USB;
	hci_set_drvdata(hdev, data);

	hdev->dev_type = HCI_PRIMARY;

	data->hdev = hdev;

	SET_HCIDEV_DEV(hdev, &intf->dev);

	reset_gpio = gpiod_get_optional(&data->udev->dev, "reset",
					GPIOD_OUT_LOW);
	if (IS_ERR(reset_gpio)) {
		err = PTR_ERR(reset_gpio);
		goto out_free_dev;
	} else if (reset_gpio) {
		data->reset_gpio = reset_gpio;
	}

	hdev->open   = btusb_open;
	hdev->close  = btusb_close;
	hdev->flush  = btusb_flush;
	hdev->send   = btusb_send_frame;
	hdev->notify = btusb_notify;

#ifdef CONFIG_PM
	err = btusb_config_oob_wake(hdev);
	if (err)
		goto out_free_dev;

#endif
	if (id->driver_info & BTUSB_CW6622)
		set_bit(HCI_QUIRK_BROKEN_STORED_LINK_KEY, &hdev->quirks);

#ifdef CONFIG_BT_HCIBTUSB_RTL
	if (id->driver_info & BTUSB_REALTEK) {
		hdev->setup = btrtl_setup_realtek;

		/* Realtek devices lose their updated firmware over suspend,
		 * but the USB hub doesn't notice any status change.
		 * Explicitly request a device reset on resume.
		 */
		interface_to_usbdev(intf)->quirks |= USB_QUIRK_RESET_RESUME;
	}
#endif

	/* Interface orders are hardcoded in the specification */
	data->isoc = usb_ifnum_to_if(data->udev, ifnum_base + 1);
	data->isoc_ifnum = ifnum_base + 1;

	if (!reset)
		set_bit(HCI_QUIRK_RESET_ON_CLOSE, &hdev->quirks);

	if (force_scofix || id->driver_info & BTUSB_WRONG_SCO_MTU) {
		if (!disable_scofix)
			set_bit(HCI_QUIRK_FIXUP_BUFFER_SIZE, &hdev->quirks);
	}

	if (id->driver_info & BTUSB_BROKEN_ISOC)
		data->isoc = NULL;

	if (id->driver_info & BTUSB_DIGIANSWER) {
		data->cmdreq_type = USB_TYPE_VENDOR;
		set_bit(HCI_QUIRK_RESET_ON_CLOSE, &hdev->quirks);
	}

	if (id->driver_info & BTUSB_CSR) {
		struct usb_device *udev = data->udev;
		u16 bcdDevice = le16_to_cpu(udev->descriptor.bcdDevice);

		/* Old firmware would otherwise execute USB reset */
		if (bcdDevice < 0x117)
			set_bit(HCI_QUIRK_RESET_ON_CLOSE, &hdev->quirks);

		/* Fake CSR devices with broken commands */
		if (bcdDevice <= 0x100 || bcdDevice == 0x134)
			hdev->setup = btusb_setup_csr;

		set_bit(HCI_QUIRK_SIMULTANEOUS_DISCOVERY, &hdev->quirks);
	}

	if (id->driver_info & BTUSB_SNIFFER) {
		struct usb_device *udev = data->udev;

		/* New sniffer firmware has crippled HCI interface */
		if (le16_to_cpu(udev->descriptor.bcdDevice) > 0x997)
			set_bit(HCI_QUIRK_RAW_DEVICE, &hdev->quirks);
	}

	if (data->isoc) {
		err = usb_driver_claim_interface(&btusb_driver,
						 data->isoc, data);
		if (err < 0)
			goto out_free_dev;
	}

	if (enable_autosuspend)
		usb_enable_autosuspend(data->udev);

	err = hci_register_dev(hdev);
	if (err < 0)
		goto out_free_dev;

	usb_set_intfdata(intf, data);

	return 0;

out_free_dev:
	if (data->reset_gpio)
		gpiod_put(data->reset_gpio);
	hci_free_dev(hdev);
	return err;
}

static void btusb_disconnect(struct usb_interface *intf)
{
	struct btusb_data *data = usb_get_intfdata(intf);
	struct hci_dev *hdev;

	BT_DBG("intf %p", intf);

	if (!data)
		return;

	hdev = data->hdev;
	usb_set_intfdata(data->intf, NULL);

	if (data->isoc)
		usb_set_intfdata(data->isoc, NULL);

	if (data->diag)
		usb_set_intfdata(data->diag, NULL);

	hci_unregister_dev(hdev);

	if (intf == data->intf) {
		if (data->isoc)
			usb_driver_release_interface(&btusb_driver, data->isoc);
		if (data->diag)
			usb_driver_release_interface(&btusb_driver, data->diag);
	} else if (intf == data->isoc) {
		if (data->diag)
			usb_driver_release_interface(&btusb_driver, data->diag);
		usb_driver_release_interface(&btusb_driver, data->intf);
	} else if (intf == data->diag) {
		usb_driver_release_interface(&btusb_driver, data->intf);
		if (data->isoc)
			usb_driver_release_interface(&btusb_driver, data->isoc);
	}

	if (data->oob_wake_irq)
		device_init_wakeup(&data->udev->dev, false);

	if (data->reset_gpio)
		gpiod_put(data->reset_gpio);

	hci_free_dev(hdev);
}

#ifdef CONFIG_PM
static int btusb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct btusb_data *data = usb_get_intfdata(intf);

	BT_DBG("intf %p", intf);

	if (data->suspend_count++)
		return 0;

	spin_lock_irq(&data->txlock);
	if (!(PMSG_IS_AUTO(message) && data->tx_in_flight)) {
		set_bit(BTUSB_SUSPENDING, &data->flags);
		spin_unlock_irq(&data->txlock);
	} else {
		spin_unlock_irq(&data->txlock);
		data->suspend_count--;
		return -EBUSY;
	}

	cancel_work_sync(&data->work);

	btusb_stop_traffic(data);
	usb_kill_anchored_urbs(&data->tx_anchor);

	if (data->oob_wake_irq && device_may_wakeup(&data->udev->dev)) {
		set_bit(BTUSB_OOB_WAKE_ENABLED, &data->flags);
		enable_irq_wake(data->oob_wake_irq);
		enable_irq(data->oob_wake_irq);
	}

	return 0;
}

static void play_deferred(struct btusb_data *data)
{
	struct urb *urb;
	int err;

	while ((urb = usb_get_from_anchor(&data->deferred))) {
		usb_anchor_urb(urb, &data->tx_anchor);

		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err < 0) {
			if (err != -EPERM && err != -ENODEV)
				BT_ERR("%s urb %p submission failed (%d)",
				       data->hdev->name, urb, -err);
			kfree(urb->setup_packet);
			usb_unanchor_urb(urb);
			usb_free_urb(urb);
			break;
		}

		data->tx_in_flight++;
		usb_free_urb(urb);
	}

	/* Cleanup the rest deferred urbs. */
	while ((urb = usb_get_from_anchor(&data->deferred))) {
		kfree(urb->setup_packet);
		usb_free_urb(urb);
	}
}

static int btusb_resume(struct usb_interface *intf)
{
	struct btusb_data *data = usb_get_intfdata(intf);
	struct hci_dev *hdev = data->hdev;
	int err = 0;

	BT_DBG("intf %p", intf);

	if (--data->suspend_count)
		return 0;

	/* Disable only if not already disabled (keep it balanced) */
	if (test_and_clear_bit(BTUSB_OOB_WAKE_ENABLED, &data->flags)) {
		disable_irq(data->oob_wake_irq);
		disable_irq_wake(data->oob_wake_irq);
	}

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		goto done;

	if (test_bit(BTUSB_INTR_RUNNING, &data->flags)) {
		err = btusb_submit_intr_urb(hdev, GFP_NOIO);
		if (err < 0) {
			clear_bit(BTUSB_INTR_RUNNING, &data->flags);
			goto failed;
		}
	}

	if (test_bit(BTUSB_BULK_RUNNING, &data->flags)) {
		err = btusb_submit_bulk_urb(hdev, GFP_NOIO);
		if (err < 0) {
			clear_bit(BTUSB_BULK_RUNNING, &data->flags);
			goto failed;
		}

		btusb_submit_bulk_urb(hdev, GFP_NOIO);
	}

	if (test_bit(BTUSB_ISOC_RUNNING, &data->flags)) {
		if (btusb_submit_isoc_urb(hdev, GFP_NOIO) < 0)
			clear_bit(BTUSB_ISOC_RUNNING, &data->flags);
		else
			btusb_submit_isoc_urb(hdev, GFP_NOIO);
	}

	spin_lock_irq(&data->txlock);
	play_deferred(data);
	clear_bit(BTUSB_SUSPENDING, &data->flags);
	spin_unlock_irq(&data->txlock);
	schedule_work(&data->work);

	return 0;

failed:
	usb_scuttle_anchored_urbs(&data->deferred);
done:
	spin_lock_irq(&data->txlock);
	clear_bit(BTUSB_SUSPENDING, &data->flags);
	spin_unlock_irq(&data->txlock);

	return err;
}
#endif

static struct usb_driver btusb_driver = {
	.name		= "btusb",
	.probe		= btusb_probe,
	.disconnect	= btusb_disconnect,
#ifdef CONFIG_PM
	.suspend	= btusb_suspend,
	.resume		= btusb_resume,
#endif
	.id_table	= btusb_table,
	.supports_autosuspend = 1,
	.disable_hub_initiated_lpm = 1,
};

module_usb_driver(btusb_driver);

module_param(disable_scofix, bool, 0644);
MODULE_PARM_DESC(disable_scofix, "Disable fixup of wrong SCO buffer size");

module_param(force_scofix, bool, 0644);
MODULE_PARM_DESC(force_scofix, "Force fixup of wrong SCO buffers size");

module_param(enable_autosuspend, bool, 0644);
MODULE_PARM_DESC(enable_autosuspend, "Enable USB autosuspend by default");

module_param(reset, bool, 0644);
MODULE_PARM_DESC(reset, "Send HCI reset command on initialization");

MODULE_AUTHOR("Marcel Holtmann <marcel@holtmann.org>");
MODULE_DESCRIPTION("Generic Bluetooth USB driver ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");
