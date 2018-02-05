/***************************************************************************
                                 test-raw.c
                                 ----------

    Programme to test the linux usbtmc driver against
    any newer T&M instrument (Keysight, Tektronix, Rohde & Schwarz, ...
    This code also serves as an example for how to use
    the driver with raw read/write functions.

    copyright : (C) 2015 by Dave Penkler
    email     : dpenkler@gmail.com
    copyright : (C) 2018 by Guido Kiener
    email     : usbtmc@kiener-muenchen.de
 ***************************************************************************/

#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <endian.h>
//#include <linux/usb/tmc.h>
#include "tmc.h"


#define NUM_CAPS 9
typedef struct cap_entry {
	char * desc;
	unsigned int mask;
} cap_entryT;

/* Driver encoded capability masks */

const cap_entryT cap_list[NUM_CAPS] = {
	{"TRIGGER      ",  USBTMC488_CAPABILITY_TRIGGER        },
	{"REN_CONTROL  ",  USBTMC488_CAPABILITY_REN_CONTROL    },
	{"GOTO_LOCAL   ",  USBTMC488_CAPABILITY_GOTO_LOCAL     },
	{"LOCAL_LOCKOUT",  USBTMC488_CAPABILITY_LOCAL_LOCKOUT  },
	{"488_DOT_2    ",  USBTMC488_CAPABILITY_488_DOT_2      },
	{"DT1          ",  USBTMC488_CAPABILITY_DT1            },
	{"RL1          ",  USBTMC488_CAPABILITY_RL1            },
	{"SR1          ",  USBTMC488_CAPABILITY_SR1            },
	{"FULL_SCPI    ",  USBTMC488_CAPABILITY_FULL_SCPI      }
};

#define HEADER_SIZE 12
#define BULKSIZE 4096

static int fd;
static const char star[] = {' ','*'};
static __u8 s_tag = 0x01;

static void show_caps(unsigned char caps) {
	int i;
	printf("Instrument capabilities: * prefix => supported capability\n\n");
	for (i=0;i<NUM_CAPS;i++)
		printf("\t%c%s\n",star[((caps & cap_list[i].mask) != 0)],
			cap_list[i].desc);
}

#define NUM_STAT_BITS 8
typedef struct stat_entry {
	char * desc;
	unsigned int mask;
} stat_entryT;

/*
   Service request enable register (SRE) Bit definitions
 */

#define SRE_Trigger            1
#define SRE_User               2
#define SRE_Message            4
#define SRE_MessageAvailable  16
#define SRE_Event_Status      32
#define SRE_Operation_Status 128

/* need these defines to inline bit definitions in SRE command strings */
#define xstr(s) str(s)
#define str(s) #s

/*
   Status register bit definitions
 */

#define STB_TRG    1
#define STB_USR    2
#define STB_MSG    4
#define STB_MAV   16
#define STB_ESB   32
#define STB_MSS   64
#define STB_OSR  128

const stat_entryT stb_bits[NUM_STAT_BITS] = {
	{"TRG",  STB_TRG},
	{"USR",  STB_USR},
	{"MSG",  STB_MSG},
	{"__8",  8}, /* not used */
	{"MAV",  STB_MAV},
	{"ESB",  STB_ESB},
	{"MSS",  STB_MSS},
	{"OSR",  STB_OSR}
};

/* Helper routines */

static double main_ts = 0.0;
double getTS_usec() {
	struct timeval tv;
	double ts,tmp;
	gettimeofday(&tv,NULL);
	tmp = tv.tv_sec*1e6 + (double)tv.tv_usec;
	ts = tmp - main_ts;
	main_ts = tmp;
	return ts;
}

static void show_stb(unsigned char stb) {
	int i;
	printf("%10.0f STB = ",getTS_usec());
	for (i=0;i<NUM_STAT_BITS;i++)
		if (stb & stb_bits[i].mask) {
			printf("%s ",stb_bits[i].desc);
		}
	printf("\n");
}

static int flag=0; /* set to 1 if srq handler called */

void srq_handler(int sig) {
	flag = 1;
}


unsigned int get_stb() {
	unsigned char stb;
	if (0 != ioctl(fd,USBTMC488_IOCTL_READ_STB,&stb)) {
		perror("stb ioctl failed");
		exit(1);
	}
	return stb;
}

