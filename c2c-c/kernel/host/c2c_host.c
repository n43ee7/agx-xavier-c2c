// SPDX-License-Identifier: GPL-2.0
/*
 * c2c_host.ko: the USB host side driver, runs on the master.
 *
 * Grabs the usb_f_c2c gadget (VID 0x1209 PID 0x0001). Link interface ->
 * /dev/c2c0 for c2cd. Each raw channel interface -> /dev/c2cchN, open to any
 * user. open() starts a session on the slave and close() ends it, details
 * in c2c_proto.h. All of them do:
 *   write()  bulk OUT, pipelined (cdev_q URBs of cdev_bufsz in flight)
 *   read()   bulk IN ring that keeps streaming, starts on the first read
 *   ioctl()  gadget mode/stats, an in-kernel throughput bench (no copies so
 *            it shows what the bus itself can do) and a ping-pong latency test
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/usb.h>
#include <linux/uaccess.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/ktime.h>
#include <linux/sort.h>
#include <linux/random.h>
#include <linux/poll.h>

#include "c2c_proto.h"

#define C2C_MINOR_BASE 192
#define C2C_CH_MINOR_BASE 200
#define C2C_MAX_Q 64

static unsigned int cdev_bufsz = 64 * 1024; /* best number I measured on tegra xhci <-> xudc */
module_param(cdev_bufsz, uint, 0444);
MODULE_PARM_DESC(cdev_bufsz, "bytes per URB for read()/write()");
static unsigned int cdev_q = 32;
static bool allow_lpm;
module_param(allow_lpm, bool, 0444);
MODULE_PARM_DESC(allow_lpm, "allow USB3 U1/U2 link power states (saves a few mW, adds ~0.4 ms latency when idle)");
module_param(cdev_q, uint, 0444);
MODULE_PARM_DESC(cdev_q, "URBs in flight for read()/write() (<=64)");

static const struct usb_device_id c2c_table[] = {
	/* our own gadget: link + raw channels */
	{ USB_DEVICE_AND_INTERFACE_INFO(C2C_VID, C2C_PID, C2C_IF_CLASS,
					C2C_IF_SUBCLASS, C2C_IF_PROTO) },
	{ USB_DEVICE_AND_INTERFACE_INFO(C2C_VID, C2C_PID, C2C_IF_CLASS,
					C2C_IF_SUBCLASS, C2C_IF_PROTO_RAW) },
	/* our function sitting inside NVIDIA's l4t composite gadget */
	{ USB_DEVICE_AND_INTERFACE_INFO(C2C_NV_VID, C2C_NV_PID, C2C_IF_CLASS,
					C2C_IF_SUBCLASS, C2C_IF_PROTO) },
	{ }
};
MODULE_DEVICE_TABLE(usb, c2c_table);

struct c2c_slot {
	struct urb *urb;
	void *buf;
	bool busy;          /* tx: URB in flight          */
	bool done;          /* rx: URB completed          */
	int status;
	unsigned int actual, offset;
};

struct c2c_dev {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct kref kref;
	u8 ifnum, ep_in, ep_out, maxburst;
	bool chan;                  /* raw data channel (/dev/c2cchN)  */
	u16 mps_in, mps_out;

	struct mutex io_mutex;      /* ioctls, disconnect          */
	struct mutex rd_mutex, wr_mutex;
	bool disconnected;
	atomic_t opened;

	spinlock_t lock;
	unsigned int nslot, bufsz;
	struct c2c_slot tx[C2C_MAX_Q];
	unsigned int tx_avail;
	int tx_err;
	wait_queue_head_t tx_wait;

	struct c2c_slot rx[C2C_MAX_Q];
	bool rx_running;
	unsigned int rx_head;
	wait_queue_head_t rx_wait;
};

static struct usb_driver c2c_driver;

static void c2c_free_slots(struct c2c_slot *s, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		usb_free_urb(s[i].urb);
		kfree(s[i].buf);
		s[i].urb = NULL;
		s[i].buf = NULL;
	}
}

static void c2c_delete(struct kref *kref)
{
	struct c2c_dev *dev = container_of(kref, struct c2c_dev, kref);

	c2c_free_slots(dev->tx, dev->nslot);
	c2c_free_slots(dev->rx, dev->nslot);
	usb_put_intf(dev->intf);
	usb_put_dev(dev->udev);
	kfree(dev);
}

/* ------------------------------------------------------------------ */
/* control helpers                                                     */

static int c2c_ctrl_out(struct c2c_dev *dev, u8 req, u16 value)
{
	return usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), req,
			       USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
			       value, dev->ifnum, NULL, 0, 1000);
}

