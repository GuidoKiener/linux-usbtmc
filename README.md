# linux-usbtmc driver

This is an experimental linux driver for usb test & measurement
control instruments that adds support for missing functions in
USBTMC-USB488 spec and the ability to handle SRQ notifications with
fasync or poll/select. Most of the functions have been incorporated in
the linux kernel starting with version 4.6.  This package is provided
for folks wanting to test or use driver features not yet supported by
the standard usbtmc driver in their kernel.

The following functions have not yet been incorporated into
a kernel.org release:
 - module params
 - user get/set ioctls for usb timeout
 - ioctl to send generic usb control messages
 - ioctl to control setting of EOM bit in writes
 - ioctl to configure TermChar and TermCharEnable
 - ioctls to send generic or vendor specific IN/OUT messages
 
The remaining features are available in the standard kernel.org releases >= 4.6.

## Installation

Prerequisite: You need a prebuilt kernel with the configuration and
kernel header files that were used to build it. Most distros have a
"kernel headers" package for this

To obtain the files either clone the repo with
`git clone https://github.com/dpenkler/linux-usbtmc.git linux-usbtmc`
or download the zip file and extract the zip file to a directory linux-usbtmc

To build the driver simply run `make` in the directory containing the
driver source code (linux-usbtmc/ or linux-usbtmc-master/).

To install the driver run `make install` as root.

To load the driver execute `rmmod usbtmc; insmod usbtmc.ko` as root.

Enable debug messages with `insmod usbtmc.ko dyndbg=+p` and use `dmesg`
to see debug output.

To compile your instrument control program ensure that it includes the
tmc.h file from this repo. An example test program for an
Agilent/Keysight scope is also provided. See the file ttmc.c
To build the provided program run `make ttmc`

**New for IVI:** To test new ioctl functions proposed by IVI Foundation
please create the program `make test-raw`

To clean the directory of build files run `make clean`

## Features

The new features supported by this driver are based on the
specifications contained in the following document from the USB
Implementers Forum, Inc.

    Universal Serial Bus
    Test and Measurement Class,
    Subclass USB488 Specification
    (USBTMC-USB488)
    Revision 1.0
    April 14, 2003

Individual feature descriptions:

### ioctl to support the USBTMC-USB488 READ_STATUS_BYTE operation.

When performing a read on an instrument that is executing
a function that runs longer than the USB timeout the instrument may
hang and require a device reset to recover. The READ_STATUS_BYTE
operation always returns even when the instrument is busy, permitting
the application to poll for the appropriate condition without blocking
as would  be the case with an "*STB?" query.

Note: The READ_STATUS_BYTE ioctl clears the SRQ condition but it has no effect
on the status byte of the device.

**New for IVI:** The returned stb (type __u8) designates a previous SRQ when
bit 6 is set. Note that if more file handles are opened to the same instrument,
all file handles will receive the same status byte with SRQ bit set.


### Support for receiving USBTMC-USB488 SRQ notifications with fasync

By configuring an instrument's service request enable register various
conditions can be reported via an SRQ notification.  When the FASYNC
flag is set on the file descriptor corresponding to the usb connected
instrument a SIGIO signal is sent to the owning process when the
instrument asserts a service request.

Example
```C
  signal(SIGIO, &srq_handler); /* dummy sample; sigaction( ) is better */
  fcntl(fd, F_SETOWN, getpid( ));
  oflags = fcntl(fd, F_GETFL);
  if (0 > fcntl(fd, F_SETFL, oflags | FASYNC)) {
	  perror("fcntl to set fasync failed\n");
	  exit(1);
  }
```

### Support for receiving USBTMC-USB488 SRQ notifications via poll/select

In many situations operations on multiple instruments need to be
synchronized. poll/select provide a convenient way of waiting on a
number of different instruments and other peripherals simultaneously.
When the instrument sends an SRQ notification the fd becomes readable.
If the MAV (message available) event is enabled the normal semantic of
poll/select on a readable file descriptor is achieved. However many
other conditions can be set to cause SRQ. To reset the poll/select
condition a READ_STATUS_BYTE ioctl must be performed.

Example

```C
  FD_SET(fd,&fdsel[0]);
  n = select(fd+1,
	  (fd_set *)(&fdsel[0]),
	  (fd_set *)(&fdsel[1]),
	  (fd_set *)(&fdsel[2]),
	  NULL);
  
  if (FD_ISSET(fd,&fdsel[0])) {
          ioctl(fd,USBTMC488_IOCTL_READ_STB,&stb)
	  if (stb & 16) { /* test for message available bit */
	      len = read(fd,buf,sizeof(buf));
	      /*
	      process buffer
	          ....
	      */
	  }
  }
```