/* Send string to T&M instrument */
static int tmc_write_common(char *msg, __u32 length, __u32 *written, int async) 
{
	struct usbtmc_message data;
	__u64 total = 0;
	int retval = 0;
	__u32 addflag = (async)?(USBTMC_FLAG_ASYNC):0;
	char buf[1024]; 
	/* Size of first package is USB 3.0 max packet size. 
	 * Use only multiple of max package size (64,512,1024) to avoid sending a short package.
	 * Only last package can be a short package.
	 */
	
	if (!msg) 
		return -EINVAL;
	
	buf[0] = 1;
	buf[1] = s_tag;
	buf[2] = ~s_tag;
	
	s_tag++;
	if (s_tag == 0) s_tag++;
	
	buf[3] = 0; /* Reserved */
	buf[4] = length >> 0;
	buf[5] = length >> 8;
	buf[6] = length >> 16;
	buf[7] = length >> 24;
	buf[8] = 0x01; /* EOM */
	buf[9] = 0; /* Reserved */
	buf[10] = 0; /* Reserved */
	buf[11] = 0; /* Reserved */

	data.message = buf;
	data.transfer_size = length + HEADER_SIZE; /* 32 bit alignment done by driver */
	
	if (data.transfer_size <= sizeof(buf)) {
		data.flags = addflag; /* synchron */
		memcpy(&buf[HEADER_SIZE], msg, length);
		retval = ioctl(fd,USBTMC_IOCTL_WRITE, &data);
		goto exit;
	}
	
	data.transfer_size = sizeof(buf);
	data.flags = (USBTMC_FLAG_ASYNC | addflag); /* asynchron */
	memcpy(&buf[HEADER_SIZE], msg, sizeof(buf)-HEADER_SIZE);
	retval = ioctl(fd, USBTMC_IOCTL_WRITE, &data);
	if (retval < 0)
		goto exit;

	assert(data.transferred == sizeof(buf));
	data.message = msg + (sizeof(buf) - HEADER_SIZE);
	data.transfer_size = length - (sizeof(buf) - HEADER_SIZE);
	data.flags = (USBTMC_FLAG_APPEND | addflag); /* append and wait*/
	retval = ioctl(fd, USBTMC_IOCTL_WRITE, &data);
exit:
	total += data.transferred;
	if (written) {
		if (total >= HEADER_SIZE)
			total -= HEADER_SIZE;
		if (total > length) {
			assert((total - length) <= 3);
			total = length; // strip 32 bit alignment done by driver */
		}
		*written = total;
	}
	// TODO: Abort BULK OUT in case of error!
	return retval;
}

static int tmc_write(char *msg, __u32 length, __u32 *written)
{
	return tmc_write_common(msg, length, written, 0);
}

static int tmc_write_async(char *msg, __u32 length, __u32 *written)
{
	return tmc_write_common(msg, length, written, 1);
}

static int tmc_write_result_async(__u32 *written)
{
	__u64 transferred;
	int retval = ioctl(fd, USBTMC_IOCTL_WRITE_RESULT, &transferred);
	if (written)
		*written = transferred;
	return retval;
}

int tmc_send(char *msg) 
{
	__u32 written;
	int retval = tmc_write(msg, strlen(msg), &written);
	return retval;
}

void setSRE(int val) {
	char buf[32];
	snprintf(buf,32,"*SRE %d\n",val);
	tmc_send(buf);
}

