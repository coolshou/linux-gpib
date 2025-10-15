// SPDX-License-Identifier: GPL-2.0

/***************************************************************************
 *	driver for Xyohro Usbtmc to Gpib Controller adapters		   *
 ***************************************************************************/

#define _GNU_SOURCE

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#define dev_fmt pr_fmt
#define DRV_NAME KBUILD_MODNAME

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/completion.h>
#include <linux/usb/tmc.h>
#include <linux/timer.h>
#include "gpibP.h"
#include "gpib_state_machines.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GPIB driver for xyohro usbtmc to gpib controller adapters");

// Table with the USB-devices: just now only testing IDs
static struct usb_device_id xugc_device_table[] = {
	//Atmel Corp., LUFA Test and Measurement Demo Application
	{USB_DEVICE_AND_INTERFACE_INFO(0x03eb, 0x2065, 255, 3, 1)},
	// xyphro usb ethernet GPIB adapter
	{USB_DEVICE_AND_INTERFACE_INFO(0x16d0, 0x1466, 255, 3, 1)},
	{} /* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, xugc_device_table);

#define MAX_NUM_XUGC_INTERFACES 16
static struct usb_interface *xugc_interfaces[MAX_NUM_XUGC_INTERFACES];
static DEFINE_MUTEX(xugc_hotplug_lock); // protect board insertion and removal

#define SET_REN_STATE	       160
#define READ_CONTROL_LINES     163
#define SET_ATN_STATE	       164
#define SET_IFC_STATE	       165
#define SET_CONTROLLER_ADDRESS 166

static unsigned int xugc_update_status(struct gpib_board *board, unsigned int clear_mask);
static int xugc_line_status(const struct gpib_board *board);

/* Workaround for Linux kernel < 5.4 'fallthrough'*/

#if !defined(fallthrough)
#if __has_attribute(fallthrough)
# define fallthrough attribute((fallthrough))
#else
# define fallthrough do {} while (0) /* fallthrough */
#endif
#endif

/* I/O buffer size used in generic read/write functions */
#define USBTMC_BUFSIZE		(4096)
#define USBTMC_HEADER_SIZE	12
/*
 * Maximum number of read cycles to empty bulk in endpoint during CLEAR and
 * ABORT_BULK_IN requests. Ends the loop if (for whatever reason) a short
 * packet is never read.
 */
#define USBTMC_MAX_READS_TO_CLEAR_BULK_IN	100

/* Minimum packet size for interrupt IN endpoint */
#define USBTMC_MIN_INT_IN_PACKET_SIZE 2	// 1 byte ID + 1 byte data

static unsigned int io_buffer_size = USBTMC_BUFSIZE;

struct xugc_urb_ctx {
	struct completion complete;
	unsigned timed_out : 1;
};

/*
 * This structure holds private data for each adapter device allocated in attach
 */
struct xugc_priv {
	const struct usb_device_id *id;
	struct usb_device *usb_dev;
	struct usb_interface *intf;

	unsigned int bulk_in;	/* Bulk	 in endpoint */
	unsigned int bulk_out;	/* Bulk out endpoint */

	u8 b_tag;
	u8 b_tag_last_write;	/* needed for abort */
	u8 b_tag_last_read;	/* needed for abort */

	u16	       bin_bsiz;       /* buffer size for  IN bulk ep */
	u16	       bout_bsiz;      /* buffer size for OUT bulk ep */

	/* data for interrupt in endpoint handling */
	u16	       ifnum;
	u8	       iin_b_tag;
	u8	      *iin_buffer;
	unsigned int   iin_ep;	       /* Interrupt in endpoint */
	int	       iin_interval;
	struct urb    *iin_urb;
	u16	       iin_max_packe_size;

	struct kref kref;
	struct mutex io_mutex;	/* only one i/o function running at a time */

	u32 out_transfer_size;
	u32 in_transfer_size;
	int in_status;

	wait_queue_head_t wait_bulk_in;

	enum talker_function_state talker_state;
	enum listener_function_state listener_state;

	unsigned short eos_char;
	unsigned short eos_mode;
	unsigned long start_jiffies; // jiffies at start of io

	struct timer_list bulk_timer;
	struct xugc_urb_ctx context;
	unsigned is_cic : 1;
	unsigned ren_state : 1;
	unsigned atn_asserted : 1;
};

#define to_xugc_data(d) container_of(d, struct xugc_priv, kref)

/* Forward declarations */

static void xugc_delete(struct kref *kref)
{
	struct xugc_priv *priv = to_xugc_data(kref);

	usb_put_dev(priv->usb_dev);
	kfree(priv);
}

static void xugc_bulk_complete(struct urb *urb)
{
	struct xugc_urb_ctx *context = urb->context;

	complete(&context->complete);
}

static void xugc_timeout_handler(COMPAT_TIMER_ARG_TYPE t)
{
	struct xugc_priv *priv = COMPAT_FROM_TIMER(priv, t, bulk_timer);
	struct xugc_urb_ctx *context = &priv->context;

	context->timed_out = 1;
	complete(&context->complete);
}

static int xugc_abort_bulk_in_tag(struct xugc_priv *priv, u8 tag)
{
	u8 *buffer;
	struct device *dev = &priv->intf->dev;
	int rv;
	int n;
	int actual;

	buffer = kmalloc(priv->bin_bsiz, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	rv = usb_control_msg(priv->usb_dev,
			     usb_rcvctrlpipe(priv->usb_dev, 0),
			     USBTMC_REQUEST_INITIATE_ABORT_BULK_IN,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT,
			     tag, priv->bulk_in,
			     buffer, 2, USB_CTRL_GET_TIMEOUT);

	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "INITIATE_ABORT_BULK_IN returned %x with tag %02x\n",
		buffer[0], buffer[1]);

	if (buffer[0] == USBTMC_STATUS_FAILED) {
		/* No transfer in progress and the Bulk-OUT FIFO is empty. */
		rv = 0;
		goto exit;
	}

	if (buffer[0] == USBTMC_STATUS_TRANSFER_NOT_IN_PROGRESS) {
		/* The device returns this status if either:
		 * - There is a transfer in progress, but the specified bTag
		 *   does not match.
		 * - There is no transfer in progress, but the Bulk-OUT FIFO
		 *   is not empty.
		 */
		rv = -ENOMSG;
		goto exit;
	}

	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "INITIATE_ABORT_BULK_IN returned %x\n",
			buffer[0]);
		rv = -EPERM;
		goto exit;
	}

	n = 0;

