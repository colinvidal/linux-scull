#ifndef _SCULL_H
#define _SCULL_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <uapi/asm-generic/ioctl.h>

#define SCULL_MAX_DEVICES 10

#define SCULL_MAJOR 0
#define SCULL_MINOR 0

#define SCULL_QUANTIUM_SZ 4000
#define SCULL_QSET_LEN 1000

/*
 * IOCS_* set a value through a pointer argument
 * IOCT_* set a value, throught a value argument
 * IOCG_* get a value through a pointer argument
 * IOCQ_* get a value through return value
 * IOCX_* combine IOCS_* and IOCG_*
 * IOCH_* combine IOCT_* and IOCQ_*
 */

#define SCULL_IOCMAGIC 0xFE
#define SCULL_IOCRESET _IO(SCULL_IOCMAGIC, 0)
#define SCULL_IOCSQUANTIUM _IOW(SCULL_IOCMAGIC, 1, int)
#define SCULL_IOCSQSET _IOW(SCULL_IOCMAGIC, 2, int)
#define SCULL_IOCTQUANTIUM _IOW(SCULL_IOCMAGIC, 3, int)
#define SCULL_IOCTQSET _IOW(SCULL_IOCMAGIC, 4, int)
#define SCULL_IOCGQUANTIUM _IOR(SCULL_IOCMAGIC, 5, int)
#define SCULL_IOCGQSET _IOR(SCULL_IOCMAGIC, 6, int)
#define SCULL_IOCQQUANTIUM _IO(SCULL_IOCMAGIC, 7)
#define SCULL_IOCQQSET _IO(SCULL_IOCMAGIC, 8)
#define SCULL_IOCXQUANTIUM _IOWR(SCULL_IOCMAGIC, 9, int)
#define SCULL_IOCXQSET _IOWR(SCULL_IOCMAGIC, 10, int)
#define SCULL_IOCHQUANTIUM _IOWR(SCULL_IOCMAGIC, 11, int)
#define SCULL_IOCHQSET _IOWR(SCULL_IOCMAGIC, 12, int)

struct scull_qset {
	struct scull_qset *next;
	void **data;
};

struct scull_dev {
	struct cdev cdev;
	struct device *dev;
	struct scull_qset *data;
	struct mutex lock;

	/* size of a quantium */
	int quantium_sz;

	/* size of data array of a qset */
	int qset_len;

	/* total amount of data owned by this device */
	unsigned long size;
};

#endif /* _SCULL_H */