static int c2c_get_gstats(struct c2c_dev *dev, struct c2c_gadget_stats *out)
{
	struct c2c_gadget_stats *st;
	int ret;

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
			      C2C_REQ_GET_STATS,
			      USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
			      0, dev->ifnum, st, sizeof(*st), 1000);
	if (ret >= 0) {
		out->rx_bytes = le64_to_cpu(st->rx_bytes);
		out->tx_bytes = le64_to_cpu(st->tx_bytes);
		out->rx_reqs = le64_to_cpu(st->rx_reqs);
		out->tx_reqs = le64_to_cpu(st->tx_reqs);
		out->rx_errs = le32_to_cpu(st->rx_errs);
		out->tx_errs = le32_to_cpu(st->tx_errs);
		out->mode = le32_to_cpu(st->mode);
		out->buflen = le32_to_cpu(st->buflen);
		out->qlen = le32_to_cpu(st->qlen);
		out->speed = le32_to_cpu(st->speed);
		ret = 0;
	}
	kfree(st);
	return ret;
}

/* read and throw away whatever the gadget still has queued on IN */
static int c2c_drain_in(struct c2c_dev *dev)
{
	unsigned int len = 1 << 20;
	int actual, ret, total = 0, rounds = 0;
	void *buf = kmalloc(len, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;
	do {
		actual = 0;
		ret = usb_bulk_msg(dev->udev,
				   usb_rcvbulkpipe(dev->udev, dev->ep_in),
				   buf, len, &actual, dev->chan ? 10 : 50);
		total += actual;
	} while ((ret == 0 || (ret == -ETIMEDOUT && actual)) &&
		 ++rounds < 4096);
	kfree(buf);
	return ret == -ETIMEDOUT ? total : ret;
}

static int c2c_set_mode(struct c2c_dev *dev, u32 mode)
{
	int ret;

	if (mode >= C2C_MODE_MAX)
		return -EINVAL;
	ret = c2c_ctrl_out(dev, C2C_REQ_SET_MODE, mode);
	if (ret < 0)
		return ret;
	/* don't let leftovers from an old SOURCE/LOOPBACK run leak through */
	if (mode != C2C_MODE_SOURCE && mode != C2C_MODE_BIDIR) {
		ret = c2c_drain_in(dev);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* in-kernel throughput bench                                          */

struct c2c_bench_ctx {
	struct c2c_dev *dev;
	struct urb *urbs[C2C_MAX_Q];
	unsigned int n;
	u32 xfer;
	u64 total;
	atomic64_t reserved;
	atomic64_t done_bytes;
	atomic_t inflight;
	atomic_t errors;
	bool stop;
	ktime_t t0, t1;
	struct completion done;
	struct usb_anchor anchor;
};

static void c2c_bench_cb(struct urb *urb)
{
	struct c2c_bench_ctx *ctx = urb->context;
	s64 off;

	if (urb->status) {
		if (urb->status != -ENOENT && urb->status != -ECONNRESET)
			atomic_inc(&ctx->errors);
		WRITE_ONCE(ctx->stop, true);
	} else {
		atomic64_add(urb->actual_length, &ctx->done_bytes);
	}

	if (!READ_ONCE(ctx->stop)) {
		off = atomic64_add_return(ctx->xfer, &ctx->reserved) - ctx->xfer;
		if (off < ctx->total) {
			urb->transfer_buffer_length =
				min_t(u64, ctx->xfer, ctx->total - off);
			usb_anchor_urb(urb, &ctx->anchor);
			if (!usb_submit_urb(urb, GFP_ATOMIC))
				return;
			usb_unanchor_urb(urb);
			atomic_inc(&ctx->errors);
			WRITE_ONCE(ctx->stop, true);
		}
	}
	if (atomic_dec_and_test(&ctx->inflight)) {
		ctx->t1 = ktime_get();
		complete(&ctx->done);
	}
}

static void c2c_bench_free(struct c2c_bench_ctx *ctx)
{
	unsigned int i;

	if (!ctx)
		return;
	for (i = 0; i < ctx->n; i++) {
		if (!ctx->urbs[i])
			continue;
		kfree(ctx->urbs[i]->transfer_buffer);
		usb_free_urb(ctx->urbs[i]);
	}
	kfree(ctx);
}

static struct c2c_bench_ctx *c2c_bench_alloc(struct c2c_dev *dev, bool in,
					     u32 xfer, u32 q, u64 total)
{
	struct c2c_bench_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	unsigned int pipe = in ? usb_rcvbulkpipe(dev->udev, dev->ep_in) :
				 usb_sndbulkpipe(dev->udev, dev->ep_out);
	unsigned int i;

	if (!ctx)
		return NULL;
	ctx->dev = dev;
	ctx->xfer = xfer;
	ctx->total = total;
	ctx->n = q;
	init_completion(&ctx->done);
	init_usb_anchor(&ctx->anchor);
	for (i = 0; i < q; i++) {
		void *buf = kmalloc(xfer, GFP_KERNEL);

		ctx->urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!buf || !ctx->urbs[i]) {
			kfree(buf);
			c2c_bench_free(ctx);
			return NULL;
		}
		if (!in)
			get_random_bytes(buf, min_t(u32, xfer, 4096));
		usb_fill_bulk_urb(ctx->urbs[i], dev->udev, pipe, buf, xfer,
				  c2c_bench_cb, ctx);
	}
	return ctx;
}

static void c2c_bench_start(struct c2c_bench_ctx *ctx)
{
	unsigned int i;
	s64 off;

	ctx->t0 = ktime_get();
	atomic_set(&ctx->inflight, 1); /* hold one ref until everything is submitted */
	for (i = 0; i < ctx->n; i++) {
		off = atomic64_add_return(ctx->xfer, &ctx->reserved) - ctx->xfer;
		if (off >= ctx->total)
			break;
		ctx->urbs[i]->transfer_buffer_length =
			min_t(u64, ctx->xfer, ctx->total - off);
		atomic_inc(&ctx->inflight);
		usb_anchor_urb(ctx->urbs[i], &ctx->anchor);
		if (usb_submit_urb(ctx->urbs[i], GFP_KERNEL)) {
			usb_unanchor_urb(ctx->urbs[i]);
			atomic_dec(&ctx->inflight);
			atomic_inc(&ctx->errors);
			WRITE_ONCE(ctx->stop, true);
			break;
		}
	}
	if (atomic_dec_and_test(&ctx->inflight)) {
		ctx->t1 = ktime_get();
		complete(&ctx->done);
	}
}

static int c2c_bench_wait(struct c2c_bench_ctx *ctx, unsigned long tmo)
{
	if (wait_for_completion_timeout(&ctx->done, tmo))
		return 0;
	WRITE_ONCE(ctx->stop, true);
	usb_kill_anchored_urbs(&ctx->anchor);
	wait_for_completion(&ctx->done);
	return -ETIMEDOUT;
}

static int c2c_do_bench(struct c2c_dev *dev, struct c2c_bench *b)
{
	struct c2c_bench_ctx *out = NULL, *in = NULL;
	unsigned long tmo;
	int ret = 0, r;

	if (b->dir < C2C_DIR_OUT || b->dir > C2C_DIR_BIDIR ||
	    !b->xfer_size || b->xfer_size % 1024 || b->xfer_size > (4 << 20) ||
	    !b->qdepth || b->qdepth > C2C_MAX_Q || !b->total_bytes)
		return -EINVAL;
	if (dev->rx_running)
		return -EBUSY;
	/* round up to whole URBs, otherwise IN can stop mid buffer */
	b->total_bytes = roundup(b->total_bytes, (u64)b->xfer_size);

	if (b->dir & C2C_DIR_OUT) {
		out = c2c_bench_alloc(dev, false, b->xfer_size, b->qdepth,
				      b->total_bytes);
		if (!out)
			return -ENOMEM;
	}
	if (b->dir & C2C_DIR_IN) {
		in = c2c_bench_alloc(dev, true, b->xfer_size, b->qdepth,
				     b->total_bytes);
		if (!in) {
			c2c_bench_free(out);
			return -ENOMEM;
		}
	}

	/* loose timeout: assume at least 20 MB/s, +5 s */
	tmo = msecs_to_jiffies(5000 + div_u64(b->total_bytes, 20000));
	if (in)
		c2c_bench_start(in);
	if (out)
		c2c_bench_start(out);
	if (out) {
		r = c2c_bench_wait(out, tmo);
		ret = ret ?: r;
		b->out_bytes = atomic64_read(&out->done_bytes);
		b->out_ns = ktime_to_ns(ktime_sub(out->t1, out->t0));
		b->errors += atomic_read(&out->errors);
	}
	if (in) {
		r = c2c_bench_wait(in, tmo);
		ret = ret ?: r;
		b->in_bytes = atomic64_read(&in->done_bytes);
		b->in_ns = ktime_to_ns(ktime_sub(in->t1, in->t0));
		b->errors += atomic_read(&in->errors);
	}
	c2c_bench_free(out);
	c2c_bench_free(in);
	return ret;
}

/* ------------------------------------------------------------------ */
/* ping-pong latency (gadget in LOOPBACK)                              */

static void c2c_sync_cb(struct urb *urb)
{
	complete(urb->context);
}

static int c2c_sync_xfer(struct urb *urb, int ms)
{
	struct completion done;

	init_completion(&done);
	urb->context = &done;
	urb->actual_length = 0;
	if (usb_submit_urb(urb, GFP_KERNEL))
		return -EIO;
	if (!wait_for_completion_timeout(&done, msecs_to_jiffies(ms))) {
		usb_kill_urb(urb);
		return -ETIMEDOUT;
	}
	return urb->status;
}

static int cmp_u64(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;

	return x < y ? -1 : x > y;
}

static int c2c_do_pingpong(struct c2c_dev *dev, struct c2c_pingpong *p)
{
	struct urb *uo = NULL, *ui = NULL;
	u8 *obuf = NULL, *ibuf = NULL;
	u64 *lat = NULL;
	unsigned int i, inlen;
	int ret = 0;

	if (!p->size || p->size > (1 << 20) || !p->iters || p->iters > 1000000)
		return -EINVAL;
	if (dev->rx_running)
		return -EBUSY;
	inlen = p->size + dev->mps_in; /* leave room for the echoed ZLP / short packet */
	obuf = kmalloc(p->size, GFP_KERNEL);
	ibuf = kmalloc(inlen, GFP_KERNEL);
	lat = vmalloc(sizeof(u64) * p->iters);
	uo = usb_alloc_urb(0, GFP_KERNEL);
	ui = usb_alloc_urb(0, GFP_KERNEL);
	if (!obuf || !ibuf || !lat || !uo || !ui) {
		ret = -ENOMEM;
		goto out;
	}
	usb_fill_bulk_urb(uo, dev->udev, usb_sndbulkpipe(dev->udev, dev->ep_out),
			  obuf, p->size, c2c_sync_cb, NULL);
	uo->transfer_flags |= URB_ZERO_PACKET;
	usb_fill_bulk_urb(ui, dev->udev, usb_rcvbulkpipe(dev->udev, dev->ep_in),
			  ibuf, inlen, c2c_sync_cb, NULL);

	p->min_ns = U64_MAX;
	p->max_ns = p->sum_ns = 0;
	p->mismatches = p->errors = 0;
	get_random_bytes(obuf, p->size);

	for (i = 0; i < p->iters; i++) {
		struct completion din;
		ktime_t t0;
		u64 ns;
		int r;

		obuf[0] = i;
		obuf[p->size - 1] = ~i;
		init_completion(&din);
		ui->context = &din;
		ui->actual_length = 0;
		t0 = ktime_get();
		/* queue IN first so the echo never has to wait for us */
		r = usb_submit_urb(ui, GFP_KERNEL);
		if (r) {
			p->errors++;
			ret = r;
			break;
		}
		r = c2c_sync_xfer(uo, 1000);
		if (r) {
			usb_kill_urb(ui);
			p->errors++;
			ret = r;
			break;
		}
		if (!wait_for_completion_timeout(&din, msecs_to_jiffies(1000))) {
			usb_kill_urb(ui);
			p->errors++;
			ret = -ETIMEDOUT;
			break;
		}
		ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
		if (ui->status) {
			p->errors++;
			ret = ui->status;
			break;
		}
		if (ui->actual_length != p->size ||
		    memcmp(obuf, ibuf, p->size))
			p->mismatches++;
		lat[i] = ns;
		p->sum_ns += ns;
		p->min_ns = min(p->min_ns, ns);
		p->max_ns = max(p->max_ns, ns);
	}
	if (i) {
		sort(lat, i, sizeof(u64), cmp_u64, NULL);
		p->p50_ns = lat[i / 2];
		p->p99_ns = lat[min_t(unsigned int, i - 1, (i * 99) / 100)];
		p->iters = i;
	}
out:
	usb_free_urb(uo);
	usb_free_urb(ui);
	kfree(obuf);
	kfree(ibuf);
	vfree(lat);
	return ret;
}

/* ------------------------------------------------------------------ */
/* char device                                                         */

static void c2c_tx_done(struct urb *urb)
{
	struct c2c_dev *dev = urb->context;
	struct c2c_slot *s = NULL;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&dev->lock, flags);
	for (i = 0; i < dev->nslot; i++)
		if (dev->tx[i].urb == urb)
			s = &dev->tx[i];
	if (s) {
		s->busy = false;
		dev->tx_avail++;
		if (urb->status && urb->status != -ENOENT &&
		    urb->status != -ECONNRESET && !dev->tx_err)
			dev->tx_err = urb->status;
	}
	spin_unlock_irqrestore(&dev->lock, flags);
	wake_up_interruptible_all(&dev->tx_wait);
}

static void c2c_rx_done(struct urb *urb)
{
	struct c2c_dev *dev = urb->context;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&dev->lock, flags);
	for (i = 0; i < dev->nslot; i++) {
		if (dev->rx[i].urb == urb) {
			dev->rx[i].status = urb->status;
			dev->rx[i].actual = urb->actual_length;
			dev->rx[i].offset = 0;
			dev->rx[i].done = true;
		}
	}
	spin_unlock_irqrestore(&dev->lock, flags);
	wake_up_interruptible_all(&dev->rx_wait);
}