xugc_abort_bulk_in_status:
	dev_dbg(dev, "Reading from bulk in EP\n");

	/* Data must be present. So use low timeout 300 ms */
	actual = 0;
	rv = usb_bulk_msg(priv->usb_dev,
			  usb_rcvbulkpipe(priv->usb_dev, priv->bulk_in),
			  buffer, priv->bin_bsiz, &actual, 300);

	print_hex_dump_debug("xugc", DUMP_PREFIX_NONE, 16, 1,
			     buffer, actual, true);

	n++;

	if (rv < 0) {
		dev_err(dev, "usb_bulk_msg returned %d\n", rv);
		if (rv != -ETIMEDOUT)
			goto exit;
	}

	if (actual == priv->bin_bsiz)
		goto xugc_abort_bulk_in_status;

	if (n >= USBTMC_MAX_READS_TO_CLEAR_BULK_IN) {
		dev_err(dev, "Couldn't clear device buffer within %d cycles\n",
			USBTMC_MAX_READS_TO_CLEAR_BULK_IN);
		rv = -EPERM;
		goto exit;
	}

	rv = usb_control_msg(priv->usb_dev,
			     usb_rcvctrlpipe(priv->usb_dev, 0),
			     USBTMC_REQUEST_CHECK_ABORT_BULK_IN_STATUS,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT,
			     0, priv->bulk_in, buffer, 0x08,
			     USB_CTRL_GET_TIMEOUT);

	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "CHECK_ABORT_BULK_IN returned %x\n", buffer[0]);

	if (buffer[0] == USBTMC_STATUS_SUCCESS) {
		rv = 0;
		goto exit;
	}

	if (buffer[0] != USBTMC_STATUS_PENDING) {
		dev_err(dev, "CHECK_ABORT_BULK_IN returned %x\n", buffer[0]);
		rv = -EPERM;
		goto exit;
	}

	if ((buffer[1] & 1) > 0) {
		/* The device has 1 or more queued packets the Host can read */
		goto xugc_abort_bulk_in_status;
	}

	/* The Host must send CHECK_ABORT_BULK_IN_STATUS at a later time. */
	rv = -EAGAIN;
exit:
	kfree(buffer);
	return rv;
}

static int xugc_abort_bulk_in(struct xugc_priv *priv)
{
	return xugc_abort_bulk_in_tag(priv, priv->b_tag_last_read);
}

static int xugc_abort_bulk_out_tag(struct xugc_priv *priv,
				   u8 tag)
{
	struct device *dev = &priv->intf->dev;
	u8 *buffer;

	int rv;
	int n;

	buffer = kmalloc(8, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	rv = usb_control_msg(priv->usb_dev,
			     usb_rcvctrlpipe(priv->usb_dev, 0),
			     USBTMC_REQUEST_INITIATE_ABORT_BULK_OUT,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT,
			     tag, priv->bulk_out,
			     buffer, 2, USB_CTRL_GET_TIMEOUT);

	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "INITIATE_ABORT_BULK_OUT returned %x\n", buffer[0]);

	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "INITIATE_ABORT_BULK_OUT returned %x\n",
			buffer[0]);
		rv = -EPERM;
		goto exit;
	}

	n = 0;

xugc_abort_bulk_out_check_status:
	/* do not stress device with subsequent requests */
	msleep(50);
	rv = usb_control_msg(priv->usb_dev,
			     usb_rcvctrlpipe(priv->usb_dev, 0),
			     USBTMC_REQUEST_CHECK_ABORT_BULK_OUT_STATUS,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT,
			     0, priv->bulk_out, buffer, 0x08,
			     USB_CTRL_GET_TIMEOUT);
	n++;
	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "CHECK_ABORT_BULK_OUT returned %x\n", buffer[0]);

	if (buffer[0] == USBTMC_STATUS_SUCCESS)
		goto xugc_abort_bulk_out_clear_halt;

	if (buffer[0] == USBTMC_STATUS_PENDING && n < USBTMC_MAX_READS_TO_CLEAR_BULK_IN)
		goto xugc_abort_bulk_out_check_status;

	rv = -EPERM;
	goto exit;

xugc_abort_bulk_out_clear_halt:
	rv = usb_clear_halt(priv->usb_dev,
			    usb_sndbulkpipe(priv->usb_dev, priv->bulk_out));

	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}
	rv = 0;

exit:
	kfree(buffer);
	return rv;
}

static int xugc_abort_bulk_out(struct xugc_priv *priv)
{
	return xugc_abort_bulk_out_tag(priv, priv->b_tag_last_write);
}

static int xugc_control_out(struct xugc_priv *priv, int enable, unsigned int cmd)
{
	struct device *dev;
	u8 *buffer;
	u16 wValue;
	int rv;

	if (!priv->intf)
		return -ENODEV;

	dev = &priv->intf->dev;

	buffer = kmalloc(8, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	wValue = enable;

	rv = usb_control_msg(priv->usb_dev,
			     usb_rcvctrlpipe(priv->usb_dev, 0),
			     cmd,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			     wValue,
			     priv->ifnum,
			     buffer, 0x01, USB_CTRL_GET_TIMEOUT);
	if (rv < 0) {
		dev_err(dev, "%s: failed %d\n", __func__, rv);
		goto exit;
	} else if (rv != 1) {
		dev_warn(dev, "%s: returned %d\n", __func__, rv);
		rv = -EIO;
		goto exit;
	}

	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "%s: status returned %x\n", __func__, buffer[0]);
		rv = -EIO;
		goto exit;
	}
	rv = 0;

exit:
	kfree(buffer);
	return rv;
}

static int xugc_control_in(struct xugc_priv *priv, unsigned int cmd)
{
	struct device *dev;
	u8 *buffer;
	int rv;

	if (!priv->intf)
		return -ENODEV;

	buffer = kmalloc(8, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	dev = &priv->intf->dev;

	rv = usb_control_msg(priv->usb_dev,
			     usb_rcvctrlpipe(priv->usb_dev, 0),
			     cmd,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			     0,
			     priv->ifnum,
			     buffer, 0x02, USB_CTRL_GET_TIMEOUT);
	if (rv < 0) {
		dev_err(dev, "%s: failed %d\n", __func__, rv);
		goto exit;
	} else if (rv != 2) {
		dev_warn(dev, "%s: returned %d\n", __func__, rv);
		rv = -EIO;
		goto exit;
	}

	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "%s: status returned %x\n", __func__, buffer[0]);
		rv = -EIO;
		goto exit;
	}
	rv = buffer[1];

