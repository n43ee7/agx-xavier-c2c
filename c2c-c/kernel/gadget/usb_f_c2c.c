// SPDX-License-Identifier: GPL-2.0
/*
 * usb_f_c2c.ko: the gadget (USB device) side, runs on the slave.
 *
 * Adds a configfs USB function called "c2c". Each instance is one vendor
 * interface (ff/43/xx) with a bulk IN and a bulk OUT endpoint, SuperSpeed,
 * burst 16. Can be the only function in the gadget, or get added to NVIDIA's
 * "l4t" composite gadget so RNDIS/NCM/ACM still work next to it.
 *
 * The master picks what the data engine does with a vendor request:
 *   IDLE/SINK  OUT armed, data gets counted and dropped
 *   SOURCE     IN sends buffers back to back           (benchmarks)
 *   BIDIR      SINK + SOURCE                           (benchmarks)
 *   LOOPBACK   OUT echoed back on IN, buffers swapped, no copy (latency)
 *   STREAM     byte stream to/from /dev/c2c0 here (the actual link)
 *
 * You can have a few instances, one per configfs function. "c2c.0" is the
 * link (proto 01, /dev/c2c0, c2cd uses it). "c2c.chN" are raw channels
 * (proto 02, /dev/c2cchN) with sessions, see c2c_proto.h. Every instance
 * gets its own endpoints, requests and char device.
 *
 * /dev/c2c0 and /dev/c2cchN are misc devices that live as long as the instance:
 *   read/write/poll move STREAM data. You get -ENOTCONN / EPOLLERR if the
 *   USB link isn't configured or the master hasn't picked STREAM yet.
 *   ioctl(C2C_IOC_GADGET_STATUS) = local stats, speed==0 means no link.
 *   ioctl(C2C_IOC_ACCEPT) (channels only) waits for the next session.
 *
 * Who owns a request: always exactly one of
 *   queued (UDC has it) | on a list (in_free/out_idle/out_parked/rx_ready) |
 *   held (read()/write() is copying it, rctx->held).
 * Re-enable skips held requests. Whoever holds one files it back when done
 * and checks the generation counter to see if things changed in between.
 *
 * Rules I learned the hard way on tegra-xudc (each of these hung or crashed
 * a board at some point):
 *  - never requeue from an error completion. The UDC completes requests out
 *    of a "while (!list_empty(&ep->queue))" nuke loop, requeue inline there
 *    and it spins forever holding the controller lock. Errored requests get
 *    parked and re-armed later from delayed work.
 *  - never use ep->driver_data, configfs autoconfig_reset() wipes it.
 *  - mode switches never cancel queued transfers (USB3 sequence numbers get
 *    out of sync). The master drains the leftovers instead.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/configfs.h>
#include <linux/workqueue.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>

#include "c2c_proto.h"

static unsigned int buflen = 65536;
module_param(buflen, uint, 0444);
MODULE_PARM_DESC(buflen, "bytes per gadget request (multiple of 1024)");

static unsigned int qlen = 32;
module_param(qlen, uint, 0444);
MODULE_PARM_DESC(qlen, "requests per endpoint");

static unsigned int maxburst = 15;
module_param(maxburst, uint, 0444);
MODULE_PARM_DESC(maxburst, "SuperSpeed bMaxBurst 0..15 (packets per burst - 1)");

static unsigned int trace;
module_param(trace, uint, 0644);
MODULE_PARM_DESC(trace, "log the next N data/control events (debug)");

#define C2C_TRACE(fmt, ...)						\
	do {								\
		if (unlikely(READ_ONCE(trace))) {			\
			WRITE_ONCE(trace, trace - 1);			\
			pr_info("c2c: " fmt, ##__VA_ARGS__);		\
		}							\
	} while (0)

struct f_c2c;

/*
 * char device state, one slot per instance. Static on purpose so a sleeping
 * waiter or an open file can never outlive the memory it's using.
 */
#define C2C_MAX_SLOTS (1 + C2C_MAX_CHANNELS)
struct c2c_slot {
	int idx;                     /* 0 = link, n = data channel n-1        */
	bool used;
	struct rw_semaphore cur_sem; /* keeps cur alive vs. unbind           */
	struct f_c2c *cur;
	wait_queue_head_t rx_wait, tx_wait;
	struct mutex rd_mutex, wr_mutex;
	atomic_t opened;
	u32 session;                 /* channels: +1 on every host SET_MODE    */
	u32 fsession;                /* session the opener accepted           */
	char name[16];
	struct miscdevice misc;
	bool misc_registered;
};
static struct c2c_slot c2c_slots[C2C_MAX_SLOTS];
static DEFINE_MUTEX(c2c_slots_mutex);

struct c2c_rctx {               /* per request info (req->context)       */
	struct f_c2c *c2c;
	u32 off;                /* STREAM rx: how much read() took so far */
	bool held;              /* read()/write() has it right now       */
};

struct f_c2c {
	struct usb_function function;
	struct c2c_slot *slot;
	struct usb_ep *in_ep, *out_ep;
	struct usb_request **in_reqs, **out_reqs;
	struct c2c_rctx *in_ctx, *out_ctx;
	unsigned int nreq, blen;

	spinlock_t lock;
	bool active;
	u32 mode;
	u32 gen;                     /* +1 on enable/disable/mode change    */
	struct list_head in_free;    /* idle IN requests                     */
	struct list_head out_parked; /* LOOPBACK: OUT waiting for an IN      */
	struct list_head out_idle;   /* OUT not queued (error / no link)     */
	struct list_head rx_ready;   /* STREAM: OUT data waiting for read()  */
	struct delayed_work rearm;   /* requeue, but not from a completion   */

	u64 rx_bytes, tx_bytes, rx_reqs, tx_reqs;
	u32 rx_errs, tx_errs;
};