int tmc_read(char *msg, __u32 max_len, __u32 *received) 
{
	struct usbtmc_message data;
	__u64 total = 0;
	__u32 expected_size = 0;
	char request[HEADER_SIZE];
	char *buf;
	int retval;
	
	if (!msg) 
		return -EINVAL;
	
	buf = malloc(BULKSIZE);
	if (!buf)
		return -ENOMEM;
	
	request[0] = 2;
	request[1] = s_tag;
	request[2] = ~s_tag;
	
	s_tag++;
	if (s_tag == 0) s_tag++;
	
	request[3] = 0; /* Reserved */
	request[4] = max_len >> 0;
	request[5] = max_len >> 8;
	request[6] = max_len >> 16;
	request[7] = max_len >> 24;
	request[8] = 0x00; /* termchar enabled */
	request[9] = 0; /* termchar */
	request[10] = 0; /* Reserved */
	request[11] = 0; /* Reserved */

	data.message = request;
	data.transfer_size = HEADER_SIZE;
	data.flags = 1; /* async */

	retval = ioctl(fd,USBTMC_IOCTL_WRITE, &data);
	if (retval < 0) {
		goto exit;
	}

	data.message = buf;
	data.transfer_size = BULKSIZE;
	data.flags = 0; /* sync */
	retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
	if (retval < 0) {
		goto exit;
	}
	
	if (data.transferred < HEADER_SIZE) {
		retval = -EPROTO;
		goto exit;
	}
	data.transferred -= HEADER_SIZE;
	
	if (memcmp(request,buf,4) != 0) {
		retval = -EPROTO; /* response out of order */
		goto exit;
	}
	
	expected_size = le32toh(*(__u32*)&buf[4]);
	if (expected_size > max_len || data.transferred > expected_size) {
		retval = -EPROTO; /* more data than requested */
		goto exit;
	}

	memcpy(msg, (char*)data.message + HEADER_SIZE, data.transferred);
	total = data.transferred;
	expected_size -= data.transferred;
	
	while (expected_size > 0) {
		data.message = msg + total;
		data.transfer_size = expected_size;
		data.flags = 0; /* sync */
		retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
		total += data.transferred;
		if (retval < 0) {
			goto exit;
		}
		if (data.transferred == 0 || data.transferred > expected_size)
			break;
		expected_size -= data.transferred;
	}
exit:	
	if (received)
		*received = (__u32)total;

	// TODO: In case of error abort bulk in.
	free(buf);
	return retval;
}

int tmc_read_async_start(__u32 max_len)
{
	struct usbtmc_message data;
	char request[HEADER_SIZE];
	int retval;
	
	request[0] = 2;
	request[1] = s_tag;
	request[2] = ~s_tag;
	
	s_tag++;
	if (s_tag == 0) s_tag++;
	
	request[3] = 0; /* Reserved */
	request[4] = max_len >> 0;
	request[5] = max_len >> 8;
	request[6] = max_len >> 16;
	request[7] = max_len >> 24;
	request[8] = 0x00; /* termchar enabled */
	request[9] = 0; /* termchar */
	request[10] = 0; /* Reserved */
	request[11] = 0; /* Reserved */

	data.message = request;
	data.transfer_size = HEADER_SIZE;
	data.flags = USBTMC_FLAG_ASYNC; /* async */

	retval = ioctl(fd,USBTMC_IOCTL_WRITE, &data);
	printf("tmc_read_async_start 1: rv=%d\n", retval);
	if (retval < 0)
		return retval;

	data.message = NULL; /* just trigger asynchronous read */
	data.transfer_size = BULKSIZE;
	data.flags = USBTMC_FLAG_ASYNC; /* sync */
	retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
	printf("tmc_read_async_start 2: rv=%d\n", retval);
	if (retval < 0) {
		if (errno == EAGAIN) // expected value
			retval = 0;
	}
	return retval;
}

int tmc_read_async_result(char *msg, __u32 max_len, __u32 *received) 
{
	struct usbtmc_message data;
	__u64 total = 0;
	__u32 expected_size = 0;
	char *buf;
	int retval;
	
	if (!msg) 
		return -EINVAL;
	
	buf = malloc(BULKSIZE);
	if (!buf)
		return -ENOMEM;
	
	data.message = buf;
	/* Attention! must be multiple of BULKSIZE otherwise urb data is truncated.
	 * Note that kernel driver only wants to copy a complete urb.
	 */
	data.transfer_size = BULKSIZE; 
	data.flags = USBTMC_FLAG_ASYNC; /* async */
	retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
	if (retval < 0) {
		goto exit;
	}
	
	if (data.transferred < HEADER_SIZE) {
		retval = -EPROTO;
		goto exit;
	}
	data.transferred -= HEADER_SIZE;

	if (buf[0] != 2) {
		retval = -EPROTO; /* response out of order */
		goto exit;
	}
	
#if 0 // This sample does not check sequence number here.	
	if (memcmp(request,buf,4) != 0) {
		retval = -EPROTO; /* response out of order */
		goto exit;
	}
#endif	

	expected_size = le32toh(*(__u32*)&buf[4]);
	if (expected_size > max_len || data.transferred > expected_size) {
		retval = -EPROTO; /* more data than requested */
		goto exit;
	}

	memcpy(msg, (char*)data.message + HEADER_SIZE, data.transferred);
	total = data.transferred;
	expected_size -= data.transferred;
	
	/* TODO: This algorithm is subject to change and not correct yet.
	 */
	while (expected_size > 0) {
		data.message = msg + total;
		data.transfer_size = expected_size;
		data.flags = 0; /* synchronous now. Which timeout?*/
		retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
		total += data.transferred;
		if (retval < 0) {
			goto exit;
		}
		if (data.transferred == 0 || data.transferred > expected_size)
			break;
		expected_size -= data.transferred;
	}
exit:	
	if (received)
		*received = (__u32)total;

	// TODO: In case of error abort bulk in.
	free(buf);
	return retval;
}