**New for IVI:** With the new asynchronous functions the behavior of the 
poll function was extended. 
 - POLLPRI is set when the interrupt pipe receives a statusbyte with SRQ.
 - POLLIN | POLLRDNORM signals that asynchronous URBs are available on IN pipe.
 - POLLOUT | POLLWRNORM signals that no URBS are submitted to IN or OUT pipe. 
   It is save to write.
 - POLLERR is set when any submitted URB fails.
 
 Note that POLLERR cannot be masked out. That means waiting only for POLLPRI 
 does not work when asynchronous operations are used. In this case ioctl
 USBTMC488_IOCTL_WAIT_SRQ is recommended.
 
 
###  **New for IVI:** ioctl USBTMC488_IOCTL_WAIT_SRQ

The new ioctl offers an alternative way to wait for a Service Request.
In opposite to the poll() function (see above) the ioctl does not return
when asynchronous operations fail.

```C
static int wait_for_srq(unsigned int timeout) {
	return ioctl(fd, USBTMC488_IOCTL_WAIT_SRQ, &timeout);
}
```
The ioctl returns 0 or -1 with errno set:
 - 0 when an SRQ is received
 - errno = ETIMEDOUT when timeout (in ms) is elapsed.
 - errno = ENODEV when file handle is closed or device disconnected
 - errno = EFAULT when device does not have an interrupt pipe.
 

### New ioctls to enable and disable local controls on an instrument

These ioctls provide support for the USBTMC-USB488 control requests
for REN_CONTROL, GO_TO_LOCAL and LOCAL_LOCKOUT

### ioctl to cause a device to trigger

This is equivalent to the IEEE 488 GET (Group Execute Trigger) action.
While a the "*TRG" command can be sent to perform the same operation,
in some situations an instrument will be busy and unable to process
the command immediately in which case the USBTMC488_IOCTL_TRIGGER can
be used. 

### Utility ioctl to retrieve USBTMC-USB488 capabilities

This is a convenience function to obtain an instrument's capabilities
from its file descriptor without having to access sysfs from the user
program. The driver encoded usb488 capability masks are defined in the
tmc.h include file.

### Two new module parameters

***io_buffer_size*** specifies the size of the buffer in bytes that is
used for usb bulk transfers. The default size is 2048. The minimum
size is 512. Values given for this parameter are automatically rounded
down to the nearest multiple of 4.

***usb_timeout*** specifies the timeout in milliseconds that is used
for usb transfers. The default value is 5000 and the minimum value is 500.

To set the parameters
```
insmod usbtmc.ko [io_buffer_size=nnn] [usb_timeout=nnn]
````
For example to set the buffer size to 256KB:
```
insmod usbtmc.ko io_buffer_size=262144
```
**Proposal of IVI:** 
 - using data_attribute(Timeout) instead of module parameter usb_timout.
 - Is there a need to change the io_buffer_size? 4kB is a good value, too.
 
### ioctl's to set/get the usb timeout value

Separate ioctl's to set and get the usb timeout value for a device.
By default the timeout is set to 5000 milliseconds unless changed by
the ***usb_timeout*** module parameter.

USBTMC_IOCTL_SET_TIMEOUT will return with error EINVAL if timeout < 500

Example

```C
	unsigned int timeout, oldtimeout;
....
	ioctl(fd,USBTMC_IOCTL_GET_TIMEOUT,&oldtimeout)
	timeout = 1000;
	ioctl(fd,USBTMC_IOCTL_SET_TIMEOUT,&timeout)

```
### ioctl to send generic usb control requests

Allows user programs to send control messages to a device over the
control pipe.

### ioctl to control setting EOM bit

Enables or disables setting the EOM bit on write.
By default the EOM bit is set on the last transfer of a write.

Will return with error EINVAL if eom is not 0 or 1

Example

```C
	unsigned char eom;
....
	eom = 0; // disable setting of EOM bit on write 
	ioctl(fd,USBTMC_IOCTL_EOM_ENABLE,&eom)

```

### ioctl to configure TermChar and TermCharEnable

Allows enabling/disabling of terminating a read on reception of term_char.
By default TermCharEnabled is false and TermChar is '\n' (0x0a).

Will return with error EINVAL if term_char_enabled is not 0 or 1 or if
attempting to enable term_char when the device does not support terminating
a read when a byte matches the specified term_char.

Example

```C
	struct usbtmc_termc termc;
....
	termc.term_char_enabled = 1; // enable terminating reads on term_char
	termc.term_char = '\n';     
	ioctl(fd,USBTMC_IOCTL_CONFIG_TERMCHAR,&termc)

```


## New ioctls for members of IVI Foundation

The working group "VISA for Linux" (IVI Foundation, www.ivifoundation.org) 
wants to extend the Linux USBTMC driver (linux/drivers/usb/class/usbtmc.c) 
with the following new ioctl functions:


### New for IVI: ioctl USBTMC_IOCTL_WRITE
The ioctl function uses the following struct to send generic OUT bulk messages:
```C
#define USBTMC_FLAG_ASYNC	0x0001
#define USBTMC_FLAG_APPEND	0x0002