static inline struct f_c2c *func_to_c2c(struct usb_function *f)
{
	return container_of(f, struct f_c2c, function);
}

static inline void c2c_wake_all(struct c2c_slot *sl)
{
	wake_up_interruptible_all(&sl->rx_wait);
	wake_up_interruptible_all(&sl->tx_wait);
}

/* ------------------------------------------------------------------ */
/* descriptors                                                         */

static struct usb_interface_descriptor c2c_intf = {
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bNumEndpoints = 2,
	.bInterfaceClass = C2C_IF_CLASS,
	.bInterfaceSubClass = C2C_IF_SUBCLASS,
	.bInterfaceProtocol = C2C_IF_PROTO,
};

static struct usb_endpoint_descriptor fs_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor fs_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_OUT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
};

static struct usb_descriptor_header *fs_descs[] = {
	(struct usb_descriptor_header *)&c2c_intf,
	(struct usb_descriptor_header *)&fs_out_desc,
	(struct usb_descriptor_header *)&fs_in_desc,
	NULL,
};

static struct usb_endpoint_descriptor hs_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(512),
};

static struct usb_endpoint_descriptor hs_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(512),
};

static struct usb_descriptor_header *hs_descs[] = {
	(struct usb_descriptor_header *)&c2c_intf,
	(struct usb_descriptor_header *)&hs_out_desc,
	(struct usb_descriptor_header *)&hs_in_desc,
	NULL,
};

static struct usb_endpoint_descriptor ss_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor ss_in_comp = {
	.bLength = USB_DT_SS_EP_COMP_SIZE,
	.bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst = 15,
};

static struct usb_endpoint_descriptor ss_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor ss_out_comp = {
	.bLength = USB_DT_SS_EP_COMP_SIZE,
	.bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst = 15,
};

static struct usb_descriptor_header *ss_descs[] = {
	(struct usb_descriptor_header *)&c2c_intf,
	(struct usb_descriptor_header *)&ss_out_desc,
	(struct usb_descriptor_header *)&ss_out_comp,
	(struct usb_descriptor_header *)&ss_in_desc,
	(struct usb_descriptor_header *)&ss_in_comp,
	NULL,
};

/* ------------------------------------------------------------------ */
/* data path                                                           */

static inline bool c2c_req_flushed(int status)
{
	return status == -ESHUTDOWN || status == -ECONNRESET ||
	       status == -ECONNABORTED;
}

static void c2c_queue(struct f_c2c *c2c, struct usb_ep *ep,
		      struct usb_request *req)
{
	unsigned long flags;
	int ret = usb_ep_queue(ep, req, GFP_ATOMIC);

	if (likely(!ret))
		return;

	spin_lock_irqsave(&c2c->lock, flags);
	if (ep == c2c->in_ep) {
		c2c->tx_errs++;
		list_add_tail(&req->list, &c2c->in_free);
	} else {
		c2c->rx_errs++;
		list_add_tail(&req->list, &c2c->out_idle);
	}
	spin_unlock_irqrestore(&c2c->lock, flags);
	pr_warn_ratelimited("c2c: queue on %s failed: %d\n", ep->name, ret);
	if (ret != -ESHUTDOWN)
		schedule_delayed_work(&c2c->rearm, msecs_to_jiffies(5));
	else if (ep == c2c->in_ep)
		wake_up_interruptible_all(&c2c->slot->tx_wait);
}

/* LOOPBACK: give the OUT data to an IN request by swapping the buffers */
static void c2c_pair(struct f_c2c *c2c, struct usb_request *out,
		     struct usb_request *in)
{
	void *tmp = in->buf;

	in->buf = out->buf;
	out->buf = tmp;
	in->length = out->actual;
	/* keep the host's framing: OUT ended short/ZLP -> IN does too */
	in->zero = out->actual < c2c->blen;
	out->length = c2c->blen;
}

static void c2c_out_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct c2c_rctx *x = req->context;
	struct f_c2c *c2c = x->c2c;
	struct usb_request *in = NULL;
	unsigned long flags;

	C2C_TRACE("OUT done st=%d act=%u\n", req->status, req->actual);
	if (c2c_req_flushed(req->status))
		return;

	spin_lock_irqsave(&c2c->lock, flags);
	if (!c2c->active) {
		spin_unlock_irqrestore(&c2c->lock, flags);
		return;
	}
	if (unlikely(req->status)) {
		/* park it, NEVER requeue from inside an error completion */
		c2c->rx_errs++;
		list_add_tail(&req->list, &c2c->out_idle);
		spin_unlock_irqrestore(&c2c->lock, flags);
		pr_warn_ratelimited("c2c: OUT completion status %d\n",
				    req->status);
		schedule_delayed_work(&c2c->rearm, msecs_to_jiffies(5));
		return;
	}
	c2c->rx_bytes += req->actual;
	c2c->rx_reqs++;

	if (c2c->mode == C2C_MODE_STREAM && req->actual) {
		x->off = 0;
		list_add_tail(&req->list, &c2c->rx_ready);
		spin_unlock_irqrestore(&c2c->lock, flags);
		wake_up_interruptible(&c2c->slot->rx_wait);
		return;
	}
	if (c2c->mode == C2C_MODE_LOOPBACK) {
		if (list_empty(&c2c->in_free)) {
			list_add_tail(&req->list, &c2c->out_parked);
			spin_unlock_irqrestore(&c2c->lock, flags);
			return;
		}
		in = list_first_entry(&c2c->in_free, struct usb_request, list);
		list_del(&in->list);
		c2c_pair(c2c, req, in);
	}
	spin_unlock_irqrestore(&c2c->lock, flags);

	if (in)
		c2c_queue(c2c, c2c->in_ep, in);
	req->length = c2c->blen;
	c2c_queue(c2c, ep, req);
}