exit:
	kfree(buffer);
	return rv;
}

static struct urb *xugc_create_urb(size_t io_buffer_size)
{
	const size_t bufsize = io_buffer_size;
	u8 *dmabuf = NULL;
	struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);

	if (!urb)
		return NULL;

	dmabuf = kzalloc(bufsize, GFP_KERNEL);
	if (!dmabuf) {
		usb_free_urb(urb);
		return NULL;
	}

	urb->transfer_buffer = dmabuf;
	urb->transfer_buffer_length = bufsize;
	urb->transfer_flags |= URB_FREE_BUFFER;
	return urb;
}

/*
 * Sends a REQUEST_DEV_DEP_MSG_IN message on the Bulk-OUT endpoint.
 * @transfer_size: number of bytes to request from the device.
 *
 * See the USBTMC specification, Table 4.
 *
 * Also updates bTag_last_write.
 */
static int send_request_dev_dep_msg_in(struct gpib_board *board, u32 transfer_size,
				       int msec_timeout)
{
	struct xugc_priv *priv = board->private_data;
	int retval;
	u8 *buffer;
	int actual;

	buffer = kmalloc(USBTMC_HEADER_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	/* Setup IO buffer for REQUEST_DEV_DEP_MSG_IN message
	 * Refer to class specs for details
	 */
	buffer[0] = 2;	       /* REQUEST_DEV_DEP_MSG_IN */
	buffer[1] = priv->b_tag;
	buffer[2] = ~priv->b_tag;
	buffer[3] = 0; /* Reserved */
	buffer[4] = transfer_size >> 0;
	buffer[5] = transfer_size >> 8;
	buffer[6] = transfer_size >> 16;
	buffer[7] = transfer_size >> 24;
	if (priv->eos_mode & REOS) {
		buffer[8] = 2;
		buffer[9] = priv->eos_char;
	} else {
		buffer[8] = 0;
		buffer[9] = 0;
	}
	buffer[10] = 0; /* Reserved */
	buffer[11] = 0; /* Reserved */

	/* Send bulk URB */
	retval = usb_bulk_msg(priv->usb_dev,
			      usb_sndbulkpipe(priv->usb_dev, priv->bulk_out),
			      buffer, USBTMC_HEADER_SIZE,
			      &actual, msec_timeout);

	/* Store bTag (in case we need to abort) */
	priv->b_tag_last_write = priv->b_tag;

	/* Increment bTag -- and increment again if zero */
	priv->b_tag++;
	if (!priv->b_tag)
		priv->b_tag++;

	kfree(buffer);
	if (retval < 0)
		dev_err(&priv->intf->dev, "%s returned %d\n", __func__, retval);

	return retval;
}

#if 0
/* Keeping this in case we may need it */
static int xugc_clear(struct gpib_board *board)	 //XXX
{
	struct xugc_priv *priv = board->private_data;
	struct device *dev = &priv->intf->dev;
	u8 *buffer;
	int rv;
	int n;
	int actual = 0;
	int msec_timeout;

	dev = &priv->intf->dev;

	dev_dbg(dev, "Sending INITIATE_CLEAR request\n");

	buffer = kmalloc(max_t((u16)2, priv->bin_bsiz), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	rv = usb_control_msg(priv->usb_dev,
			     usb_rcvctrlpipe(priv->usb_dev, 0),
			     USBTMC_REQUEST_INITIATE_CLEAR,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			     0, 0, buffer, 1, USB_CTRL_GET_TIMEOUT);
	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "INITIATE_CLEAR returned %x\n", buffer[0]);

	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "INITIATE_CLEAR returned %x\n", buffer[0]);
		rv = -EPERM;
		goto exit;
	}

	n = 0;

xugc_clear_check_status:

	dev_dbg(dev, "Sending CHECK_CLEAR_STATUS request\n");

	rv = usb_control_msg(priv->usb_dev,
			     usb_rcvctrlpipe(priv->usb_dev, 0),
			     USBTMC_REQUEST_CHECK_CLEAR_STATUS,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			     0, 0, buffer, 2, USB_CTRL_GET_TIMEOUT);
	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "CHECK_CLEAR_STATUS returned %x\n", buffer[0]);

	if (buffer[0] == USBTMC_STATUS_SUCCESS)
		goto xugc_clear_bulk_out_halt;

	if (buffer[0] != USBTMC_STATUS_PENDING) {
		dev_err(dev, "CHECK_CLEAR_STATUS returned %x\n", buffer[0]);
		rv = -EPERM;
		goto exit;
	}

	if ((buffer[1] & 1) != 0) {
		do {
			dev_dbg(dev, "Reading from bulk in EP\n");
			msec_timeout = (board->usec_timeout + 999) / 1000;
			actual = 0;
			rv = usb_bulk_msg(priv->usb_dev,
					  usb_rcvbulkpipe(priv->usb_dev,
							  priv->bulk_in),
					  buffer, priv->bin_bsiz,
					  &actual, msec_timeout);

			print_hex_dump_debug("xugc ", DUMP_PREFIX_NONE,
					     16, 1, buffer, actual, true);

			n++;

			if (rv < 0) {
				dev_err(dev, "usb_control_msg returned %d\n",
					rv);
				goto exit;
			}
		} while ((actual == priv->bin_bsiz) &&
			 (n < USBTMC_MAX_READS_TO_CLEAR_BULK_IN));
	} else {
		/* do not stress device with subsequent requests */
		msleep(50);
		n++;
	}

	if (n >= USBTMC_MAX_READS_TO_CLEAR_BULK_IN) {
		dev_err(dev, "Couldn't clear device buffer within %d cycles\n",
			USBTMC_MAX_READS_TO_CLEAR_BULK_IN);
		rv = -EPERM;
		goto exit;
	}

	goto xugc_clear_check_status;

xugc_clear_bulk_out_halt:

	rv = usb_clear_halt(priv->usb_dev,
			    usb_sndbulkpipe(priv->usb_dev, priv->bulk_out));
	if (rv < 0) {
		dev_err(dev, "usb_clear_halt returned %d\n", rv);
		goto exit;
	}
	rv = 0;

exit:
	kfree(buffer);
	return rv;
}
#endif

