
#include <sys/ioctl.h>
#include <sys/time.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
//#include <signal.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
//#include <endian.h>
#include "tmc.h"


#define HEADER_SIZE 12
#define BULKSIZE 4096

static int fd;
static __u8 s_tag = 0x01;

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

unsigned int get_stb() {
	unsigned char stb;
	if (0 != ioctl(fd,USBTMC488_IOCTL_READ_STB,&stb)) {
		perror("stb ioctl failed");
		exit(1);
	}
	return stb;
}

/* Send string to T&M instrument */
static int tmc_raw_write_common(char *msg, __u32 length, __u32 *written, int async) 
{
	struct usbtmc_message data;
	__u32 total = 0;
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

	data.message = (__u64)(uintptr_t)buf;
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
	data.message = (__u64)(uintptr_t)msg + (sizeof(buf) - HEADER_SIZE);
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
	if (retval < 0)
		retval = -errno;
	return retval;
}

static int tmc_raw_write(char *msg, __u32 length, __u32 *written)
{
	return tmc_raw_write_common(msg, length, written, 0);
}

static int tmc_raw_write_async(char *msg, __u32 length, __u32 *written)
{
	return tmc_raw_write_common(msg, length, written, 1);
}

static int tmc_raw_write_result_async(__u32 *written)
{
	__u32 transferred;
	/* Be careful: transferred size includes header and up to 3 padded bytes */
	int retval = ioctl(fd, USBTMC_IOCTL_WRITE_RESULT, &transferred);
	if (written)
		*written = transferred;
	return retval;
}

static int tmc_raw_send(char *msg) 
{
	__u32 written;
	int retval = tmc_raw_write(msg, strlen(msg), &written);
	return retval;
}

static int tmc_send(char *msg) 
{
	int retval = write(fd, msg, strlen(msg));
	return retval;
}

static int tmc_raw_read_async_start(__u32 max_len)
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

	data.message = (__u64)(uintptr_t)request;
	data.transfer_size = HEADER_SIZE;
	data.flags = USBTMC_FLAG_ASYNC; /* async */

	retval = ioctl(fd,USBTMC_IOCTL_WRITE, &data);
	//printf("tmc_raw_read_async_start 1: rv=%d\n", retval);
	if (retval < 0)
		return retval;

	data.message = (__u64)(uintptr_t)NULL; /* just trigger asynchronous read */
	data.transfer_size = BULKSIZE;
	data.flags = USBTMC_FLAG_ASYNC; /* sync */
	retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
	//printf("tmc_raw_read_async_start 2: rv=%d\n", retval);
	if (retval < 0) {
		if (errno == EAGAIN) // expected value
			retval = 0;
	}
	return retval;
}

int tmc_raw_read_async_result(char *msg, __u32 max_len, __u32 *received) 
{
	struct usbtmc_message data;
	__u32 total = 0;
	__u32 expected_size = 0;
	char *buf;
	int retval;
	
	if (!msg) 
		return -EINVAL;
	
	buf = malloc(BULKSIZE);
	if (!buf)
		return -ENOMEM;
	
	data.message = (__u64)(uintptr_t)buf;
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
		printf("tmc_raw_read: response < HEADER_SIZE\n");
		retval = -EPROTO;
		goto exit;
	}
	data.transferred -= HEADER_SIZE;

	if (buf[0] != 2) {
		printf("tmc_raw_read: response out of order\n");
		retval = -EPROTO;
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
		printf("tmc_raw_read: more data than requested \n");
		retval = -EPROTO; /* more data than requested */
		goto exit;
	}

	memcpy(msg, (char*)(uintptr_t)data.message + HEADER_SIZE, data.transferred);
	total = data.transferred;
	expected_size -= data.transferred;
	
	if (retval == 0) {
		// No short packet or ZLP received => ready
		do {
			data.message = (__u64)(uintptr_t)msg + total;
			data.transfer_size = expected_size;
			data.flags = USBTMC_FLAG_ASYNC|USBTMC_FLAG_IGNORE_TRAILER;
			retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
			if (retval < 0) {
				if (errno == EAGAIN) {
					struct pollfd pfd;
					pfd.fd = fd;
					pfd.events = POLLIN|POLLERR|POLLHUP;
					retval = poll(&pfd,1,100); // should not take longer!
					if (retval!=1) {
						ioctl(fd,USBTMC_IOCTL_CLEANUP_IO);
						retval = -ETIMEDOUT;
						goto exit;
					}
					retval = 0;
					continue; // try again!
				}
				retval = -errno;
				ioctl(fd,USBTMC_IOCTL_CLEANUP_IO);
				goto exit;
			}
			total += data.transferred;
			expected_size -= data.transferred;
		} while (retval == 0);
		assert(retval == 1); // must be short packet or ZLP now.
		retval = 0;
	}