/* Read string from scope */
int rscope(char *buf, int max_len) {
	int len = read(fd,buf,max_len);
	buf[len] = 0; /* zero terminate */
	return len;
}

/* Wait for SRQ using poll() */
void wait_for_srq() {
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLPRI;
	poll(&pfd,1,-1);
}

/* return true if we can write */
int wait_for_write(int timeout) {
	struct pollfd pfd;
	int err;
	pfd.fd = fd;
	pfd.events = POLLOUT|POLLERR|POLLHUP;
	err = poll(&pfd,1,timeout);
	return ((err == 1) && (pfd.events & POLLOUT));
}

/* return true if we can read */
int wait_for_read(int timeout) {
	struct pollfd pfd;
	int err;
	pfd.fd = fd;
	pfd.events = POLLIN|POLLERR|POLLHUP;
	err = poll(&pfd,1,timeout);
	return ((err == 1) && (pfd.events & POLLIN));
}

void wait_for_user() {
	char buf[8];
	read(0,buf,1);
}

const size_t MAX_BL = 1024;


int main () {
  int rv;
  static char sBigSend[10000];
  static char sBigReceive[10000];
  
  unsigned int tmp,tmp1,caps,ren;
  int len,n;
  unsigned char stb;
  __u32 received, sent;
  char buf[MAX_BL];
  int oflags;
  fd_set fdsel[3];
  double time, time2, time3;
  int i;

  // prepare big send data
  strcpy(sBigSend, "mmem:data 'test.txt', #49000");
  n = strlen(sBigSend);
  for (i = 0; i < 9000; i++) 
	sBigSend[n+i] = (char)i;
  
  /* Open file */
  if (0 > (fd = open("/dev/usbtmc0",O_RDWR))) {
	perror("failed to open device");
	exit(1);
  }

  setSRE(0x00); /* disable SRQ */
  tmc_send("*CLS\n");

  /* Send device clear */
  ioctl(fd,USBTMC_IOCTL_CLEAR);
  getTS_usec(); /* initialise time stamp */
  show_stb(get_stb());

  if (!wait_for_write(0)) {
	// poll function failed
  	perror("cannot write");
	exit(1);
  }

  #if 1
  /* Send identity query */
  tmc_send("*IDN?\n");
  /* Read and print returned identity string */
  tmc_read(buf,MAX_BL, &received);
  printf("*IDN? = %.*s\n", received, buf);


  getTS_usec(); /* initialise time stamp */
  for (i = 0; i < 10; i++) {
	tmc_write("*OPC?\n",6,NULL);
	tmc_read(sBigReceive, 10, NULL);
  }
  time = getTS_usec();
  printf("*OPC? Latency = %.0f us per call\n", time/10.0);

  /* Any system error? */
  tmc_send("system:error?\n");
  tmc_read(buf,MAX_BL, &received);
  printf("syst:err? = %.*s", received, buf);
  
  show_stb(get_stb());
  setSRE(0x10); /* Do SRQ when MAV set (message available) */
  getTS_usec(); /* initialise time stamp */
  tmc_send("*TST?");
  wait_for_srq();
  show_stb(get_stb());
  show_stb(get_stb());
  tmc_read(buf,MAX_BL, &received);

  setSRE(0x00);
  
  //return 0;
  /* Any system error? */
  tmc_send("system:error?\n");
  tmc_read(buf,MAX_BL, &received);
  printf("syst:err? = %.*s", received, buf);

  tmc_write(sBigSend,n+9000, &sent);
  tmc_send("mmem:data? 'test.txt'");
  tmc_read(sBigReceive, sizeof(sBigReceive), &received);
  if ( memcmp(&sBigSend[n], &sBigReceive[6], 9000) != 0) {
  	perror("data mismatch");
	exit(1);
  }

  /* Any system error? */
  tmc_send("system:error?\n");
  tmc_read(buf,MAX_BL, &received);
  printf("syst:err? = %.*s", received, buf);

  /* test asynchronous write */
  getTS_usec(); /* initialize time stamp */
  rv = tmc_write_async(sBigSend,n+9000, &sent);
  if (rv < 0 || !wait_for_write(500)) {
	// poll function failed
  	perror("cannot write asynchron");
	exit(1);
  }
  time = getTS_usec();
  printf("async write: rv=%d sent=%u time=%.0f us\n", rv, sent, time);
  rv = tmc_write_result_async(&sent);
  printf("async result: rv=%d transferred=%u\n", rv, sent);

  /* test asynchronous read */
  tmc_send("mmem:data? 'test.txt'");
  getTS_usec();
  rv = tmc_read_async_start(sizeof(sBigReceive));
  time = getTS_usec();
  if (rv < 0 || !wait_for_read(500)) {
	// poll function failed
  	perror("cannot start asynchron read");
	exit(1);
  }
  time2 = getTS_usec();
  printf("async read start: rv=%d\n", rv);

  rv = tmc_read_async_result(sBigReceive, sizeof(sBigReceive), &received);
  if (rv < 0) {
  	perror("cannot read asynchronous result");
	exit(1);
  }
  if ( memcmp(&sBigSend[n], &sBigReceive[6], 9000) != 0) {
  	perror("data mismatch");
	exit(1);
  }

  /* test error handling */
  ioctl(fd,USBTMC_IOCTL_SET_OUT_HALT); // lock bulk out
  rv = tmc_send("system:error?\n");
  printf("send should fail: rv=%d errno=%d\n", rv, errno);
  if (rv >= 0) {
  	perror("sending should fail: ");
	exit(1);
  }
#endif

  ioctl(fd,USBTMC_IOCTL_SET_OUT_HALT); // lock bulk out
  rv = tmc_write(sBigSend,n+9000, &sent);
  printf("big send should fail: rv=%d errno=%d sent=%u\n", rv, errno, sent);
  if (rv >= 0) {
  	perror("sending should fail: ");
	exit(1);
  }

  rv = tmc_write_async("123",3, &sent);
  printf("async send should fail: rv=%d errno=%d sent=%u\n", rv, errno, sent);
  wait_for_write(500);

  rv = tmc_write_result_async(&sent);
  printf("async result: rv=%d transferred=%u\n", rv, sent);
  if (rv >= 0 || sent > 0) {
  	perror("async send result should fail: ");
	exit(1);
  }
  
#if 1
  rv = tmc_write_async(sBigSend,n+9000, &sent);
  printf("async send should fail: rv=%d errno=%d sent=%u\n", rv, errno, sent);
  // This error should be detected before everything is sent.
  wait_for_write(500);
  rv = tmc_write_result_async(&sent);
  printf("async result: rv=%d transferred=%u\n", rv, sent);
  if (rv >= 0 || sent > 0) {
  	perror("async send result should fail: ");
	exit(1);
  }
#endif

  fsync(fd); // clear all errors for safety. TODO: When is it called?
  ioctl(fd,USBTMC_IOCTL_CLEAR_OUT_HALT); // lock bulk out

  /* Any system error? */
  tmc_send("system:error?\n");
  tmc_read(buf,MAX_BL, &received);
  printf("syst:err? = %.*s", received, buf);

  ioctl(fd,USBTMC_IOCTL_SET_IN_HALT); // lock bulk in
  tmc_send("system:error?\n");
  rv = tmc_read(buf,MAX_BL, &received);
  printf("read should fail: rv=%d errno=%d recv=%u\n", rv, errno, received);
  if (rv >= 0 || received > 0) {
  	perror("async send result should fail: ");
	exit(1);
  }

  ioctl(fd,USBTMC_IOCTL_CLEAR_IN_HALT); // clear feature halt bulk in
  
  /* just try to read data. Note this is not conform with protocol */
  rv = tmc_read(buf,MAX_BL, &received);
  printf("read should fail with timeout: rv=%d errno=%d recv=%u\n", rv, errno, received);
  printf("syst:err? = %.*s", received, buf);
  
  printf("done\n");
  close(fd);
  exit(0);
}