static int c2c_rx_submit(struct c2c_dev *dev, struct c2c_slot *s)
{
	s->done = false;
	usb_fill_bulk_urb(s->urb, dev->udev,
			  usb_rcvbulkpipe(dev->udev, dev->ep_in), s->buf,
			  dev->bufsz, c2c_rx_done, dev);
	return usb_submit_urb(s->urb, GFP_KERNEL);
}

static void c2c_rx_stop(struct c2c_dev *dev)
{
	unsigned int i;

	if (!dev->rx_running)
		return;
	for (i = 0; i < dev->nslot; i++)
		usb_kill_urb(dev->rx[i].urb);
	dev->rx_running = false;
}

static int c2c_tx_flush(struct c2c_dev *dev)
{
	int ret = wait_event_interruptible(dev->tx_wait,
				dev->tx_avail == dev->nslot || dev->disconnected);

	if (ret)
		return ret;
	ret = dev->tx_err;
	dev->tx_err = 0;
	return ret;
}

static int c2c_open(struct inode *inode, struct file *file)
{
	struct usb_interface *intf;
	struct c2c_dev *dev;

	intf = usb_find_interface(&c2c_driver, iminor(inode));
	if (!intf)
		return -ENODEV;
	dev = usb_get_intfdata(intf);
	if (!dev)
		return -ENODEV;
	if (atomic_cmpxchg(&dev->opened, 0, 1))
		return -EBUSY;
	kref_get(&dev->kref);
	file->private_data = dev;
	if (dev->chan) {
		int ret;

		/* new session: gadget drops old data and the server's
		 * ACCEPT wakes up */
		mutex_lock(&dev->io_mutex);
		ret = dev->disconnected ? -ENODEV :
					  c2c_set_mode(dev, C2C_MODE_STREAM);
		mutex_unlock(&dev->io_mutex);
		if (ret < 0) {
			atomic_set(&dev->opened, 0);
			kref_put(&dev->kref, c2c_delete);
			return ret;
		}
	}
	return 0;
}