static void c2c_in_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct c2c_rctx *x = req->context;
	struct f_c2c *c2c = x->c2c;
	struct usb_request *out = NULL;
	unsigned long flags;

	C2C_TRACE("IN done st=%d act=%u len=%u\n", req->status, req->actual,
		  req->length);
	if (c2c_req_flushed(req->status))
		return;

	spin_lock_irqsave(&c2c->lock, flags);
	if (!c2c->active) {
		spin_unlock_irqrestore(&c2c->lock, flags);
		return;
	}
	if (unlikely(req->status)) {
		c2c->tx_errs++;
		list_add_tail(&req->list, &c2c->in_free);
		spin_unlock_irqrestore(&c2c->lock, flags);
		pr_warn_ratelimited("c2c: IN completion status %d\n",
				    req->status);
		wake_up_interruptible(&c2c->slot->tx_wait);
		schedule_delayed_work(&c2c->rearm, msecs_to_jiffies(5));
		return;
	}
	c2c->tx_bytes += req->actual;
	c2c->tx_reqs++;

	switch (c2c->mode) {
	case C2C_MODE_SOURCE:
	case C2C_MODE_BIDIR:
		spin_unlock_irqrestore(&c2c->lock, flags);
		req->length = c2c->blen;
		req->zero = 0;
		c2c_queue(c2c, ep, req);
		return;
	case C2C_MODE_LOOPBACK:
		if (!list_empty(&c2c->out_parked)) {
			out = list_first_entry(&c2c->out_parked,
					       struct usb_request, list);
			list_del(&out->list);
			c2c_pair(c2c, out, req);
			break;
		}
		fallthrough;
	default:
		list_add_tail(&req->list, &c2c->in_free);
	}
	spin_unlock_irqrestore(&c2c->lock, flags);

	if (out) {
		c2c_queue(c2c, ep, req);
		c2c_queue(c2c, c2c->out_ep, out);
	} else {
		wake_up_interruptible(&c2c->slot->tx_wait);
	}
}

/*
 * Queue up every idle request the current mode needs. Called from setup
 * (SET_MODE) and the re-arm work, never from a completion handler.
 */
static void c2c_kick(struct f_c2c *c2c)
{
	struct usb_request *req, *tmp, *in;
	LIST_HEAD(to_in);
	LIST_HEAD(to_out);
	unsigned long flags;

	spin_lock_irqsave(&c2c->lock, flags);
	if (!c2c->active) {
		spin_unlock_irqrestore(&c2c->lock, flags);
		return;
	}
	list_for_each_entry_safe(req, tmp, &c2c->out_idle, list) {
		req->length = c2c->blen;
		list_move_tail(&req->list, &to_out);
	}
	if (c2c->mode != C2C_MODE_STREAM) {
		list_for_each_entry_safe(req, tmp, &c2c->rx_ready, list) {
			req->length = c2c->blen;
			list_move_tail(&req->list, &to_out);
		}
	}
	if (c2c->mode == C2C_MODE_LOOPBACK) {
		while (!list_empty(&c2c->out_parked) &&
		       !list_empty(&c2c->in_free)) {
			req = list_first_entry(&c2c->out_parked,
					       struct usb_request, list);
			in = list_first_entry(&c2c->in_free,
					      struct usb_request, list);
			c2c_pair(c2c, req, in);
			list_move_tail(&in->list, &to_in);
			list_move_tail(&req->list, &to_out);
		}
	} else {
		list_for_each_entry_safe(req, tmp, &c2c->out_parked, list) {
			req->length = c2c->blen;
			list_move_tail(&req->list, &to_out);
		}
	}
	if (c2c->mode == C2C_MODE_SOURCE || c2c->mode == C2C_MODE_BIDIR) {
		list_for_each_entry_safe(req, tmp, &c2c->in_free, list) {
			req->length = c2c->blen;
			req->zero = 0;
			list_move_tail(&req->list, &to_in);
		}
	}
	spin_unlock_irqrestore(&c2c->lock, flags);

	list_for_each_entry_safe(req, tmp, &to_in, list) {
		list_del(&req->list);
		c2c_queue(c2c, c2c->in_ep, req);
	}
	list_for_each_entry_safe(req, tmp, &to_out, list) {
		list_del(&req->list);
		c2c_queue(c2c, c2c->out_ep, req);
	}
}

static void c2c_rearm_work(struct work_struct *work)
{
	c2c_kick(container_of(to_delayed_work(work), struct f_c2c, rearm));
}

static void c2c_set_mode(struct f_c2c *c2c, u32 mode)
{
	struct usb_request *req, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&c2c->lock, flags);
	c2c->mode = mode;
	c2c->gen++;
	/* data channel: each SET_MODE from the host starts/ends a session */
	if (c2c->slot->idx)
		WRITE_ONCE(c2c->slot->session, c2c->slot->session + 1);
	/* new mode or new STREAM session -> drop old rx data */
	list_for_each_entry_safe(req, tmp, &c2c->rx_ready, list)
		list_move_tail(&req->list, &c2c->out_idle);
	spin_unlock_irqrestore(&c2c->lock, flags);
	c2c_kick(c2c);
	c2c_wake_all(c2c->slot);
}

static void c2c_disable_eps(struct f_c2c *c2c)
{
	unsigned long flags;
	bool was_active;

	spin_lock_irqsave(&c2c->lock, flags);
	was_active = c2c->active;
	c2c->active = false;
	c2c->mode = C2C_MODE_IDLE;
	c2c->gen++;
	if (c2c->slot->idx)     /* lost the link = session over */
		WRITE_ONCE(c2c->slot->session, c2c->slot->session + 1);
	spin_unlock_irqrestore(&c2c->lock, flags);
	c2c_wake_all(c2c->slot);

	if (was_active) {
		/* UDC hands back everything queued w/ -ESHUTDOWN */
		usb_ep_disable(c2c->in_ep);
		usb_ep_disable(c2c->out_ep);
	}
}

