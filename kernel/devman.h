#ifndef _DEVMAN_H_
#define _DEVMAN_H_

#define MAX_DEVICES 128

/* Definitions for drivers */

#define IO_DRIVER_CONTEXT_SPACE 128
struct IODriver
{
	uchar valid; /* Whether or not this driver is in use. */
	slock_t device_lock;
	int (*init)(struct IODriver* driver);
	int (*read)(void* dst, uint start_read, uint sz, void* context);
        int (*write)(void* src, uint start_write, uint sz, void* context);
	uchar context[IO_DRIVER_CONTEXT_SPACE];
	char node[FILE_MAX_PATH]; /* where is the node for this driver? */
};

extern struct IODriver io_drivers[];

/**
 * Setup all io drivers for all available devices.
 */
int dev_init();

/**
 * Read from the device. Returns the amount of bytes read.
 */
int dev_read(struct IODriver* device, void* dst, uint start_read, uint sz);

/**
 * Write to an io device. Returns the amount of bytes written.
 */
int dev_write(struct IODriver* device, void* src, uint start_write, uint sz);

#endif