static int c2c_release(struct inode *inode, struct file *file)
{
	struct c2c_dev *dev = file->private_data;
	unsigned int i;

	mutex_lock(&dev->rd_mutex);
	c2c_rx_stop(dev);
	mutex_unlock(&dev->rd_mutex);
	mutex_lock(&dev->wr_mutex);
	if (wait_event_timeout(dev->tx_wait, dev->tx_avail == dev->nslot ||
			       dev->disconnected, 5 * HZ) == 0) {
		for (i = 0; i < dev->nslot; i++)
			usb_kill_urb(dev->tx[i].urb);
	}
	dev->tx_err = 0;
	mutex_unlock(&dev->wr_mutex);
	if (dev->chan) {
		/* end of session, server gets ECONNRESET on read()/write() */
		mutex_lock(&dev->io_mutex);
		if (!dev->disconnected)
			c2c_ctrl_out(dev, C2C_REQ_SET_MODE, C2C_MODE_IDLE);
		mutex_unlock(&dev->io_mutex);
	}
	atomic_set(&dev->opened, 0);
	kref_put(&dev->kref, c2c_delete);
	return 0;
}

static int c2c_flush(struct file *file, fl_owner_t id)
{
	struct c2c_dev *dev = file->private_data;
	int ret;

	mutex_lock(&dev->wr_mutex);
	ret = c2c_tx_flush(dev);
	mutex_unlock(&dev->wr_mutex);
	return ret;
}