static void xugc_interrupt(struct urb *urb)
{
	struct gpib_board *board = urb->context;
	struct xugc_priv *priv = board->private_data;
	struct device *dev = &priv->intf->dev;
	int status = urb->status;
	int rv;

	dev_dbg(dev, "int status: %d len %d\n",	status, urb->actual_length);

	switch (status) {
	case 0: /* SUCCESS */
		/* check for valid length */
		if (urb->actual_length < USBTMC_MIN_INT_IN_PACKET_SIZE) {
			dev_warn(dev, "short interrupt packet: %d bytes, min %d required\n",
				 urb->actual_length,
				 USBTMC_MIN_INT_IN_PACKET_SIZE);
			goto exit;
		}

		/* check for SRQ notification */
		if (priv->iin_buffer[0] == 0x81) {
			set_bit(SRQI_NUM, &board->status);  /* set_bit() is atomic */

			dev_dbg(dev, "srq received bTag %x stb %x\n",
				(unsigned int)priv->iin_buffer[0],
				(unsigned int)priv->iin_buffer[1]);
			wake_up_interruptible(&board->wait);
			goto exit;
		}
		dev_warn(dev, "invalid notification: %x\n", priv->iin_buffer[0]);
		break;
	case -EOVERFLOW:
		dev_err(dev, "overflow with length %d, actual length is %d\n",
			priv->iin_max_packe_size, urb->actual_length);
		fallthrough;
	default:
		/* urb terminated, clean up */
		dev_dbg(dev, "urb terminated, status: %d\n", status);
		return;
	}
exit:
	rv = usb_submit_urb(urb, GFP_ATOMIC);
	if (rv)
		dev_err(dev, "usb_submit_urb failed: %d\n", rv);
}

static void xugc_free_int(struct xugc_priv *priv)
{
	if (!priv->iin_urb)
		return;
	usb_kill_urb(priv->iin_urb);
	kfree(priv->iin_buffer);
	priv->iin_buffer = NULL;
	usb_free_urb(priv->iin_urb);
	priv->iin_urb = NULL;
	kref_put(&priv->kref, xugc_delete);
}

/****************************
 *			    *
 * gpib_interface functions *
 *			    *
 ****************************/

static int xugc_read(struct gpib_board *board, u8 *buf, size_t count, int *end, size_t *nbytes)
{
	struct xugc_priv *priv = board->private_data;
	struct device *dev = &priv->intf->dev;
	const u32 bufsize = (u32)priv->bin_bsiz;
	struct xugc_urb_ctx *context = &priv->context;
	struct urb *urb = NULL;
	u8 *buffer;
	int buflen;
	u32 bytes_to_read;	 /* # data bytes to read in this xfer			*/
	u32 bytes_transferred;	 /* # bytes xferred in urb: header + data [+ pad]	*/
	u32 bytes_read;		 /* # data bytes received in the usbtmc message		*/
	u32 remaining;		 /* total # data still remaining in current xfer	*/
	int done;		 /* total # data bytes read so far			*/
	u32 msec_timeout;
	int retval;

	mutex_lock(&priv->io_mutex);

	if (!priv->intf) {
		retval = -ENODEV;
		goto exit;
	}

	remaining = count;
	bytes_read = 0;
	done = 0;
	*end = 0;

	dev_dbg(dev, "%s(count:%zu)\n", __func__, count);

	urb = xugc_create_urb(priv->bin_bsiz);
	if (!urb) {
		retval = -ENOMEM;
		goto exit;
	}

	buffer = urb->transfer_buffer;
	buflen = urb->transfer_buffer_length;
	msec_timeout = (board->usec_timeout + 999) / 1000;
	priv->start_jiffies = jiffies;

	/* We will always get a full message since bufsize > xyphro buffer */

	while (remaining > 0) {
		if (remaining > bufsize - USBTMC_HEADER_SIZE - 3)
			bytes_to_read = bufsize - USBTMC_HEADER_SIZE - 3;
		else
			bytes_to_read = remaining;

		if (board->usec_timeout != 0)
			msec_timeout -= jiffies_to_msecs(jiffies - priv->start_jiffies);

		if (msec_timeout < 0) {
			retval = -ETIMEDOUT;
			goto exit;
		}

		retval = send_request_dev_dep_msg_in(board, bytes_to_read, msec_timeout);

		if (retval < 0) {
			xugc_abort_bulk_out(priv);
			goto exit;
		}

		if (board->usec_timeout != 0)
			msec_timeout -= jiffies_to_msecs(jiffies - priv->start_jiffies) - 1;

		if (msec_timeout < 0) {
			retval = -ETIMEDOUT;
			goto exit;
		}

		init_completion(&context->complete);
		context->timed_out = 0;

		usb_fill_bulk_urb(urb, priv->usb_dev,
				  usb_rcvbulkpipe(priv->usb_dev, priv->bulk_in),
				  urb->transfer_buffer, buflen,
				  xugc_bulk_complete, context);

		if (msec_timeout)
			mod_timer(&priv->bulk_timer, priv->start_jiffies +
				  msecs_to_jiffies(msec_timeout));

		retval = usb_submit_urb(urb, GFP_KERNEL);

		if (retval) {
			dev_err(dev, "%s: submit urb failed %d\n", __func__, retval);
			xugc_abort_bulk_in(priv);
			goto exit;
		}

		if (wait_for_completion_interruptible(&context->complete)) {
			dev_dbg(dev, "wait read complete interrupted\n");
			xugc_abort_bulk_in(priv);
			usb_kill_urb(urb);
			mutex_unlock(&priv->io_mutex);
			return -ERESTARTSYS;
		}

		if (context->timed_out) {
			dev_dbg(dev, "read timedout\n");
			xugc_abort_bulk_in(priv);
			usb_kill_urb(urb);
			retval = -ETIMEDOUT;
			goto exit;
		} else {
			retval = urb->status;
			bytes_transferred = urb->actual_length;
		}

		dev_dbg(dev, "bulk_in retval(%d), bytes_transferred(%u) timeout(%u)\n",
			retval, bytes_transferred, msec_timeout);

		/* Store bTag (in case we need to abort) */
		priv->b_tag_last_read = priv->b_tag;

		/* Sanity checks for the header */
		if (bytes_transferred < USBTMC_HEADER_SIZE) {
			dev_err(dev, "Device sent too small first packet: %u < %u\n",
				bytes_transferred, USBTMC_HEADER_SIZE);
			xugc_abort_bulk_in(priv);
			goto exit;
		}

		if (buffer[0] != 2) {
			dev_err(dev, "Device sent reply with wrong MsgID: %u != 2\n", buffer[0]);
			xugc_abort_bulk_in(priv);
			goto exit;
		}

		if (buffer[1] != priv->b_tag_last_write) {
			dev_err(dev, "Device sent reply with wrong bTag: %u != %u\n",
				buffer[1], priv->b_tag_last_write);
			xugc_abort_bulk_in(priv);
			goto exit;
		}

		/* How many characters did the adapter send this time? */
		bytes_read = buffer[4] +
			(buffer[5] << 8) +
			(buffer[6] << 16) +
			(buffer[7] << 24);

		dev_dbg(dev, "Bulk-IN header: bytes_read(%u), bTransAttr(%u)\n",
			bytes_read, buffer[8]);

		if (bytes_read > bytes_to_read) {
			dev_err(dev, "Device wants to return more data than requested: %u > %u\n",
				bytes_read, bytes_to_read);
			xugc_abort_bulk_in(priv);
			goto exit;
		}

		remaining -= bytes_read;

		memcpy(buf + done, &buffer[USBTMC_HEADER_SIZE], bytes_read);

		done += bytes_read;

		if (buffer[8]) { /* if eos (aka termchar) or eoi (aka eom) */
			remaining = 0;
			*end = 1;
		}
	}
	retval = 0;

exit:
	if (msec_timeout) {
		if (timer_pending(&priv->bulk_timer))
			COMPAT_DEL_TIMER_SYNC(&priv->bulk_timer);
	}
	*nbytes = done;
	mutex_unlock(&priv->io_mutex);
	usb_free_urb(urb);
	return retval;
}

