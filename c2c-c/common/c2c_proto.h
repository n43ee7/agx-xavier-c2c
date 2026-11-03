/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stuff shared between the kernel modules and userspace.
 *
 * master (USB host, c2c_host.ko) <== USB-C ==> slave (USB device, usb_f_c2c.ko)
 *
 * Every interface on the gadget is vendor class w/ one bulk IN + one bulk OUT.
 * Interface 0 is the link that c2cd uses, anything after it is a raw data
 * channel. The host drives each one with vendor control requests sent to
 * that interface.
 */
#ifndef C2C_PROTO_H
#define C2C_PROTO_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <linux/types.h>
#include <sys/ioctl.h>
#endif

/* pid.codes test VID/PID, fine for us but don't ship a product with it */
#define C2C_VID 0x1209
#define C2C_PID 0x0001

/* composite mode: our function bolted onto NVIDIA's stock "l4t" gadget */
#define C2C_NV_VID 0x0955
#define C2C_NV_PID 0x7020

/* interface class/subclass/proto that c2c_host matches on */
#define C2C_IF_CLASS    0xff
#define C2C_IF_SUBCLASS 0x43 /* 'C' */
#define C2C_IF_PROTO    0x01 /* the link interface (c2cd): /dev/c2c0      */
#define C2C_IF_PROTO_RAW 0x02 /* raw data channels: /dev/c2cchN            */

/*
 * Raw data channels = gadget instances "c2c.chN" (exclusive mode only).
 * Each one is just a byte pipe on its own endpoint pair. No framing, no TCP,
 * no daemon sitting in the middle.
 * How sessions work:
 *   master: open(/dev/c2cchN) starts one (gadget throws away stale data),
 *           close() ends it. Only one opener at a time.
 *   slave:  ioctl(C2C_IOC_ACCEPT) waits for the next session. After the
 *           master closes, read()/write() give -ECONNRESET.
 */
#define C2C_MAX_CHANNELS 7

/* ---- vendor control requests (bmRequestType = VENDOR | INTERFACE) ---- */
#define C2C_REQ_SET_MODE    0x51 /* OUT, wValue = mode, no data          */
#define C2C_REQ_GET_STATS   0x52 /* IN,  returns struct c2c_gadget_stats  */
#define C2C_REQ_RESET_STATS 0x53 /* OUT, no data                          */

/* gadget data-engine modes */
enum c2c_mode {
	C2C_MODE_IDLE     = 0, /* nothing queued                               */
	C2C_MODE_SINK     = 1, /* OUT: receive and discard                     */
	C2C_MODE_SOURCE   = 2, /* IN : stream pattern data to host             */
	C2C_MODE_BIDIR    = 3, /* SINK + SOURCE at the same time               */
	C2C_MODE_LOOPBACK = 4, /* every OUT buffer is echoed back on IN        */
	C2C_MODE_STREAM   = 5, /* byte stream to/from the gadget's /dev/c2c0   */
	C2C_MODE_MAX
};

struct c2c_gadget_stats {        /* little endian on the wire */
	__u64 rx_bytes;          /* bytes received on OUT             */
	__u64 tx_bytes;          /* bytes sent on IN                  */
	__u64 rx_reqs;
	__u64 tx_reqs;
	__u32 rx_errs;
	__u32 tx_errs;
	__u32 mode;
	__u32 buflen;            /* gadget request buffer size        */
	__u32 qlen;              /* requests per endpoint             */
	__u32 speed;             /* enum usb_device_speed             */
} __attribute__((packed));

/* ---- host char device ioctls (/dev/c2c0) ---- */
#define C2C_IOC_MAGIC 'C'

struct c2c_info {
	__u32 speed;             /* enum usb_device_speed (5 = SuperSpeed, 6 = SS+) */
	__u32 mps_in;
	__u32 mps_out;
	__u32 maxburst;          /* from SS endpoint companion (0 if none) */
	__u32 busnum;
	__u32 devnum;
	char  speed_str[32];
};

enum c2c_bench_dir {
	C2C_DIR_OUT   = 1,       /* host -> gadget (gadget must be SINK/BIDIR)   */
	C2C_DIR_IN    = 2,       /* gadget -> host (gadget must be SOURCE/BIDIR) */
	C2C_DIR_BIDIR = 3,       /* both at once   (gadget must be BIDIR)        */
};

struct c2c_bench {
	/* in */
	__u32 dir;
	__u32 xfer_size;         /* bytes per URB (multiple of 1024)          */
	__u32 qdepth;            /* URBs in flight per direction (1..64)      */
	__u32 pad;
	__u64 total_bytes;       /* bytes per direction                       */
	/* out */
	__u64 out_bytes;
	__u64 in_bytes;
	__u64 out_ns;            /* first submit -> last completion           */
	__u64 in_ns;
	__u32 errors;
	__u32 pad2;
};

struct c2c_pingpong {
	/* in (gadget must be in LOOPBACK) */
	__u32 size;              /* payload bytes per ping (1..1 MiB)         */
	__u32 iters;
	/* out, nanoseconds */
	__u64 min_ns;
	__u64 max_ns;
	__u64 sum_ns;
	__u64 p50_ns;
	__u64 p99_ns;
	__u32 mismatches;        /* data verification failures                */
	__u32 errors;
};

#define C2C_IOC_GET_INFO     _IOR(C2C_IOC_MAGIC, 1, struct c2c_info)
#define C2C_IOC_SET_MODE     _IOW(C2C_IOC_MAGIC, 2, __u32)
#define C2C_IOC_GET_GSTATS   _IOR(C2C_IOC_MAGIC, 3, struct c2c_gadget_stats)
#define C2C_IOC_RESET_GSTATS _IO(C2C_IOC_MAGIC, 4)
#define C2C_IOC_BENCH        _IOWR(C2C_IOC_MAGIC, 5, struct c2c_bench)
#define C2C_IOC_PINGPONG     _IOWR(C2C_IOC_MAGIC, 6, struct c2c_pingpong)

/* slave side /dev/c2c0: local stats, speed == 0 means no link */
#define C2C_IOC_GADGET_STATUS _IOR(C2C_IOC_MAGIC, 7, struct c2c_gadget_stats)

/* slave side /dev/c2cchN: blocks until the master opens a new session */
#define C2C_IOC_ACCEPT        _IO(C2C_IOC_MAGIC, 8)

#endif /* C2C_PROTO_H */