static int c2c_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	return c2c_flush(file, NULL);
}

static ssize_t c2c_write(struct file *file, const char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	struct c2c_dev *dev = file->private_data;
	size_t done = 0;
	int ret = 0;

	if (!count)
		return 0;
	if (mutex_lock_interruptible(&dev->wr_mutex))
		return -ERESTARTSYS;
	while (done < count) {
		struct c2c_slot *s = NULL;
		unsigned long flags;
		size_t chunk;
		unsigned int i;

		ret = wait_event_interruptible(dev->tx_wait, dev->tx_avail ||
					       dev->disconnected || dev->tx_err);
		if (ret)
			break;
		if (dev->disconnected) {
			ret = -ENODEV;
			break;
		}
		if (dev->tx_err) {
			ret = dev->tx_err;
			dev->tx_err = 0;
			break;
		}
		spin_lock_irqsave(&dev->lock, flags);
		for (i = 0; i < dev->nslot; i++) {
			if (!dev->tx[i].busy) {
				s = &dev->tx[i];
				s->busy = true;
				dev->tx_avail--;
				break;
			}
		}
		spin_unlock_irqrestore(&dev->lock, flags);
		if (!s)
			continue;

		chunk = min_t(size_t, count - done, dev->bufsz);
		if (copy_from_user(s->buf, ubuf + done, chunk)) {
			ret = -EFAULT;
			goto release_slot;
		}
		usb_fill_bulk_urb(s->urb, dev->udev,
				  usb_sndbulkpipe(dev->udev, dev->ep_out),
				  s->buf, chunk, c2c_tx_done, dev);
		/* end the USB transfer at the end of every write() */
		s->urb->transfer_flags = (done + chunk == count) ?
					 URB_ZERO_PACKET : 0;
		ret = usb_submit_urb(s->urb, GFP_KERNEL);
		if (ret)
			goto release_slot;
		done += chunk;
		continue;
release_slot:
		spin_lock_irqsave(&dev->lock, flags);
		s->busy = false;
		dev->tx_avail++;
		spin_unlock_irqrestore(&dev->lock, flags);
		break;
	}
	mutex_unlock(&dev->wr_mutex);
	return done ? done : ret;
}