static int c2c_enable_eps(struct f_c2c *c2c)
{
	struct usb_composite_dev *cdev = c2c->function.config->cdev;
	struct usb_request *req, *tmp;
	LIST_HEAD(to_out);
	unsigned long flags;
	unsigned int i;
	int ret;

	c2c->in_ep->driver_data = c2c;
	c2c->out_ep->driver_data = c2c;
	ret = config_ep_by_speed(cdev->gadget, &c2c->function, c2c->in_ep);
	if (ret)
		return ret;
	ret = usb_ep_enable(c2c->in_ep);
	if (ret)
		return ret;
	ret = config_ep_by_speed(cdev->gadget, &c2c->function, c2c->out_ep);
	if (!ret)
		ret = usb_ep_enable(c2c->out_ep);
	if (ret) {
		usb_ep_disable(c2c->in_ep);
		return ret;
	}

	spin_lock_irqsave(&c2c->lock, flags);
	c2c->mode = C2C_MODE_IDLE;
	c2c->gen++;
	INIT_LIST_HEAD(&c2c->in_free);
	INIT_LIST_HEAD(&c2c->out_parked);
	INIT_LIST_HEAD(&c2c->out_idle);
	INIT_LIST_HEAD(&c2c->rx_ready);
	for (i = 0; i < c2c->nreq; i++) {
		/* held ones get filed back by whoever holds them */
		if (!c2c->in_ctx[i].held)
			list_add_tail(&c2c->in_reqs[i]->list, &c2c->in_free);
		if (!c2c->out_ctx[i].held) {
			c2c->out_reqs[i]->length = c2c->blen;
			list_add_tail(&c2c->out_reqs[i]->list, &to_out);
		}
	}
	c2c->active = true;
	spin_unlock_irqrestore(&c2c->lock, flags);

	/* OUT is always armed */
	list_for_each_entry_safe(req, tmp, &to_out, list) {
		list_del(&req->list);
		c2c_queue(c2c, c2c->out_ep, req);
	}
	c2c_wake_all(c2c->slot);
	INFO(cdev, "c2c: %s up, speed=%s, %u x %u B per endpoint\n", c2c->slot->name,
	     usb_speed_string(cdev->gadget->speed), c2c->nreq, c2c->blen);
	return 0;
}

/* ------------------------------------------------------------------ */
/* usb_function ops                                                    */

static int c2c_set_alt(struct usb_function *f, unsigned int intf,
		       unsigned int alt)
{
	struct f_c2c *c2c = func_to_c2c(f);

	if (alt != 0)
		return -EINVAL;
	c2c_disable_eps(c2c);
	return c2c_enable_eps(c2c);
}

static void c2c_disable(struct usb_function *f)
{
	c2c_disable_eps(func_to_c2c(f));
}

static void c2c_fill_stats(struct f_c2c *c2c, struct c2c_gadget_stats *st,
			   bool le)
{
	struct usb_gadget *g = c2c->function.config ?
			       c2c->function.config->cdev->gadget : NULL;
	unsigned long flags;

	spin_lock_irqsave(&c2c->lock, flags);
	st->rx_bytes = c2c->rx_bytes;
	st->tx_bytes = c2c->tx_bytes;
	st->rx_reqs = c2c->rx_reqs;
	st->tx_reqs = c2c->tx_reqs;
	st->rx_errs = c2c->rx_errs;
	st->tx_errs = c2c->tx_errs;
	st->mode = c2c->mode;
	st->speed = (c2c->active && g) ? g->speed : 0;
	spin_unlock_irqrestore(&c2c->lock, flags);
	st->buflen = c2c->blen;
	st->qlen = c2c->nreq;
	if (le) {
		st->rx_bytes = cpu_to_le64(st->rx_bytes);
		st->tx_bytes = cpu_to_le64(st->tx_bytes);
		st->rx_reqs = cpu_to_le64(st->rx_reqs);
		st->tx_reqs = cpu_to_le64(st->tx_reqs);
		st->rx_errs = cpu_to_le32(st->rx_errs);
		st->tx_errs = cpu_to_le32(st->tx_errs);
		st->mode = cpu_to_le32(st->mode);
		st->speed = cpu_to_le32(st->speed);
		st->buflen = cpu_to_le32(st->buflen);
		st->qlen = cpu_to_le32(st->qlen);
	}
}

static int c2c_setup(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	struct f_c2c *c2c = func_to_c2c(f);
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_request *req = cdev->req;
	u16 w_value = le16_to_cpu(ctrl->wValue);
	u16 w_length = le16_to_cpu(ctrl->wLength);
	const u8 out_type = USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE;
	const u8 in_type = USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_INTERFACE;
	struct c2c_gadget_stats st;
	unsigned long flags;
	int value;

	C2C_TRACE("setup type=%02x req=%02x val=%u len=%u\n",
		  ctrl->bRequestType, ctrl->bRequest, w_value, w_length);
	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_VENDOR)
		return -EOPNOTSUPP;

	switch (ctrl->bRequest) {
	case C2C_REQ_SET_MODE:
		if (ctrl->bRequestType != out_type || w_length ||
		    w_value >= C2C_MODE_MAX)
			return -EINVAL;
		c2c_set_mode(c2c, w_value);
		value = 0;
		break;
	case C2C_REQ_RESET_STATS:
		if (ctrl->bRequestType != out_type || w_length)
			return -EINVAL;
		spin_lock_irqsave(&c2c->lock, flags);
		c2c->rx_bytes = c2c->tx_bytes = 0;
		c2c->rx_reqs = c2c->tx_reqs = 0;
		c2c->rx_errs = c2c->tx_errs = 0;
		spin_unlock_irqrestore(&c2c->lock, flags);
		value = 0;
		break;
	case C2C_REQ_GET_STATS:
		if (ctrl->bRequestType != in_type)
			return -EINVAL;
		c2c_fill_stats(c2c, &st, true);
		value = min_t(int, w_length, sizeof(st));
		memcpy(req->buf, &st, value);
		break;
	default:
		return -EOPNOTSUPP;
	}

	req->zero = 0;
	req->length = value;
	value = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
	if (value < 0)
		ERROR(cdev, "c2c: ep0 queue failed %d\n", value);
	return value < 0 ? value : req->length;
}