struct usbtmc_message {
	void *message; /* pointer to header and data */
	__u64 transfer_size; /* size of bytes to transfer */
	__u64 transferred; /* size of received/written bytes */
	__u32 flags; /* bit 0: 0 = synchronous; 1 = asynchronous */
} __attribute__ ((packed));
```
In synchronous mode (flags=0) the generic write function sends the *message* with
a size of *transfer_size*. The *message* is split into chunks of 4k(=page size) and
submitted (by usb_submit_urb) to the Bulk Out.
A semaphore limits the number of flying urbs. The function waits for the end of
transmission or returns on error e.g when a single chunk exceeds the timeout.
The member *usbtmc_message.transferred* returns the number of transferred bytes.

In asynchronous mode (flags=USBTMC_FLAG_ASYNC) the generic write function is non blocking.
The ioctl clears the current error state and the *internal transfer counter*.
The member *usbtmc_message.transferred* returns the number of submitted bytes,
however less data can be sent to the device in case of error. 
The *internal transfer counter* holds the number of total transferred bytes.

With flag USBTMC_FLAG_APPEND additional urbs are submitted without clearing the current
error state or *internal transfer counter*.

The function returns -EAGAIN when the semaphore does not allow to submit any urb.

POLLOUT | POLLWRNORM are signaled when all submitted urbs are completed.
POLLERR is set when any urb fails. See poll() function above.


### New for IVI: ioctl USBTMC_IOCTL_WRITE_RESULT
The ioctl function copies the current *internal transfer counter* to the 
given __u64 pointer and returns the current error state.


### New for IVI: ioctl USBTMC_IOCTL_READ
The ioctl function uses the following struct to get generic IN bulk messages:
```C
#define USBTMC_FLAG_ASYNC	0x0001
#define USBTMC_FLAG_APPEND	0x0002

struct usbtmc_message {
	void *message; /* pointer to header and data */
	__u64 transfer_size; /* size of bytes to transfer */
	__u64 transferred; /* size of received/written bytes */
	__u32 flags; /* bit 0: 0 = synchronous; 1 = asynchronous */
} __attribute__ ((packed));
```
In synchronous mode (flags=0) the generic read function copies max. 
*transfer_size* bytes of received data from Bulk IN to the 
*usbtmc_message.message* pointer.
Depending on *transfer_size* the read function submits one (<=4kB) or 
two urbs to Bulk IN. For best performance the read function copies bytes
from one urb to the *message* buffer while the other urb can receive data
from the T&M device concurrently. The function waits for the end of 
transmission or returns on error or timeout.
The member *usbtmc_message.transferred* returns the number of received bytes.

For best performance the requested transfer size should be a multiple of 4 kB.
Please note that the driver has to round down the transfer_size to a multiple
of 4 kByte when you use more then 4kB, since the Linux driver only detects 
short packages, but no ZLP (zero length packages).

In asynchronous mode (flags=USBTMC_FLAG_ASYNC) the generic read function 
is non blocking. When no received data is available, the read function 
submits urbs as many as needed to receive *transfer_size* bytes.
However the number of flying urbs is limited by a semaphore (TODO).
In this case the message pointer may be NULL.

The function returns -EAGAIN when no data is available or the semaphore does
not allow to submit an urb.

When received data is already available (triggered by a previous async call)
the function returns available data. The member *usbtmc_message.transferred*
returns the number of received bytes. Ih this case *usbtmc_message.message* 
pointer must be valid.

POLLIN | POLLRDNORM are signaled  when at least one urb has completed 
with received data.
POLLOUT | POLLWRNORM are signaled when all submitted urbs IN/OUT are completed.
POLLERR is set when any urb fails. See poll() function above.


### New for IVI: ioctl USBTMC_IOCTL_CANCEL_IO
This ioctl function cancels USBTMC_IOCTL_READ/USBTMC_IOCTL_WRITE functions.
Internal error flags are set to -ECANCELED. A subsequent call to USBTMC_IOCTL_READ
or USBTMC_IOCTL_WRITE_RESULT will return -ECANCELED with information about current
transferred data.


### New for IVI: ioctl USBTMC_IOCTL_CLEANUP_IO
This ioctl function kills all submitted urbs to OUT and IN pipe, and clears all 
received data from IN pipe. The *Internal transfer counters* are reset.

### New for IVI: ioctl USBTMC_IOCTL_SET_OUT_HALT
This ioctl sends a SET_FEATURE(HALT) request to the OUT endpoint. The ioctl is
useful for test purpose to simulate a device that can not receive any data due
to an error condition.


### New for IVI: ioctl USBTMC_IOCTL_SET_IN_HALT
This ioctl sends a SET_FEATURE(HALT) request to the IN endpoint. The ioctl is
useful for test purpose to simulate a device that can not send any data due to
and error condition.


## Issues and enhancement requests

Use the [Issue](https://github.com/dpenkler/linux-usbtmc/issues) feature in github to post requests for enhancements or bugfixes.