/* caller holds rd_mutex */
static int c2c_rx_start(struct c2c_dev *dev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < dev->nslot; i++) {
		ret = c2c_rx_submit(dev, &dev->rx[i]);
		if (ret) {
			while (i--)
				usb_kill_urb(dev->rx[i].urb);
			return ret;
		}
	}
	dev->rx_head = 0;
	dev->rx_running = true;
	return 0;
}

static __poll_t c2c_poll(struct file *file, poll_table *wait)
{
	struct c2c_dev *dev = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &dev->rx_wait, wait);
	poll_wait(file, &dev->tx_wait, wait);
	if (dev->disconnected)
		return EPOLLERR | EPOLLHUP;
	/* polling for input is enough to start the rx ring */
	if (!dev->rx_running && mutex_trylock(&dev->rd_mutex)) {
		if (!dev->rx_running && !dev->disconnected)
			c2c_rx_start(dev);
		mutex_unlock(&dev->rd_mutex);
	}
	if (dev->rx_running && READ_ONCE(dev->rx[dev->rx_head].done))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(dev->tx_avail))
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (READ_ONCE(dev->tx_err))
		mask |= EPOLLERR;
	return mask;
}

static ssize_t c2c_read(struct file *file, char __user *ubuf, size_t count,
			loff_t *ppos)
{
	struct c2c_dev *dev = file->private_data;
	size_t copied = 0;
	int ret = 0;

	if (!count)
		return 0;
	if (mutex_lock_interruptible(&dev->rd_mutex))
		return -ERESTARTSYS;
	if (dev->disconnected) {
		ret = -ENODEV;
		goto out;
	}
	if (!dev->rx_running) {
		ret = c2c_rx_start(dev);
		if (ret)
			goto out;
	}

	while (copied < count) {
		struct c2c_slot *s = &dev->rx[dev->rx_head];
		unsigned int avail, n;

		if (!s->done) {
			if (copied || (file->f_flags & O_NONBLOCK))
				break;
			ret = wait_event_interruptible(dev->rx_wait,
						       s->done || dev->disconnected);
			if (ret)
				break;
			if (dev->disconnected) {
				ret = -ENODEV;
				break;
			}
		}
		if (s->status && s->status != -EREMOTEIO) {
			ret = s->status == -EOVERFLOW ? -EOVERFLOW : -EIO;
			c2c_rx_submit(dev, s);
			dev->rx_head = (dev->rx_head + 1) % dev->nslot;
			break;
		}
		avail = s->actual - s->offset;
		n = min_t(size_t, avail, count - copied);
		if (n && copy_to_user(ubuf + copied, s->buf + s->offset, n)) {
			ret = -EFAULT;
			break;
		}
		s->offset += n;
		copied += n;
		if (s->offset == s->actual) {
			ret = c2c_rx_submit(dev, s);
			dev->rx_head = (dev->rx_head + 1) % dev->nslot;
			if (ret)
				break;
		}
	}
	if (!copied && !ret && (file->f_flags & O_NONBLOCK))
		ret = -EAGAIN;
out:
	mutex_unlock(&dev->rd_mutex);
	return copied ? copied : ret;
}