static void c2c_free_reqs(struct usb_ep *ep, struct usb_request **reqs,
			  unsigned int n)
{
	unsigned int i;

	if (!reqs)
		return;
	for (i = 0; i < n; i++) {
		if (!reqs[i])
			continue;
		kfree(reqs[i]->buf);
		usb_ep_free_request(ep, reqs[i]);
	}
	kfree(reqs);
}

static struct usb_request **c2c_alloc_reqs(struct f_c2c *c2c,
					   struct usb_ep *ep,
					   struct c2c_rctx *ctx,
					   void (*complete)(struct usb_ep *,
							    struct usb_request *))
{
	struct usb_request **reqs;
	unsigned int i;

	reqs = kcalloc(c2c->nreq, sizeof(*reqs), GFP_KERNEL);
	if (!reqs)
		return NULL;
	for (i = 0; i < c2c->nreq; i++) {
		reqs[i] = usb_ep_alloc_request(ep, GFP_KERNEL);
		if (!reqs[i])
			goto fail;
		reqs[i]->buf = kmalloc(c2c->blen, GFP_KERNEL);
		if (!reqs[i]->buf)
			goto fail;
		ctx[i].c2c = c2c;
		ctx[i].off = 0;
		ctx[i].held = false;
		reqs[i]->complete = complete;
		reqs[i]->context = &ctx[i];
		INIT_LIST_HEAD(&reqs[i]->list);
	}
	return reqs;
fail:
	c2c_free_reqs(ep, reqs, c2c->nreq);
	return NULL;
}

static void c2c_release_mem(struct f_c2c *c2c)
{
	c2c_free_reqs(c2c->in_ep, c2c->in_reqs, c2c->nreq);
	c2c_free_reqs(c2c->out_ep, c2c->out_reqs, c2c->nreq);
	kfree(c2c->in_ctx);
	kfree(c2c->out_ctx);
	c2c->in_reqs = c2c->out_reqs = NULL;
	c2c->in_ctx = c2c->out_ctx = NULL;
}

static int c2c_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_composite_dev *cdev = c->cdev;
	struct f_c2c *c2c = func_to_c2c(f);
	int id, ret;

	id = usb_interface_id(c, f);
	if (id < 0)
		return id;
	c2c_intf.bInterfaceNumber = id;
	c2c_intf.bInterfaceProtocol = c2c->slot->idx ? C2C_IF_PROTO_RAW :
						       C2C_IF_PROTO;
	/* descriptors are shared, reset them so autoconfig picks new eps */
	fs_in_desc.bEndpointAddress = USB_DIR_IN;
	fs_out_desc.bEndpointAddress = USB_DIR_OUT;

	c2c->in_ep = usb_ep_autoconfig(cdev->gadget, &fs_in_desc);
	if (!c2c->in_ep)
		return -ENODEV;
	c2c->out_ep = usb_ep_autoconfig(cdev->gadget, &fs_out_desc);
	if (!c2c->out_ep)
		return -ENODEV;

	ss_in_comp.bMaxBurst = min(maxburst, 15u);
	ss_out_comp.bMaxBurst = min(maxburst, 15u);
	hs_in_desc.bEndpointAddress = fs_in_desc.bEndpointAddress;
	hs_out_desc.bEndpointAddress = fs_out_desc.bEndpointAddress;
	ss_in_desc.bEndpointAddress = fs_in_desc.bEndpointAddress;
	ss_out_desc.bEndpointAddress = fs_out_desc.bEndpointAddress;

	ret = usb_assign_descriptors(f, fs_descs, hs_descs, ss_descs, NULL);
	if (ret)
		return ret;

	c2c->blen = buflen;
	c2c->nreq = qlen;
	c2c->in_ctx = kcalloc(c2c->nreq, sizeof(*c2c->in_ctx), GFP_KERNEL);
	c2c->out_ctx = kcalloc(c2c->nreq, sizeof(*c2c->out_ctx), GFP_KERNEL);
	if (c2c->in_ctx && c2c->out_ctx) {
		c2c->in_reqs = c2c_alloc_reqs(c2c, c2c->in_ep, c2c->in_ctx,
					      c2c_in_complete);
		c2c->out_reqs = c2c_alloc_reqs(c2c, c2c->out_ep, c2c->out_ctx,
					       c2c_out_complete);
	}
	if (!c2c->in_reqs || !c2c->out_reqs) {
		c2c_release_mem(c2c);
		usb_free_all_descriptors(f);
		return -ENOMEM;
	}

	down_write(&c2c->slot->cur_sem);
	if (c2c->slot->cur) {
		up_write(&c2c->slot->cur_sem);
		ERROR(cdev, "c2c: %s is already bound\n", c2c->slot->name);
		c2c_release_mem(c2c);
		usb_free_all_descriptors(f);
		return -EBUSY;
	}
	c2c->slot->cur = c2c;
	up_write(&c2c->slot->cur_sem);

	INFO(cdev, "c2c: %s bound, interface %d, IN=%s OUT=%s, %u x %u B\n",
	     c2c->slot->name, id, c2c->in_ep->name, c2c->out_ep->name,
	     c2c->nreq, c2c->blen);
	return 0;
}