exit:	
	if (received)
		*received = (__u32)total;

	// TODO: In case of error abort bulk in.
	free(buf);
	return retval;
}

#if 1
static int tmc_raw_read(char *msg, __u32 max_len, __u32 *received)
{
	struct pollfd pfd;
	int timeout = 2000;
	int err;
	__u32 transferred = 0;
	
	int rv = tmc_raw_read_async_start(max_len);
	if (rv < 0) {
		printf("tmc_raw_read failed to start: errno=%d\n", errno);
		ioctl(fd,USBTMC_IOCTL_CLEANUP_IO);
		goto exit;
	}

	pfd.fd = fd;
	pfd.events = POLLOUT|POLLIN|POLLERR|POLLHUP;
	err = poll(&pfd,1,timeout);
	if (err!=1) {
		ioctl(fd,USBTMC_IOCTL_CLEANUP_IO);
		rv = -ETIMEDOUT;
		goto exit;
	}

	/* Note that POLLOUT is set when all anchor "submitted" is empty */
	if (pfd.revents & (POLLERR|POLLOUT)) {
		__u32 written;
		rv = tmc_raw_write_result_async(&written);
		if (rv < 0 || written != HEADER_SIZE) {
			printf("tmc_raw_read failed to write header: rv=%d written=%u\n", rv, written);
			ioctl(fd,USBTMC_IOCTL_CLEANUP_IO);
			goto exit;
		}
	}
	if (pfd.revents & (POLLERR|POLLIN)) {
		rv = tmc_raw_read_async_result( msg, max_len, &transferred);
		if (rv < 0) {
			printf("tmc_raw_read failed: rv=%d received=%u\n", rv, transferred);
		}
	}
	else {
		/* must not happen */
		assert(0);
		rv = -1; // TODO:
	}
exit:
	if (received)
		*received = transferred;
	
	return rv;
	
	
}
#else
static int tmc_raw_read(char *msg, __u32 max_len, __u32 *received) 
{
	struct usbtmc_message data;
	__u32 total = 0;
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
	data.flags = USBTMC_FLAG_ASYNC;

	retval = ioctl(fd,USBTMC_IOCTL_WRITE, &data);
	if (retval < 0) {
		retval = -errno;
		goto exit;
	}

	data.message = buf;
	data.transfer_size = BULKSIZE;
	data.flags = 0; /* sync */
	retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
	if (retval < 0) {
		retval = -errno;
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

	if (retval == 0) {
		// No short packet or ZLP received => ready
		data.message = msg + total;
		data.transfer_size = expected_size;
		data.flags = USBTMC_FLAG_IGNORE_TRAILER; /* sync */
		retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
		total += data.transferred;
		if (retval < 0) {
			retval = -errno;
		}
		assert(retval == 1); // must be short packet or ZLP now.
		retval = 0;
	}
exit:	
	if (received)
		*received = (__u32)total;

	// TODO: In case of error abort bulk in.
	free(buf);
	return retval;
}
#endif

static int tmc_read(char *msg, __u32 max_len, __u32 *received)
{
	int len = read(fd,msg,max_len);
	if (received)
		*received = (len>0)? len : 0;
	return len;
}

static int enable_eom(unsigned char enabled)
{
	int rv = ioctl(fd, USBTMC_IOCTL_EOM_ENABLE, &enabled);
	assert(rv == 0);
}

static int set_timeout(unsigned int tmo)
{
	int rv = ioctl(fd, USBTMC_IOCTL_SET_TIMEOUT, &tmo);
	assert(rv == 0);
}

const size_t MAX_BL = 1024;

static void any_system_error()
{
  char buf[MAX_BL];
  int rv, res;
  __u32 received = 0;
  
  tmc_raw_send("system:error?\n");
  rv = tmc_raw_read(buf,MAX_BL, &received);
  if (rv < 0 || received == 0) {
	printf("system error read failed: rv=%d errno=%d recv=%u\n", rv, errno, received);
	exit(1);
  }
  rv = sscanf(buf, "%d,", &res);
  if (res != 0 || rv < 1)
	printf("syst:err? = %.*s", received, buf);
}

int main () {
  int rv;
  const unsigned maxsize = 3 * (1024*1024);
  __u32 bigsize;
  char *sBigSend = malloc(maxsize + MAX_BL); /* freed when program exits */
  char *sBigReceive = malloc(maxsize + MAX_BL); /* freed when program exits */
  
  unsigned int tmp,tmp1,caps;
  __u32 len,n, digits;
  __u32 received, sent;
  char buf[MAX_BL];
  //int oflags;
  //fd_set fdsel[3];
  double time, time2;
  int i,k;
  int first_ascii = 'a';

  /* Open file */
  if (0 > (fd = open("/dev/usbtmc0",O_RDWR))) {
	perror("failed to open device");
	exit(1);
  }
  
  /***********************************************************
   * 1. Prepare interface
   ***********************************************************/
  set_timeout(2000);
  enable_eom(1);
  /* Send device clear */
  rv = ioctl(fd,USBTMC_IOCTL_CLEAR);
  assert(rv == 0);
  tmc_send("*CLS\n");
  getTS_usec(); /* initialize time stamp */

  /* Send identity query */
  tmc_raw_send("*IDN?\n");
  /* Read and print returned identity string */
  tmc_raw_read(buf,MAX_BL, &received);
  
  puts("*******************************************************************");
  printf("Testing performance of device: *IDN? = %.*s", received, buf);
  puts("*******************************************************************");
  puts("1. Latency test with raw read/write");
  getTS_usec(); /* initialize time stamp */
  for (i = 0; i < 10; i++) {
	tmc_raw_write("*OPC?\n",6,NULL);
	tmc_raw_read(sBigReceive, 10, NULL);
  }
  time = getTS_usec();
  printf("*OPC? Latency = %.0f us per call with raw read/write functions\n", time/10.0);

  puts("*******************************************************************");
  puts("2a. Send and receive 3 MB data with raw read/write");
  bigsize = 3 * 1024 * 1024;
  for (k = 0; k < 3; k++) {
	/* prepare big send data */
	digits = sprintf( buf, "%u", bigsize );
	n = sprintf( sBigSend,":MMEM:DATA 'test.txt',#%u%s", digits, buf );
	for (i = 0; i < bigsize; i++) 
		sBigSend[n+i] = (char)i+first_ascii;
	first_ascii++;
	getTS_usec(); /* initialize time stamp */
	rv = tmc_raw_write(sBigSend, n+bigsize, &sent);
	time = getTS_usec();
	if (rv < 0) 
		printf("Error in tmc_raw_write: %d\n", rv);
	assert(rv >=0);
	assert(sent == (n+bigsize));
	any_system_error(); /* wait until file is written */

	tmc_raw_send("mmem:data? 'test.txt'");
	getTS_usec(); /* initialize time stamp */
	rv = tmc_raw_read(sBigReceive, bigsize + MAX_BL, &received);
	time2 = getTS_usec();
	if (rv < 0) 
		printf("Error in tmc_raw_read: %d\n", rv);
	if ( memcmp(&sBigSend[n], &sBigReceive[2+digits], bigsize) != 0) {
		for (i = 0; i < bigsize; i++) {
			if ( sBigSend[n+i] != sBigReceive[2+digits+i] ) {
				printf("data mismatch at index: %d, 0x%02x != 0x%02x\n", 
					i, (int)sBigSend[n+i],(int)sBigReceive[2+digits+i]);
				break;
			}
		}
		exit(1);
	}
	printf("Raw I/O: size=%d send %.0f us, rate=%.3f MB/s, read %.0f rate %.3f MB/s\n", 
		bigsize, 
		time, bigsize * (1.0e6/(1024*1024)) / time, 
		time2, bigsize * (1.0e6/(1024*1024)) / time2);
	any_system_error();
  }
  
#if 1
  puts("*******************************************************************");
  puts("2b. Send and receive data with raw read/write");
  for (bigsize = 64; bigsize < maxsize; bigsize <<= 1) {
	/* prepare big send data */
	digits = sprintf( buf, "%u", bigsize );
	n = sprintf( sBigSend,":MMEM:DATA 'test.txt',#%u%s", digits, buf );
	for (i = 0; i < bigsize; i++) 
		sBigSend[n+i] = (char)i+first_ascii;
	first_ascii++;
	getTS_usec(); /* initialize time stamp */
	rv = tmc_raw_write(sBigSend, n+bigsize, &sent);
	time = getTS_usec();
	if (rv < 0) 
		printf("Error in tmc_raw_write: %d\n", rv);
	assert(rv >=0);
	assert(sent == (n+bigsize));
	any_system_error(); /* wait until file is written */

	tmc_raw_send("mmem:data? 'test.txt'");
	getTS_usec(); /* initialize time stamp */
	rv = tmc_raw_read(sBigReceive, bigsize + MAX_BL, &received);
	time2 = getTS_usec();
	if (rv < 0) 
		printf("Error in tmc_raw_read: %d\n", rv);
	if ( memcmp(&sBigSend[n], &sBigReceive[2+digits], bigsize) != 0) {
		for (i = 0; i < bigsize; i++) {
			if ( sBigSend[n+i] != sBigReceive[2+digits+i] ) {
				printf("data mismatch at index: %d, 0x%02x != 0x%02x\n", 
					i, (int)sBigSend[n+i],(int)sBigReceive[2+digits+i]);
				break;
			}
		}
		exit(1);
	}
	printf("Raw I/O: size=%d send %.0f us, rate=%.3f MB/s, read %.0f rate %.3f MB/s\n", 
		bigsize, 
		time, bigsize * (1.0e6/(1024*1024)) / time, 
		time2, bigsize * (1.0e6/(1024*1024)) / time2);
	any_system_error();
  }
#endif

  puts("*******************************************************************");
  puts("2a. Send and receive 3 MB data with normal read/write");
  bigsize = 3 * 1024 * 1024;
  for (k = 0; k < 3; k++) {
	/* prepare big send data */
	digits = sprintf( buf, "%u", bigsize );
	n = sprintf( sBigSend,":MMEM:DATA 'test.txt',#%u%s", digits, buf );
	for (i = 0; i < bigsize; i++) 
		sBigSend[n+i] = (char)i+first_ascii;
	first_ascii++;
   
	getTS_usec(); /* initialize time stamp */
	sent = write(fd, sBigSend, n+bigsize);
	time = getTS_usec();
	if (sent < 0) 
		printf("Error in tmc_raw_write: %d\n", errno);
	assert(sent > 0);
	assert(sent == (n+bigsize));
	any_system_error(); /* wait until file is written */

	tmc_send("mmem:data? 'test.txt'");
	getTS_usec(); /* initialize time stamp */
	rv = tmc_read(sBigReceive, bigsize + MAX_BL, &received);
	time2 = getTS_usec();
	if (rv < 0) 
		printf("Error in tmc_raw_read: %d\n", rv);
	if ( memcmp(&sBigSend[n], &sBigReceive[2+digits], bigsize) != 0) {
		for (i = 0; i < bigsize; i++) {
			if ( sBigSend[n+i] != sBigReceive[2+digits+i] ) {
				printf("data mismatch at index: %d, 0x%02x != 0x%02x\n", 
					i, (int)sBigSend[n+i],(int)sBigReceive[2+digits+i]);
				break;
			}
		}
		exit(1);
	}
	printf("Normal I/O: size=%d send %.0f us, rate=%.3f MB/s, read %.0f rate %.3f MB/s\n", 
		bigsize, 
		time, bigsize * (1.0e6/(1024*1024)) / time, 
		time2, bigsize * (1.0e6/(1024*1024)) / time2);
	any_system_error();
  }
	
  puts("*******************************************************************");
  puts("3b. Send and receive data with normal read/write");
  for (bigsize = 64; bigsize < maxsize; bigsize <<= 1) {
	/* prepare big send data */
	digits = sprintf( buf, "%u", bigsize );
	n = sprintf( sBigSend,":MMEM:DATA 'test.txt',#%u%s", digits, buf );
	for (i = 0; i < bigsize; i++) 
		sBigSend[n+i] = (char)i+first_ascii;
	first_ascii++;
   
	getTS_usec(); /* initialize time stamp */
	sent = write(fd, sBigSend, n+bigsize);
	time = getTS_usec();
	if (sent < 0) 
		printf("Error in tmc_raw_write: %d\n", errno);
	assert(sent > 0);
	assert(sent == (n+bigsize));
	any_system_error(); /* wait until file is written */

	tmc_send("mmem:data? 'test.txt'");
	getTS_usec(); /* initialize time stamp */
	rv = tmc_read(sBigReceive, bigsize + MAX_BL, &received);
	time2 = getTS_usec();
	if (rv < 0) 
		printf("Error in tmc_raw_read: %d\n", rv);
	if ( memcmp(&sBigSend[n], &sBigReceive[2+digits], bigsize) != 0) {
		for (i = 0; i < bigsize; i++) {
			if ( sBigSend[n+i] != sBigReceive[2+digits+i] ) {
				printf("data mismatch at index: %d, 0x%02x != 0x%02x\n", 
					i, (int)sBigSend[n+i],(int)sBigReceive[2+digits+i]);
				break;
			}
		}
		exit(1);
	}
	printf("Normal I/O: size=%d send %.0f us, rate=%.3f MB/s, read %.0f rate %.3f MB/s\n", 
		bigsize, 
		time, bigsize * (1.0e6/(1024*1024)) / time, 
		time2, bigsize * (1.0e6/(1024*1024)) / time2);
	any_system_error();
  }
  
  printf("done\n");
  close(fd);
  exit(0);
}