static long c2c_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct c2c_dev *dev = file->private_data;
	void __user *uarg = (void __user *)arg;
	long ret = 0;

	if (mutex_lock_interruptible(&dev->io_mutex))
		return -ERESTARTSYS;
	if (dev->disconnected) {
		ret = -ENODEV;
		goto out;
	}

	switch (cmd) {
	case C2C_IOC_GET_INFO: {
		struct c2c_info info = {
			.speed = dev->udev->speed,
			.mps_in = dev->mps_in,
			.mps_out = dev->mps_out,
			.maxburst = dev->maxburst,
			.busnum = dev->udev->bus->busnum,
			.devnum = dev->udev->devnum,
		};

		strscpy(info.speed_str, usb_speed_string(dev->udev->speed),
			sizeof(info.speed_str));
		if (copy_to_user(uarg, &info, sizeof(info)))
			ret = -EFAULT;
		break;
	}
	case C2C_IOC_SET_MODE: {
		u32 mode;

		if (get_user(mode, (u32 __user *)uarg)) {
			ret = -EFAULT;
			break;
		}
		/* mode change = whatever is sitting in the rx ring is junk now */
		mutex_lock(&dev->rd_mutex);
		c2c_rx_stop(dev);
		ret = c2c_set_mode(dev, mode);
		mutex_unlock(&dev->rd_mutex);
		break;
	}
	case C2C_IOC_GET_GSTATS: {
		struct c2c_gadget_stats st;

		ret = c2c_get_gstats(dev, &st);
		if (!ret && copy_to_user(uarg, &st, sizeof(st)))
			ret = -EFAULT;
		break;
	}
	case C2C_IOC_RESET_GSTATS:
		ret = c2c_ctrl_out(dev, C2C_REQ_RESET_STATS, 0);
		if (ret > 0)
			ret = 0;
		break;
	case C2C_IOC_BENCH: {
		struct c2c_bench b;

		if (copy_from_user(&b, uarg, sizeof(b))) {
			ret = -EFAULT;
			break;
		}
		b.out_bytes = b.in_bytes = b.out_ns = b.in_ns = 0;
		b.errors = 0;
		mutex_lock(&dev->rd_mutex);
		ret = c2c_do_bench(dev, &b);
		mutex_unlock(&dev->rd_mutex);
		if (copy_to_user(uarg, &b, sizeof(b)))
			ret = -EFAULT;
		break;
	}
	case C2C_IOC_PINGPONG: {
		struct c2c_pingpong p;

		if (copy_from_user(&p, uarg, sizeof(p))) {
			ret = -EFAULT;
			break;
		}
		mutex_lock(&dev->rd_mutex);
		ret = c2c_do_pingpong(dev, &p);
		mutex_unlock(&dev->rd_mutex);
		if (copy_to_user(uarg, &p, sizeof(p)))
			ret = -EFAULT;
		break;
	}
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&dev->io_mutex);
	return ret;
}

