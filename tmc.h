/*
 * Copyright (C) 2007 Stefan Kopp, Gechingen, Germany
 * Copyright (C) 2008 Novell, Inc.
 * Copyright (C) 2008 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (C) 2015 Dave Penkler <dpenkler@gmail.com>
 *
 * This file holds USB constants defined by the USB Device Class
 * and USB488 Subclass Definitions for Test and Measurement devices
 * published by the USB-IF.
 *
 * It also has the ioctl and capability definitions for the
 * usbtmc kernel driver that userspace needs to know about.
 */

#ifndef __LINUX_USB_TMC_H
#define __LINUX_USB_TMC_H

#include <linux/usb/ch9.h>

/* USB TMC status values */
#define USBTMC_STATUS_SUCCESS				0x01
#define USBTMC_STATUS_PENDING				0x02
#define USBTMC_STATUS_FAILED				0x80
#define USBTMC_STATUS_TRANSFER_NOT_IN_PROGRESS		0x81
#define USBTMC_STATUS_SPLIT_NOT_IN_PROGRESS		0x82
#define USBTMC_STATUS_SPLIT_IN_PROGRESS			0x83

/* USB TMC requests values */
#define USBTMC_REQUEST_INITIATE_ABORT_BULK_OUT		1
#define USBTMC_REQUEST_CHECK_ABORT_BULK_OUT_STATUS	2
#define USBTMC_REQUEST_INITIATE_ABORT_BULK_IN		3
#define USBTMC_REQUEST_CHECK_ABORT_BULK_IN_STATUS	4
#define USBTMC_REQUEST_INITIATE_CLEAR			5
#define USBTMC_REQUEST_CHECK_CLEAR_STATUS		6
#define USBTMC_REQUEST_GET_CAPABILITIES			7
#define USBTMC_REQUEST_INDICATOR_PULSE			64
#define USBTMC488_REQUEST_READ_STATUS_BYTE		128
#define USBTMC488_REQUEST_REN_CONTROL			160
#define USBTMC488_REQUEST_GOTO_LOCAL			161
#define USBTMC488_REQUEST_LOCAL_LOCKOUT			162

struct usbtmc_ctrlrequest {
	struct usb_ctrlrequest req;
	void *data;
} __attribute__ ((packed));

struct usbtmc_termchar {
	__u8 term_char;
	__u8 term_char_enabled; // bool
} __attribute__ ((packed));

struct usbtmc_interrupt {
	__u8 notify1;
	__u8 notify2;
} __attribute__ ((packed));

#if 0
struct usbtmc_header {
	__u8  msgid;
	__u8  tag;
	__u8  inv_tag;
	__u8  res1;
	__u32 transfer_size;
	__u8  attributes;
	__u8  term_char;
	__u8  res2[2];
} __attribute__ ((packed));
#endif

/*
 * usbtmc_message->flags:
 */
#define USBTMC_FLAG_ASYNC		0x0001
#define USBTMC_FLAG_APPEND		0x0002
#define USBTMC_FLAG_IGNORE_TRAILER	0x0004

struct usbtmc_message {
	void *message; /* pointer to header and data */
	__u64 transfer_size; /* size of bytes to transfer */
	__u64 transferred; /* size of received/written bytes */
	__u32 flags; /* bit 0: 0 = synchronous; 1 = asynchronous */
} __attribute__ ((packed));

/* Request values for USBTMC driver's ioctl entry point */
#define USBTMC_IOC_NR			91
#define USBTMC_IOCTL_INDICATOR_PULSE	_IO(USBTMC_IOC_NR, 1)
#define USBTMC_IOCTL_CLEAR		_IO(USBTMC_IOC_NR, 2)
#define USBTMC_IOCTL_ABORT_BULK_OUT	_IO(USBTMC_IOC_NR, 3)
#define USBTMC_IOCTL_ABORT_BULK_IN	_IO(USBTMC_IOC_NR, 4)
#define USBTMC_IOCTL_CLEAR_OUT_HALT	_IO(USBTMC_IOC_NR, 6)
#define USBTMC_IOCTL_CLEAR_IN_HALT	_IO(USBTMC_IOC_NR, 7)
#define USBTMC_IOCTL_CTRL_REQUEST	_IOWR(USBTMC_IOC_NR, 8, struct usbtmc_ctrlrequest)
#define USBTMC_IOCTL_GET_TIMEOUT	_IOR(USBTMC_IOC_NR, 9, unsigned int)
#define USBTMC_IOCTL_SET_TIMEOUT	_IOW(USBTMC_IOC_NR, 10, unsigned int)
#define USBTMC_IOCTL_EOM_ENABLE	        _IOW(USBTMC_IOC_NR, 11, unsigned char)
#define USBTMC_IOCTL_CONFIG_TERMCHAR	_IOW(USBTMC_IOC_NR, 12, struct usbtmc_termchar)

#define USBTMC_IOCTL_WRITE		_IOWR(USBTMC_IOC_NR, 13, struct usbtmc_message)
#define USBTMC_IOCTL_READ		_IOWR(USBTMC_IOC_NR, 14, struct usbtmc_message)
#define USBTMC_IOCTL_WRITE_RESULT	_IOWR(USBTMC_IOC_NR, 15, __u64)

#define USBTMC488_IOCTL_GET_CAPS	_IOR(USBTMC_IOC_NR, 17, unsigned char)
#define USBTMC488_IOCTL_READ_STB	_IOR(USBTMC_IOC_NR, 18, unsigned char)
#define USBTMC488_IOCTL_REN_CONTROL	_IOW(USBTMC_IOC_NR, 19, unsigned char)
#define USBTMC488_IOCTL_GOTO_LOCAL	_IO(USBTMC_IOC_NR, 20)
#define USBTMC488_IOCTL_LOCAL_LOCKOUT	_IO(USBTMC_IOC_NR, 21)
#define USBTMC488_IOCTL_TRIGGER		_IO(USBTMC_IOC_NR, 22)
#define USBTMC488_IOCTL_WAIT_SRQ	_IOW(USBTMC_IOC_NR, 23, unsigned int)

/* For test purpose only */
#define USBTMC_IOCTL_SET_OUT_HALT	_IO(USBTMC_IOC_NR, 30)
#define USBTMC_IOCTL_SET_IN_HALT	_IO(USBTMC_IOC_NR, 31)

#define USBTMC_IOCTL_CANCEL_IO		_IO(USBTMC_IOC_NR, 35)
#define USBTMC_IOCTL_CLEANUP_IO		_IO(USBTMC_IOC_NR, 36)

/* Driver encoded usb488 capabilities */
#define USBTMC488_CAPABILITY_TRIGGER         1
#define USBTMC488_CAPABILITY_SIMPLE          2
#define USBTMC488_CAPABILITY_REN_CONTROL     2
#define USBTMC488_CAPABILITY_GOTO_LOCAL      2
#define USBTMC488_CAPABILITY_LOCAL_LOCKOUT   2
#define USBTMC488_CAPABILITY_488_DOT_2       4
#define USBTMC488_CAPABILITY_DT1             16
#define USBTMC488_CAPABILITY_RL1             32
#define USBTMC488_CAPABILITY_SR1             64
#define USBTMC488_CAPABILITY_FULL_SCPI       128

#endif