static void c2c_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_c2c *c2c = func_to_c2c(f);

	c2c_disable_eps(c2c);           /* wakes up all readers/writers  */
	down_write(&c2c->slot->cur_sem);/* waits till they're all out    */
	if (c2c->slot->cur == c2c)
		c2c->slot->cur = NULL;
	up_write(&c2c->slot->cur_sem);
	cancel_delayed_work_sync(&c2c->rearm);
	c2c_release_mem(c2c);
	usb_free_all_descriptors(f);
}

static void c2c_free_func(struct usb_function *f)
{
	struct f_c2c *c2c = func_to_c2c(f);

	cancel_delayed_work_sync(&c2c->rearm);
	kfree(c2c);
}

struct f_c2c_opts {
	struct usb_function_instance func_inst;
	struct c2c_slot *slot;          /* set_inst_name claims this         */
};

static struct usb_function *c2c_alloc(struct usb_function_instance *fi)
{
	struct f_c2c_opts *opts = container_of(fi, struct f_c2c_opts, func_inst);
	struct f_c2c *c2c;

	if (!opts->slot)
		return ERR_PTR(-EINVAL);
	c2c = kzalloc(sizeof(*c2c), GFP_KERNEL);
	if (!c2c)
		return ERR_PTR(-ENOMEM);
	c2c->slot = opts->slot;
	spin_lock_init(&c2c->lock);
	INIT_LIST_HEAD(&c2c->in_free);
	INIT_LIST_HEAD(&c2c->out_parked);
	INIT_LIST_HEAD(&c2c->out_idle);
	INIT_LIST_HEAD(&c2c->rx_ready);
	INIT_DELAYED_WORK(&c2c->rearm, c2c_rearm_work);
	c2c->function.name = "c2c";
	c2c->function.bind = c2c_bind;
	c2c->function.unbind = c2c_unbind;
	c2c->function.set_alt = c2c_set_alt;
	c2c->function.disable = c2c_disable;
	c2c->function.setup = c2c_setup;
	c2c->function.free_func = c2c_free_func;
	return &c2c->function;
}

/* ------------------------------------------------------------------ */
/* /dev/c2c0 and /dev/c2cchN (slave side)                              */

/* session ended (or a newer one started) since this opener accepted */
static inline bool c2c_stale(struct c2c_slot *sl)
{
	return sl->idx && READ_ONCE(sl->session) != sl->fsession;
}

static bool c2c_rx_cond(struct c2c_slot *sl, struct f_c2c *c2c)
{
	unsigned long flags;
	bool r;

	if (c2c_stale(sl))
		return true;
	spin_lock_irqsave(&c2c->lock, flags);
	r = !c2c->active || !list_empty(&c2c->rx_ready);
	spin_unlock_irqrestore(&c2c->lock, flags);
	return r;
}

static bool c2c_tx_cond(struct c2c_slot *sl, struct f_c2c *c2c)
{
	unsigned long flags;
	bool r;

	if (c2c_stale(sl))
		return true;
	spin_lock_irqsave(&c2c->lock, flags);
	r = !c2c->active || c2c->mode != C2C_MODE_STREAM ||
	    !list_empty(&c2c->in_free);
	spin_unlock_irqrestore(&c2c->lock, flags);
	return r;
}

/* new reader shouldn't see data that piled up while nobody was reading */
static void c2c_flush_rx(struct f_c2c *c2c)
{
	struct usb_request *req, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&c2c->lock, flags);
	list_for_each_entry_safe(req, tmp, &c2c->rx_ready, list)
		list_move_tail(&req->list, &c2c->out_idle);
	c2c->gen++;
	spin_unlock_irqrestore(&c2c->lock, flags);
	c2c_kick(c2c);
}

static int c2cg_open(struct inode *inode, struct file *file)
{
	struct c2c_slot *sl = container_of(file->private_data,
					   struct c2c_slot, misc);

	if (atomic_cmpxchg(&sl->opened, 0, 1))
		return -EBUSY;
	down_read(&sl->cur_sem);
	if (sl->cur)
		c2c_flush_rx(sl->cur);
	up_read(&sl->cur_sem);
	/* a session that started before we opened isn't ours */
	sl->fsession = READ_ONCE(sl->session);
	file->private_data = sl;
	return nonseekable_open(inode, file);
}

static int c2cg_release(struct inode *inode, struct file *file)
{
	struct c2c_slot *sl = file->private_data;

	atomic_set(&sl->opened, 0);
	return 0;
}