static const struct file_operations c2c_fops = {
	.owner = THIS_MODULE,
	.open = c2c_open,
	.release = c2c_release,
	.read = c2c_read,
	.write = c2c_write,
	.flush = c2c_flush,
	.fsync = c2c_fsync,
	.poll = c2c_poll,
	.unlocked_ioctl = c2c_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

static struct usb_class_driver c2c_class = {
	.name = "c2c%d",
	.fops = &c2c_fops,
	.minor_base = C2C_MINOR_BASE,
};

/* raw channels are for normal (non-root) apps */
static char *c2c_ch_devnode(struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;
}

static struct usb_class_driver c2c_ch_class = {
	.name = "c2cch%d",
	.devnode = c2c_ch_devnode,
	.fops = &c2c_fops,
	.minor_base = C2C_CH_MINOR_BASE,
};

/* ------------------------------------------------------------------ */
/* probe / disconnect                                                  */

static int c2c_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_endpoint_descriptor *in, *out;
	struct usb_host_endpoint *hep;
	struct c2c_dev *dev;
	unsigned int i;
	int ret;

	ret = usb_find_common_endpoints(intf->cur_altsetting, &in, &out,
					NULL, NULL);
	if (ret)
		return ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	kref_init(&dev->kref);
	mutex_init(&dev->io_mutex);
	mutex_init(&dev->rd_mutex);
	mutex_init(&dev->wr_mutex);
	spin_lock_init(&dev->lock);
	init_waitqueue_head(&dev->tx_wait);
	init_waitqueue_head(&dev->rx_wait);
	dev->udev = usb_get_dev(interface_to_usbdev(intf));
	dev->intf = usb_get_intf(intf);
	dev->ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
	dev->chan = intf->cur_altsetting->desc.bInterfaceProtocol ==
		    C2C_IF_PROTO_RAW;
	dev->ep_in = usb_endpoint_num(in);
	dev->ep_out = usb_endpoint_num(out);
	dev->mps_in = usb_endpoint_maxp(in);
	dev->mps_out = usb_endpoint_maxp(out);
	hep = usb_pipe_endpoint(dev->udev,
				usb_rcvbulkpipe(dev->udev, dev->ep_in));
	if (hep)
		dev->maxburst = hep->ss_ep_comp.bMaxBurst;

	dev->nslot = clamp_t(unsigned int, cdev_q, 1, C2C_MAX_Q);
	dev->bufsz = clamp_t(unsigned int, rounddown(cdev_bufsz, 1024), 1024,
			     4 << 20);
	for (i = 0; i < dev->nslot; i++) {
		dev->tx[i].urb = usb_alloc_urb(0, GFP_KERNEL);
		dev->tx[i].buf = kmalloc(dev->bufsz, GFP_KERNEL);
		dev->rx[i].urb = usb_alloc_urb(0, GFP_KERNEL);
		dev->rx[i].buf = kmalloc(dev->bufsz, GFP_KERNEL);
		if (!dev->tx[i].urb || !dev->tx[i].buf || !dev->rx[i].urb ||
		    !dev->rx[i].buf) {
			ret = -ENOMEM;
			goto err;
		}
	}
	dev->tx_avail = dev->nslot;

	/* U1/U2 exit latency eats most of the idle round trip (~0.5 ms), keep it in U0 */
	if (!allow_lpm && dev->udev->speed >= USB_SPEED_SUPER)
		usb_unlocked_disable_lpm(dev->udev);

	usb_set_intfdata(intf, dev);
	ret = usb_register_dev(intf, dev->chan ? &c2c_ch_class : &c2c_class);
	if (ret) {
		usb_set_intfdata(intf, NULL);
		goto err;
	}
	dev_info(&intf->dev,
		 "c2c: /dev/%s%d  %s  IN ep%u mps %u burst %u, OUT ep%u mps %u\n",
		 dev->chan ? "c2cch" : "c2c",
		 intf->minor - (dev->chan ? C2C_CH_MINOR_BASE : C2C_MINOR_BASE),
		 usb_speed_string(dev->udev->speed), dev->ep_in, dev->mps_in,
		 dev->maxburst + 1, dev->ep_out, dev->mps_out);
	return 0;
err:
	kref_put(&dev->kref, c2c_delete);
	return ret;
}

static void c2c_disconnect(struct usb_interface *intf)
{
	struct c2c_dev *dev = usb_get_intfdata(intf);
	unsigned int i;

	usb_deregister_dev(intf, dev->chan ? &c2c_ch_class : &c2c_class);
	usb_set_intfdata(intf, NULL);

	mutex_lock(&dev->io_mutex);
	dev->disconnected = true;
	mutex_unlock(&dev->io_mutex);
	for (i = 0; i < dev->nslot; i++) {
		usb_kill_urb(dev->tx[i].urb);
		usb_kill_urb(dev->rx[i].urb);
	}
	wake_up_interruptible_all(&dev->tx_wait);
	wake_up_interruptible_all(&dev->rx_wait);
	dev_info(&intf->dev, "c2c: disconnected\n");
	kref_put(&dev->kref, c2c_delete);
}

static struct usb_driver c2c_driver = {
	.name = "c2c_host",
	.probe = c2c_probe,
	.disconnect = c2c_disconnect,
	.id_table = c2c_table,
	.disable_hub_initiated_lpm = 1,
};

module_usb_driver(c2c_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("C2C-thunderbolt");
MODULE_DESCRIPTION("C2C USB host driver: chardev + in-kernel bulk bench");