static int xugc_write(struct gpib_board *board, u8 *buf, size_t count, int send_eoi,
		      size_t *bytes_written)
{
	struct xugc_priv *priv = board->private_data;
	struct xugc_urb_ctx *context = &priv->context;
	struct device *dev = &priv->intf->dev;
	struct usb_device *usb_dev;
	struct urb *urb = NULL;
	u8 *buffer;		      /* Urb buffer					*/
	u32 buflen;		      /* Total size of urb buffer			*/
	u32 bytes_to_transfer;	      /* # bytes to send this round hdr + data + [pad]	*/
	u32 bytes_to_send;	      /* # data bytes to send this round		*/
	u32 remaining;		      /* # data bytes remaining to send			*/
	u32 done;		      /* # data bytes already sent so far		*/
	int eoi;		      /* Are we sending eoi this round			*/
	int timeout_msecs = 0;
	int retval = 0;

	*bytes_written = 0;
	if (!priv->intf)
		return -ENODEV;

	priv = board->private_data;
	usb_dev = interface_to_usbdev(priv->intf);

	mutex_lock(&priv->io_mutex);

	dev_dbg(dev, "%s(count:%zu)\n", __func__, count);

	remaining = count;
	done = 0;

	if (!count)
		goto exit;

	urb = xugc_create_urb(priv->bout_bsiz);
	if (!urb) {
		retval = -ENOMEM;
		goto exit;
	}

	buffer = urb->transfer_buffer;
	buflen = urb->transfer_buffer_length;
	timeout_msecs = (board->usec_timeout + 999) / 1000;
	priv->start_jiffies = jiffies;

	while (remaining > 0) {
		if (remaining > buflen - USBTMC_HEADER_SIZE - 3) {
			bytes_to_send = buflen - USBTMC_HEADER_SIZE - 3;
			buffer[8] = 0;
			eoi = 0;
		} else {
			bytes_to_send = remaining;
			buffer[8] = send_eoi;
			eoi  = send_eoi;
		}

		/* Setup IO buffer for DEV_DEP_MSG_OUT message */
		buffer[0] = 1;
		buffer[1] = priv->b_tag;
		buffer[2] = ~priv->b_tag;
		buffer[3] = 0; /* Reserved */
		buffer[4] = bytes_to_send >> 0;
		buffer[5] = bytes_to_send >> 8;
		buffer[6] = bytes_to_send >> 16;
		buffer[7] = bytes_to_send >> 24;
		/* buffer[8] is set above... */
		buffer[9] = 0; /* Reserved */
		buffer[10] = 0; /* Reserved */
		buffer[11] = 0; /* Reserved */

		memcpy(&buffer[USBTMC_HEADER_SIZE], buf + done, bytes_to_send);

		bytes_to_transfer = roundup(USBTMC_HEADER_SIZE + bytes_to_send, 4);

		dev_dbg(dev, "%s(data bytes_to_send:%u bytes_to_xfer:%u eoi:%d)\n",
			__func__, (unsigned int)bytes_to_send,
			(unsigned int)bytes_to_transfer, eoi);

		init_completion(&context->complete);
		context->timed_out = 0;

		usb_fill_bulk_urb(urb, priv->usb_dev,
				  usb_sndbulkpipe(priv->usb_dev, priv->bulk_out),
				  urb->transfer_buffer, bytes_to_transfer,
				  xugc_bulk_complete, context);

		if (timeout_msecs)
			mod_timer(&priv->bulk_timer, priv->start_jiffies +
				  msecs_to_jiffies(timeout_msecs));

		retval = usb_submit_urb(urb, GFP_KERNEL);

		if (retval < 0) {
			dev_err(dev, "Failed to submit urb, error %d\n", (int)retval);
			xugc_abort_bulk_out(priv);
			goto exit;
		}

		priv->b_tag_last_write = priv->b_tag;
		priv->b_tag++;

		if (!priv->b_tag)
			priv->b_tag++;

		if (wait_for_completion_interruptible(&context->complete)) {
			dev_dbg(dev, "wait write complete interrupted\n");
			xugc_abort_bulk_out(priv);
			mutex_unlock(&priv->io_mutex);
			return -ERESTARTSYS;
		}

		if (context->timed_out) {
			dev_dbg(dev, "write timedout\n");
			xugc_abort_bulk_out(priv);
			usb_kill_urb(urb);
			retval = -ETIMEDOUT;
			goto exit;
		} else {
			retval = urb->status;
			dev_dbg(dev, "write urb xfer len %d retval %d\n",
				urb->actual_length, retval);
		}

		done += bytes_to_send;
		remaining -= bytes_to_send;
	}

exit:
	if (timeout_msecs) {
		if (timer_pending(&priv->bulk_timer))
			COMPAT_DEL_TIMER_SYNC(&priv->bulk_timer);
	}
	*bytes_written = done;
	usb_free_urb(urb);
	mutex_unlock(&priv->io_mutex);
	return retval;
}