static ssize_t c2cg_read(struct file *file, char __user *ubuf, size_t count,
			 loff_t *ppos)
{
	struct c2c_slot *sl = file->private_data;
	struct f_c2c *c2c;
	size_t copied = 0;
	ssize_t ret = 0;

	if (!count)
		return 0;
	if (mutex_lock_interruptible(&sl->rd_mutex))
		return -ERESTARTSYS;
	down_read(&sl->cur_sem);
	c2c = sl->cur;
	if (!c2c) {
		ret = -ENOTCONN;
		goto out;
	}

	while (copied < count) {
		struct usb_request *req;
		struct c2c_rctx *x;
		unsigned long flags;
		bool fault = false;
		size_t n;
		u32 gen;

		if (c2c_stale(sl)) {
			ret = -ECONNRESET;
			break;
		}
		spin_lock_irqsave(&c2c->lock, flags);
		if (!c2c->active) {
			spin_unlock_irqrestore(&c2c->lock, flags);
			ret = -ENOTCONN;
			break;
		}
		if (list_empty(&c2c->rx_ready)) {
			spin_unlock_irqrestore(&c2c->lock, flags);
			if (copied)
				break;
			if (file->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}
			ret = wait_event_interruptible(sl->rx_wait,
						       c2c_rx_cond(sl, c2c));
			if (ret)
				break;
			continue;
		}
		req = list_first_entry(&c2c->rx_ready, struct usb_request, list);
		list_del(&req->list);
		x = req->context;
		x->held = true;
		gen = c2c->gen;
		spin_unlock_irqrestore(&c2c->lock, flags);

		n = min_t(size_t, req->actual - x->off, count - copied);
		if (copy_to_user(ubuf + copied, (u8 *)req->buf + x->off, n)) {
			fault = true;
		} else {
			x->off += n;
			copied += n;
		}

		spin_lock_irqsave(&c2c->lock, flags);
		x->held = false;
		if (!fault && gen == c2c->gen && c2c->active &&
		    x->off < req->actual) {
			list_add(&req->list, &c2c->rx_ready); /* back to head */
			spin_unlock_irqrestore(&c2c->lock, flags);
		} else if (c2c->active) {
			x->off = 0;
			req->length = c2c->blen;
			spin_unlock_irqrestore(&c2c->lock, flags);
			c2c_queue(c2c, c2c->out_ep, req);
		} else {
			list_add_tail(&req->list, &c2c->out_idle);
			spin_unlock_irqrestore(&c2c->lock, flags);
		}
		if (fault) {
			ret = -EFAULT;
			break;
		}
	}
out:
	up_read(&sl->cur_sem);
	mutex_unlock(&sl->rd_mutex);
	return copied ? copied : ret;
}

static ssize_t c2cg_write(struct file *file, const char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	struct c2c_slot *sl = file->private_data;
	struct f_c2c *c2c;
	size_t done = 0;
	ssize_t ret = 0;

	if (!count)
		return 0;
	if (mutex_lock_interruptible(&sl->wr_mutex))
		return -ERESTARTSYS;
	down_read(&sl->cur_sem);
	c2c = sl->cur;
	if (!c2c) {
		ret = -ENOTCONN;
		goto out;
	}

	while (done < count) {
		struct usb_request *req;
		struct c2c_rctx *x;
		unsigned long flags;
		size_t chunk;
		bool fault;
		u32 gen;

		if (c2c_stale(sl)) {
			ret = -ECONNRESET;
			break;
		}
		spin_lock_irqsave(&c2c->lock, flags);
		if (!c2c->active || c2c->mode != C2C_MODE_STREAM) {
			spin_unlock_irqrestore(&c2c->lock, flags);
			ret = -ENOTCONN;
			break;
		}
		if (list_empty(&c2c->in_free)) {
			spin_unlock_irqrestore(&c2c->lock, flags);
			if (file->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}
			ret = wait_event_interruptible(sl->tx_wait,
						       c2c_tx_cond(sl, c2c));
			if (ret)
				break;
			continue;
		}
		req = list_first_entry(&c2c->in_free, struct usb_request, list);
		list_del(&req->list);
		x = req->context;
		x->held = true;
		gen = c2c->gen;
		spin_unlock_irqrestore(&c2c->lock, flags);

		chunk = min_t(size_t, count - done, c2c->blen);
		fault = copy_from_user(req->buf, ubuf + done, chunk) != 0;
		req->length = chunk;
		/* last chunk of this write(): end the USB transfer */
		req->zero = done + chunk == count;

		spin_lock_irqsave(&c2c->lock, flags);
		x->held = false;
		if (!fault && gen == c2c->gen && c2c->active &&
		    c2c->mode == C2C_MODE_STREAM) {
			spin_unlock_irqrestore(&c2c->lock, flags);
			c2c_queue(c2c, c2c->in_ep, req);
			done += chunk;
		} else {
			list_add_tail(&req->list, &c2c->in_free);
			spin_unlock_irqrestore(&c2c->lock, flags);
			ret = fault ? -EFAULT : -ENOTCONN;
			break;
		}
	}
out:
	up_read(&sl->cur_sem);
	mutex_unlock(&sl->wr_mutex);
	return done ? done : ret;
}

static __poll_t c2cg_poll(struct file *file, poll_table *wait)
{
	struct c2c_slot *sl = file->private_data;
	struct f_c2c *c2c;
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(file, &sl->rx_wait, wait);
	poll_wait(file, &sl->tx_wait, wait);

	if (c2c_stale(sl))
		return EPOLLIN | EPOLLHUP;
	down_read(&sl->cur_sem);
	c2c = sl->cur;
	if (!c2c) {
		mask = EPOLLERR;
	} else {
		spin_lock_irqsave(&c2c->lock, flags);
		if (!c2c->active) {
			mask = EPOLLERR;
		} else {
			if (!list_empty(&c2c->rx_ready))
				mask |= EPOLLIN | EPOLLRDNORM;
			if (c2c->mode == C2C_MODE_STREAM &&
			    !list_empty(&c2c->in_free))
				mask |= EPOLLOUT | EPOLLWRNORM;
		}
		spin_unlock_irqrestore(&c2c->lock, flags);
	}
	up_read(&sl->cur_sem);
	return mask;
}

/* channel: is there a session waiting to be accepted? */
static bool c2c_accept_cond(struct c2c_slot *sl)
{
	struct f_c2c *c2c;
	bool r = false;

	down_read(&sl->cur_sem);
	c2c = sl->cur;
	if (c2c && READ_ONCE(sl->session) != sl->fsession) {
		unsigned long flags;

		spin_lock_irqsave(&c2c->lock, flags);
		r = c2c->active && c2c->mode == C2C_MODE_STREAM;
		spin_unlock_irqrestore(&c2c->lock, flags);
	}
	up_read(&sl->cur_sem);
	return r;
}