static int xugc_command(struct gpib_board *board, u8 *buffer, size_t length, size_t *bytes_written)
{
	struct xugc_priv *priv = board->private_data;
	int i, retval;

	retval = xugc_write(board, buffer, length, 0, bytes_written);

	for (i = 0; i < length; i++) {
		if (buffer[i] == UNT) {
			priv->talker_state = talker_idle;
		} else {
			if (buffer[i] == UNL) {
				priv->listener_state = listener_idle;
			} else {
				if (buffer[i] == (MTA(board->pad))) {
					priv->talker_state = talker_addressed;
					priv->listener_state = listener_idle;
				} else if (buffer[i] == (MLA(board->pad))) {
					priv->listener_state = listener_addressed;
					priv->talker_state = talker_idle;
				}
			}
		}
	}

	return retval;
}

static int set_atn(struct gpib_board *board, int atn_asserted)
{
	struct xugc_priv *priv = board->private_data;
	int retval;

	if (!priv->intf)
		return -ENODEV;

	dev_dbg(&priv->intf->dev, "set atn %d\n", atn_asserted);

	if (priv->listener_state != listener_idle &&
	    priv->talker_state != talker_idle) {
		dev_err(board->gpib_dev, "listener/talker state machine conflict\n");
	}

	retval = xugc_control_out(priv, atn_asserted ? 0 : 1, SET_ATN_STATE);

	if (retval < 0)
		return retval;

	priv->atn_asserted = atn_asserted;

	if (atn_asserted) {
		if (priv->listener_state == listener_active)
			priv->listener_state = listener_addressed;
		if (priv->talker_state == talker_active)
			priv->talker_state = talker_addressed;
	} else {
		if (priv->listener_state == listener_addressed)
			priv->listener_state = listener_active;

		if (priv->talker_state == talker_addressed)
			priv->talker_state = talker_active;
	}

	return 0;
}

static int xugc_take_control(struct gpib_board *board, int synchronous)
{
	return set_atn(board, 1);
}

static int xugc_go_to_standby(struct gpib_board *board)
{
	return set_atn(board, 0);
}

static void xugc_interface_clear(struct gpib_board *board, int assert)
{
	struct xugc_priv *priv = board->private_data;

	if (!priv->intf)
		return; //  -ENODEV

	dev_dbg(&priv->intf->dev, "IFC %d\n", assert);

	xugc_control_out(priv, assert ? 0 : 1, SET_IFC_STATE);

	if (assert) {
		priv->talker_state = talker_idle;
		priv->listener_state = listener_idle;
		set_bit(CIC_NUM, &board->status);
		priv->is_cic = 1;
	}
}

static void xugc_remote_enable(struct gpib_board *board, int enable)
{
	struct xugc_priv *priv = board->private_data;

	if (!priv->intf)
		return; //  -ENODEV

	dev_dbg(&priv->intf->dev, "REN %d\n", enable);

	priv->ren_state = enable ? 1 : 0;
	xugc_control_out(priv, priv->ren_state, SET_REN_STATE);
	if (priv->ren_state) // should actually only do this on first entry to LADS
		set_bit(REM_NUM, &board->status);
	else
		clear_bit(REM_NUM, &board->status);
}

static int xugc_request_system_control(struct gpib_board *board, int request_control)
{
	struct xugc_priv *priv = board->private_data;

	if (!priv->intf)
		return -ENODEV;

	if (!request_control) // can only be system controller
		return -EINVAL;

	xugc_interface_clear(board, 0);

	xugc_remote_enable(board, 0);

	set_atn(board, 0);

	return 0;
}

static int xugc_enable_eos(struct gpib_board *board, u8 eos_byte, int compare_8_bits)
{
	struct xugc_priv *priv = board->private_data;

	if (!priv->intf)
		return -ENODEV;
	if (compare_8_bits == 0)
		return -EOPNOTSUPP;

	priv->eos_char = eos_byte;
	priv->eos_mode = REOS | BIN;
	return 0;
}

static void xugc_disable_eos(struct gpib_board *board)
{
	struct xugc_priv *priv = board->private_data;

	priv->eos_mode &= ~REOS;
}

static unsigned int xugc_update_status(struct gpib_board *board, unsigned int clear_mask)
{
	struct xugc_priv *priv = board->private_data;
	struct usb_device *usb_dev;
	int line_status;

	if (!priv->intf)
		return -ENODEV;

	usb_dev = interface_to_usbdev(priv->intf);
	board->status &= ~clear_mask;

	if (priv->is_cic)
		set_bit(CIC_NUM, &board->status);
	else
		clear_bit(CIC_NUM, &board->status);

	line_status = xugc_line_status(board);

	if ((line_status & VALID_ATN) && ((!(line_status & BUS_ATN)) == priv->atn_asserted))
		dev_warn(&usb_dev->dev, "ATN state confusion\n");

	if (priv->atn_asserted)
		set_bit(ATN_NUM, &board->status);
	else
		clear_bit(ATN_NUM, &board->status);

	if (line_status & VALID_SRQ) {
		if (line_status & BUS_SRQ)
			set_bit(SRQI_NUM, &board->status);
		else
			clear_bit(SRQI_NUM, &board->status);
	}

	if (priv->talker_state == talker_active || priv->talker_state == talker_addressed)
		set_bit(TACS_NUM, &board->status);
	else
		clear_bit(TACS_NUM, &board->status);

	if (priv->listener_state == listener_active || priv->listener_state == listener_addressed)
		set_bit(LACS_NUM, &board->status);
	else
		clear_bit(LACS_NUM, &board->status);

	return board->status;
}

static int xugc_primary_address(struct gpib_board *board, unsigned int address)
{
	struct xugc_priv *priv = board->private_data;
	int retval;

	if (!priv->intf)
		return -ENODEV;

	dev_dbg(&priv->intf->dev, "Set primary address %d\n", address);

	if (address > 30)
		return -EINVAL;

	retval = xugc_control_out(priv, address, SET_CONTROLLER_ADDRESS);

	if (retval < 0)
		return retval;

	return 0;
}

static int xugc_secondary_address(struct gpib_board *board,
				  unsigned int address, int enable)
{
	if (enable)
		board->sad = address;
	return 0;
}