static long c2cg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct c2c_slot *sl = file->private_data;
	struct c2c_gadget_stats st;
	int ret;

	switch (cmd) {
	case C2C_IOC_GADGET_STATUS:
		memset(&st, 0, sizeof(st));
		down_read(&sl->cur_sem);
		if (sl->cur)
			c2c_fill_stats(sl->cur, &st, false);
		up_read(&sl->cur_sem);
		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	case C2C_IOC_ACCEPT:
		if (!sl->idx)
			return -ENOTTY;
		if (file->f_flags & O_NONBLOCK) {
			if (c2c_accept_cond(sl)) {
				sl->fsession = READ_ONCE(sl->session);
				return 0;
			}
			/* session ended and no new one, eat the change so poll()
			 * stops flagging it */
			sl->fsession = READ_ONCE(sl->session);
			return -EAGAIN;
		}
		/* the counter only moves while a function is bound, so wait
		 * with a timeout in case the function gets swapped out */
		for (;;) {
			ret = wait_event_interruptible_timeout(sl->rx_wait,
						c2c_accept_cond(sl), HZ);
			if (ret < 0)
				return ret;
			if (ret > 0 || c2c_accept_cond(sl))
				break;
		}
		sl->fsession = READ_ONCE(sl->session);
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations c2cg_fops = {
	.owner = THIS_MODULE,
	.open = c2cg_open,
	.release = c2cg_release,
	.read = c2cg_read,
	.write = c2cg_write,
	.poll = c2cg_poll,
	.unlocked_ioctl = c2cg_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = no_llseek,
};

/* ------------------------------------------------------------------ */
/* configfs function instance                                          */

static void c2c_attr_release(struct config_item *item)
{
	struct f_c2c_opts *opts = container_of(to_config_group(item),
					       struct f_c2c_opts,
					       func_inst.group);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations c2c_item_ops = {
	.release = c2c_attr_release,
};

static const struct config_item_type c2c_func_type = {
	.ct_item_ops = &c2c_item_ops,
	.ct_owner = THIS_MODULE,
};

/*
 * Instance names: "chN" (N = 0..C2C_MAX_CHANNELS-1) -> data channel N,
 * /dev/c2cchN. Anything else (installer uses "0") -> the link, /dev/c2c0.
 * The name claims its slot and registers the device.
 */
static int c2c_set_inst_name(struct usb_function_instance *fi, const char *name)
{
	struct f_c2c_opts *opts = container_of(fi, struct f_c2c_opts, func_inst);
	struct c2c_slot *sl;
	unsigned int ch;
	int idx = 0, ret;

	if (opts->slot)
		return -EBUSY;
	if (!strncmp(name, "ch", 2)) {
		if (kstrtouint(name + 2, 10, &ch) || ch >= C2C_MAX_CHANNELS)
			return -EINVAL;
		idx = 1 + ch;
	}
	mutex_lock(&c2c_slots_mutex);
	sl = &c2c_slots[idx];
	if (sl->used) {
		/* (a file left open from a removed instance still works, it
		 * follows the slot, same as /dev/c2c0 always did on rebind) */
		mutex_unlock(&c2c_slots_mutex);
		return -EBUSY;
	}
	if (idx)
		snprintf(sl->name, sizeof(sl->name), "c2cch%u", idx - 1);
	else
		strscpy(sl->name, "c2c0", sizeof(sl->name));
	sl->misc.minor = MISC_DYNAMIC_MINOR;
	sl->misc.name = sl->name;
	sl->misc.fops = &c2cg_fops;
	sl->misc.mode = 0600;
	ret = misc_register(&sl->misc);
	if (!ret) {
		sl->used = true;
		sl->misc_registered = true;
		opts->slot = sl;
	}
	mutex_unlock(&c2c_slots_mutex);
	return ret;
}

static void c2c_free_instance(struct usb_function_instance *fi)
{
	struct f_c2c_opts *opts = container_of(fi, struct f_c2c_opts, func_inst);
	struct c2c_slot *sl = opts->slot;

	if (sl) {
		mutex_lock(&c2c_slots_mutex);
		if (sl->misc_registered)
			misc_deregister(&sl->misc);
		sl->misc_registered = false;
		sl->used = false;
		mutex_unlock(&c2c_slots_mutex);
		c2c_wake_all(sl);
	}
	kfree(opts);
}

static struct usb_function_instance *c2c_alloc_inst(void)
{
	struct f_c2c_opts *opts;

	if (!buflen || buflen % 1024 || buflen > (4 << 20) || !qlen ||
	    qlen > 256)
		return ERR_PTR(-EINVAL);
	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);
	opts->func_inst.set_inst_name = c2c_set_inst_name;
	opts->func_inst.free_func_inst = c2c_free_instance;
	config_group_init_type_name(&opts->func_inst.group, "", &c2c_func_type);
	return &opts->func_inst;
}

DECLARE_USB_FUNCTION(c2c, c2c_alloc_inst, c2c_alloc);

static int __init c2c_mod_init(void)
{
	unsigned int i;

	for (i = 0; i < C2C_MAX_SLOTS; i++) {
		struct c2c_slot *sl = &c2c_slots[i];

		sl->idx = i;
		init_rwsem(&sl->cur_sem);
		init_waitqueue_head(&sl->rx_wait);
		init_waitqueue_head(&sl->tx_wait);
		mutex_init(&sl->rd_mutex);
		mutex_init(&sl->wr_mutex);
		atomic_set(&sl->opened, 0);
	}
	return usb_function_register(&c2cusb_func);
}

static void __exit c2c_mod_exit(void)
{
	usb_function_unregister(&c2cusb_func);
}

module_init(c2c_mod_init);
module_exit(c2c_mod_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("C2C");
MODULE_DESCRIPTION("C2C USB gadget function: bulk link /dev/c2c0 + raw channels /dev/c2cchN");