static int xugc_parallel_poll(struct gpib_board *board, u8 *result)
{
	return -ENOENT;
}

static void xugc_parallel_poll_configure(struct gpib_board *board, u8 config)
{
	//board can only be system controller
	return;// 0;
}

static void xugc_parallel_poll_response(struct gpib_board *board, int ist)
{
	//board can only be system controller
	return;// 0;
}

static void xugc_serial_poll_response(struct gpib_board *board, u8 status)
{
	//board can only be system controller
	return;// 0;
}

static u8 xugc_serial_poll_status(struct gpib_board *board)
{
	//board can only be system controller
	return 0;
}

static void xugc_return_to_local(struct gpib_board *board)
{
	//board can only be system controller
	return;// 0;
}

static int xugc_line_status(const struct gpib_board *board)
{
	struct xugc_priv *priv = board->private_data;
	int status = VALID_ALL;
	int lines;

	if (!priv->intf)
		return -ENODEV;

	lines = xugc_control_in(priv, READ_CONTROL_LINES);
	if (lines > 0)
		status |= (~lines & 0xff) << 8;
	else
		status = 0;

	return status;
}

static int xugc_t1_delay(struct gpib_board *board, unsigned int nanosec)
{
	struct xugc_priv *priv = board->private_data;

	if (!priv->intf)
		return -ENODEV;

	return board->t1_nano_sec;
}

static int xugc_allocate_private(struct gpib_board *board)
{
	board->private_data = kzalloc(sizeof(struct xugc_priv), GFP_KERNEL);
	if (!board->private_data)
		return -ENOMEM;
	return 0;
}

static void xugc_free_private(struct gpib_board *board)
{
	kfree(board->private_data);
	board->private_data = NULL;
}

static inline int xugc_device_match(struct usb_interface *interface,
				    const struct gpib_board_config *config)
{
	struct usb_device * const usbdev = interface_to_usbdev(interface);

	if (gpib_match_device_path(&interface->dev, config->device_path) == 0)
		return 0;
	if (config->serial_number &&
	    strcmp(usbdev->serial, config->serial_number) != 0)
		return 0;

	return 1;
}

static int xugc_attach(struct gpib_board *board, const struct gpib_board_config *config)
{
	int retval;
	int i;
	struct xugc_priv *priv;
	struct usb_device *usb_dev;
	struct usb_host_interface *iface_desc;

	if (mutex_lock_interruptible(&xugc_hotplug_lock))
		return -ERESTARTSYS;

	retval = xugc_allocate_private(board);
	if (retval < 0) {
		mutex_unlock(&xugc_hotplug_lock);
		return retval;
	}
	priv = board->private_data;

	for (i = 0; i < MAX_NUM_XUGC_INTERFACES; ++i) {
		if (xugc_interfaces[i] &&
		    !usb_get_intfdata(xugc_interfaces[i]) &&
		    xugc_device_match(xugc_interfaces[i], config)) {
			priv->intf = xugc_interfaces[i];
			usb_set_intfdata(xugc_interfaces[i], board);
			usb_dev = interface_to_usbdev(priv->intf);
			break;
		}
	}
	if (i == MAX_NUM_XUGC_INTERFACES) {
		dev_err(board->gpib_dev, "No supported adapters found\n");
		retval = -ENODEV;
		goto attach_fail;
	}

	priv->usb_dev = usb_dev;
	kref_init(&priv->kref);
	mutex_init(&priv->io_mutex);

	/* Initialize USBTMC bTag and other fields */
	priv->b_tag	= 1;
	/*  2 <= bTag <= 127   USBTMC-USB488 subclass specification 4.3.1 */
	priv->iin_b_tag = 2;

	/* USBTMC devices have only one setting, so use that */
	iface_desc = priv->intf->cur_altsetting;
	priv->ifnum = iface_desc->desc.bInterfaceNumber;

	/* Assign endpoint data as per firmware */
	priv->iin_ep = 0x81;
	priv->iin_max_packe_size = 2;
	priv->iin_interval = 5;

	priv->bulk_in = 0x82;
	priv->bulk_out = 0x3;

	/* Assign transfer buffer sizes */
	priv->bin_bsiz = io_buffer_size;
	priv->bout_bsiz = io_buffer_size;

	/* allocate int urb */
	priv->iin_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!priv->iin_urb) {
		retval = -ENOMEM;
		goto attach_fail;
	}

	/* Protect interrupt in endpoint data until iin_urb is freed */
	kref_get(&priv->kref);

	/* allocate buffer for interrupt in */
	priv->iin_buffer = kmalloc(priv->iin_max_packe_size, GFP_KERNEL);
	if (!priv->iin_buffer) {
		retval = -ENOMEM;
		goto attach_fail;
	}

	/* fill interrupt urb */
	usb_fill_int_urb(priv->iin_urb, priv->usb_dev,
			 usb_rcvintpipe(priv->usb_dev, priv->iin_ep),
			 priv->iin_buffer, priv->iin_max_packe_size,
			 xugc_interrupt,
			 board, priv->iin_interval);

	retval = usb_submit_urb(priv->iin_urb, GFP_KERNEL);
	if (retval) {
		dev_err(&usb_dev->dev, "Failed to submit iin_urb\n");
		goto attach_fail;
	}

	COMPAT_TIMER_SETUP(&priv->bulk_timer, xugc_timeout_handler, 0);

	board->t1_nano_sec = 2000;

	xugc_primary_address(board, board->pad);  // tell the controller our address

	dev_info(&usb_dev->dev, "bus %d dev num %d attached to gpib%d, interface %i\n",
		 usb_dev->bus->busnum, usb_dev->devnum, board->minor, i);
	mutex_unlock(&xugc_hotplug_lock);
	return retval;

attach_fail:
	xugc_free_int(priv);
	xugc_free_private(board);
	mutex_unlock(&xugc_hotplug_lock);
	return retval;
}

static void xugc_detach(struct gpib_board *board)
{
	struct xugc_priv *priv;

	mutex_lock(&xugc_hotplug_lock);

	priv = board->private_data;
	if (priv) {
		if (priv->intf)
			usb_set_intfdata(priv->intf, NULL);

		xugc_free_int(priv);
		xugc_free_private(board);
	}
	dev_dbg(board->gpib_dev, "Detached\n");
	mutex_unlock(&xugc_hotplug_lock);
}

static struct gpib_interface xugc_gpib_interface = {
	.name = "xyphro_ugc",
	.attach = xugc_attach,
	.detach = xugc_detach,
	.read = xugc_read,
	.write = xugc_write,
	.command = xugc_command,
	.take_control = xugc_take_control,
	.go_to_standby = xugc_go_to_standby,
	.request_system_control = xugc_request_system_control,
	.interface_clear = xugc_interface_clear,
	.remote_enable = xugc_remote_enable,
	.enable_eos = xugc_enable_eos,
	.disable_eos = xugc_disable_eos,
	.parallel_poll = xugc_parallel_poll,
	.parallel_poll_configure = xugc_parallel_poll_configure,
	.parallel_poll_response = xugc_parallel_poll_response,
	.local_parallel_poll_mode = NULL, // XXX
	.line_status = xugc_line_status,
	.update_status = xugc_update_status,
	.primary_address = xugc_primary_address,
	.secondary_address = xugc_secondary_address,
	.serial_poll_response = xugc_serial_poll_response,
	.serial_poll_status = xugc_serial_poll_status,
	.t1_delay = xugc_t1_delay,
	.return_to_local = xugc_return_to_local,
	.no_7_bit_eos = 1,
	.skip_check_for_command_acceptors = 1
};

static int xugc_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	int i;
	char *path;
	static const int path_length = 1024;
	struct usb_device *usb_dev;

	if (mutex_lock_interruptible(&xugc_hotplug_lock))
		return -ERESTARTSYS;
	usb_dev = usb_get_dev(interface_to_usbdev(interface));
	for (i = 0; i < MAX_NUM_XUGC_INTERFACES; ++i) {
		if (!xugc_interfaces[i]) {
			xugc_interfaces[i] = interface;
			usb_set_intfdata(interface, NULL);
			dev_dbg(&usb_dev->dev, "set bus interface %i to address 0x%p\n",
				i, interface);
			break;
		}
	}
	if (i == MAX_NUM_XUGC_INTERFACES) {
		usb_put_dev(usb_dev);
		mutex_unlock(&xugc_hotplug_lock);
		dev_err(&usb_dev->dev, "out of space in xugc_interfaces[]\n");
		return -1;
	}
	path = kmalloc(path_length, GFP_KERNEL);
	if (!path) {
		usb_put_dev(usb_dev);
		mutex_unlock(&xugc_hotplug_lock);
		return -ENOMEM;
	}
	usb_make_path(usb_dev, path, path_length);
	dev_info(&usb_dev->dev, "probe succeeded for path: %s\n", path);
	kfree(path);
	mutex_unlock(&xugc_hotplug_lock);
	return 0;
}

static void xugc_disconnect(struct usb_interface *intf)
{
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	int i;

	mutex_lock(&xugc_hotplug_lock);

	for (i = 0; i < MAX_NUM_XUGC_INTERFACES; ++i) {
		if (xugc_interfaces[i] == intf) {
			struct gpib_board *board = usb_get_intfdata(intf);

			if (board) {
				struct xugc_priv *priv = board->private_data;

				if (priv) {
					xugc_free_int(priv);
					priv->intf = NULL;
				}
			}
			xugc_interfaces[i] = NULL;
			break;
		}
	}
	if (i == MAX_NUM_XUGC_INTERFACES)
		dev_err(&usb_dev->dev, "unable to find interface - bug?\n");

	usb_put_dev(usb_dev);

	mutex_unlock(&xugc_hotplug_lock);
	pr_info("Experimental xugc driver unloaded");
}

static int xugc_suspend(struct usb_interface *intf, pm_message_t message)
{
	int i, retval;
	struct gpib_board *board;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct xugc_priv *priv;

	mutex_lock(&xugc_hotplug_lock);

	for (i = 0; i < MAX_NUM_XUGC_INTERFACES; ++i) {
		if (xugc_interfaces[i] == intf)	{
			board = usb_get_intfdata(intf);
			if (board)
				break;
		}
	}
	if (i == MAX_NUM_XUGC_INTERFACES) {
		retval = -ENOENT;
		goto suspend_exit;
	}
	priv = board->private_data;

	if (!priv)
		goto suspend_exit;

	if (priv->iin_urb)
		usb_kill_urb(priv->iin_urb);

	dev_dbg(&usb_dev->dev, "Suspended");

suspend_exit:
	mutex_unlock(&xugc_hotplug_lock);

	return retval;
}

static int xugc_resume(struct usb_interface *intf)
{
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct xugc_priv *priv;
	struct gpib_board *board;
	int i, retval = 0;

	mutex_lock(&xugc_hotplug_lock);

	for (i = 0; i < MAX_NUM_XUGC_INTERFACES; ++i)	{
		if (xugc_interfaces[i] == intf) {
			board = usb_get_intfdata(intf);
			if (board)
				break;
		}
	}
	if (i == MAX_NUM_XUGC_INTERFACES) {
		retval = -ENOENT;
		goto resume_exit;
	}

	priv = board->private_data;

	if (priv->iin_urb)
		retval = usb_submit_urb(priv->iin_urb, GFP_KERNEL);
	if (retval)
		dev_err(&usb_dev->dev, "Failed to submit iin_urb\n");
	else
		dev_dbg(&usb_dev->dev, "Resumed");

resume_exit:
	mutex_unlock(&xugc_hotplug_lock);
	return retval;
}

static struct usb_driver xyphro_ugc_bus_driver = {
	.name = DRV_NAME,
	.probe = xugc_probe,
	.disconnect = xugc_disconnect,
	.suspend = xugc_suspend,
	.resume = xugc_resume,
	.id_table = xugc_device_table,
};

static int __init xugc_init_module(void)
{
	int i;
	int ret;

	for (i = 0; i < MAX_NUM_XUGC_INTERFACES; i++)
		xugc_interfaces[i] = NULL;

	ret = usb_register(&xyphro_ugc_bus_driver);
	if (ret) {
		pr_err("usb_register failed: error = %d\n", ret);
		return ret;
	}

	ret = gpib_register_driver(&xugc_gpib_interface, THIS_MODULE);
	if (ret) {
		pr_err("gpib_register_driver failed: error = %d\n", ret);
		usb_deregister(&xyphro_ugc_bus_driver);
		return ret;
	}

	return 0;
}

static void __exit xugc_exit_module(void)
{
	gpib_unregister_driver(&xugc_gpib_interface);
	usb_deregister(&xyphro_ugc_bus_driver);
}

module_init(xugc_init_module);
module_exit(xugc_exit_module);
