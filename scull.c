/*
 * TODO: override scull_quantium_sz and scull_qset_len if given to
 * module loading parameters.
 */

#define pr_fmt(fmt) KBUILD_BASENAME ": " fmt

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/capability.h>
#include "scull.h"

static dev_t dev_region;
static int scull_major = SCULL_MAJOR;
static int scull_minor = SCULL_MINOR;

static int scull_quantium_sz = SCULL_QUANTIUM_SZ;
static int scull_qset_len = SCULL_QSET_LEN;

static struct proc_dir_entry *scull_proc;
static struct class *scull_class;
static struct scull_dev scull_devices[] = { {}, {}, {}, {} };
static int scull_devices_nbr = sizeof(scull_devices) / sizeof(struct scull_dev);

static void scull_trim(struct scull_dev *dev)
{
	struct scull_qset *qset;
	struct scull_qset *next;
	int i;

	for (qset = dev->data; qset; qset = next) {
		if (qset->data) {
			for (i = 0; i < dev->qset_len; i++) {
				if (qset->data[i]) {
					memset(qset->data[i], 0,
					       dev->quantium_sz);
					kfree(qset->data[i]);
					qset->data[i] = NULL;
				}
			}
			kfree(qset->data);
		}
		next = qset->next;
		memset(qset, 0, sizeof(struct scull_qset));
		kfree(qset);
	}
	dev->size = 0;
	dev->data = NULL;
}

static struct scull_qset *scull_new_qset(void)
{
	struct scull_qset *qset = kmalloc(sizeof(struct scull_qset),
					  GFP_KERNEL);

	if (!qset)
		return NULL;
	memset(qset, 0, sizeof(struct scull_qset));
	return qset;
}

static struct scull_qset *scull_follow(struct scull_dev *dev, int i)
{
	struct scull_qset *qset = dev->data;

	if (!qset) {
		qset = scull_new_qset();
		if (!qset)
			goto out;
		dev->data = qset;
	}

	while (qset && i--) {
		if (!qset->next)
			qset->next = scull_new_qset();
		qset = qset->next;
	}

out:
	return qset;
}

static ssize_t scull_read(struct file *filp, char __user *buffer, size_t count,
			  loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *qset;
	int ret = 0;
	int q_sz;
	int item_sz;
	int size;
	int qset_i;
	int q_i;
	int q_off;
	int rest;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	q_sz = dev->quantium_sz;
	item_sz = dev->qset_len * q_sz;
	size = dev->size;

	if (*f_pos >= size)
		goto out;

	if (*f_pos + count >= size)
		count = size - *f_pos;

	qset_i = (long)*f_pos / item_sz;
	rest = (long)*f_pos % item_sz;
	q_i = rest / q_sz;
	q_off = rest % q_sz;

	qset = scull_follow(dev, qset_i);
	if (!qset || !qset->data || !qset->data[q_i])
		goto out;

	if (count > q_sz - q_off)
		count = q_sz - q_off;

	if (copy_to_user(buffer, qset->data[q_i] + q_off, count)) {
		ret = -EFAULT;
		goto out;
	}

	*f_pos += count;
	ret = count;

out:
	mutex_unlock(&dev->lock);
	return ret;
}

static ssize_t scull_write(struct file *filp, const char __user *buffer,
			   size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *qset;
	int ret = -ENOMEM;
	int qset_len;
	int q_sz;
	int item_sz;
	int rest;
	int qset_i;
	int q_i;
	int q_off;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	qset_len = dev->qset_len;
	q_sz = dev->quantium_sz;
	item_sz = qset_len * q_sz;
	qset_i = (long)*f_pos / item_sz;
	rest = (long)*f_pos % item_sz;
	q_i = rest / q_sz;
	q_off = rest % q_sz;

	qset = scull_follow(dev, qset_i);
	if (!qset)
		goto out;

	if (!qset->data) {
		qset->data = kmalloc_array(qset_len, sizeof(void *),
					   GFP_KERNEL);
		if (!qset->data)
			goto out;
		memset(qset->data, 0, qset_len * sizeof(void *));
	}

	if (!qset->data[q_i]) {
		qset->data[q_i] = kmalloc(q_sz, GFP_KERNEL);
		if (!qset->data[q_i])
			goto out;
		memset(qset->data[q_i], 0, q_sz);
	}

	if (count > q_sz - q_off)
		count = q_sz - q_off;

	if (copy_from_user(qset->data[q_i] + q_off, buffer, count)) {
		ret = -EFAULT;
		goto out;
	}

	*f_pos += count;
	dev->size += count;
	ret = count;

	if (*f_pos > dev->size)
		pr_alert("f_pos (%lld) > size (%lu)\n", *f_pos, dev->size);

out:
	mutex_unlock(&dev->lock);
	return ret;
}

static int scull_open(struct inode *inode, struct file *filp)
{
	static struct scull_dev *dev;

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;
	if (filp->f_flags & O_WRONLY) {
		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;
		dev->qset_len = scull_qset_len;
		dev->quantium_sz = scull_quantium_sz;
		scull_trim(dev);
		mutex_unlock(&dev->lock);
	}
	return 0;
}

static int scull_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static long scull_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	switch(cmd)
	{
	case SCULL_IOCRESET:
		scull_qset_len = SCULL_QSET_LEN;
		scull_quantium_sz = SCULL_QUANTIUM_SZ;
		break;
	case SCULL_IOCSQUANTIUM:
		if (!capable(CAP_SYS_ADMIN))
		    return -EPERM;
		ret = get_user(scull_quantium_sz, (int __user *)arg);
		break;
	case SCULL_IOCSQSET:
		if (!capable(CAP_SYS_ADMIN))
		    return -EPERM;
		ret = get_user(scull_qset_len, (int __user *)arg);
		break;
	case SCULL_IOCTQUANTIUM:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		scull_quantium_sz = (int)arg;
		break;
	case SCULL_IOCTQSET:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		scull_qset_len = (int)arg;
		break;
	case SCULL_IOCGQUANTIUM:
		ret = put_user(scull_quantium_sz, (int __user *)arg);
		break;
	case SCULL_IOCGQSET:
		ret = put_user(scull_qset_len, (int __user *)arg);
		break;
	case SCULL_IOCQQUANTIUM:
		ret = scull_quantium_sz;
		break;
	case SCULL_IOCQQSET:
		ret = scull_qset_len;
		break;
	case SCULL_IOCXQUANTIUM:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		ret = get_user(scull_quantium_sz, (int __user *)arg);
		if (ret)
			return ret;
		ret = scull_quantium_sz;
		break;
	case SCULL_IOCXQSET:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		ret = get_user(scull_qset_len, (int __user *)arg);
		if (ret)
			return ret;
		ret = scull_qset_len;
		break;
	case SCULL_IOCHQUANTIUM:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		scull_quantium_sz = (int)arg;
		ret = scull_quantium_sz;
		break;
	case SCULL_IOCHQSET:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		scull_qset_len = (int)arg;
		ret = scull_qset_len;
		break;
	default:
		ret = -ENOTTY;
	}

	return ret;
}

static const struct file_operations scull_fops = {
	.owner = THIS_MODULE,
	.read = scull_read,
	.write = scull_write,
	.open = scull_open,
	.release = scull_release,
	.unlocked_ioctl = scull_ioctl,

	/* https://lwn.net/Articles/119652/
	   Using the same function should be OK. */
	.compat_ioctl = scull_ioctl
};

static int scull_setup_cdev(struct scull_dev *dev, int index)
{
	int ret = 0;
	dev_t dev_num;

	mutex_init(&dev->lock);
	dev_num = MKDEV(scull_major, scull_minor + index);
	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&dev->cdev, dev_num, 1);
	if (ret) {
		pr_warn("can't add cdev %d (err %d)\n", index, ret);
		goto out;
	}

	dev->dev = device_create(scull_class, NULL, dev_num, NULL,
				 "scull%d", index);
	if (!dev->dev) {
		pr_warn("can't create device %d\n", index);
		ret = -EAGAIN;
	}
out:
	return ret;
}

static void scull_remove(struct scull_dev *dev)
{
	struct cdev *cdev = &dev->cdev;

	device_destroy(scull_class, cdev->dev);
	cdev_del(cdev);

	/* device is not accessible anymore, didn't need lock */
	scull_trim(dev);
}

static void *scull_seq_start(struct seq_file *s, loff_t *pos)
{
	if (*pos >= scull_devices_nbr)
		return NULL;
	return scull_devices + *pos;
}

static void *scull_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;
	if (*pos >= scull_devices_nbr)
		return NULL;
	return scull_devices + *pos;
}

static int scull_seq_show(struct seq_file *s, void *v)
{
	struct scull_dev *dev = (struct scull_dev *)v;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;
	seq_printf(s, "scull%ld %d %d %lu\n", dev - scull_devices,
		   dev->quantium_sz, dev->qset_len, dev->size);
	mutex_unlock(&dev->lock);
	return 0;
}

static void scull_seq_stop(struct seq_file *s, void *v)
{
}

static const struct seq_operations scull_seq_ops = {
	.start = scull_seq_start,
	.next = scull_seq_next,
	.show = scull_seq_show,
	.stop = scull_seq_stop
};

static int scull_proc_open(struct inode *inode, struct file *filp)
{
	return seq_open(filp, &scull_seq_ops);
}

static const struct file_operations scull_proc_ops = {
	.owner = THIS_MODULE,
	.open = scull_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release
};

static int __init scull_init(void)
{
	int ret = 0;

	if (scull_major) {
		dev_region = MKDEV(scull_major, scull_minor);
		ret = register_chrdev_region(dev_region, SCULL_MAX_DEVICES,
					     "scull");
	} else {
		ret = alloc_chrdev_region(&dev_region, 0, SCULL_MAX_DEVICES,
					  "scull");
		scull_major = MAJOR(dev_region);
	}

	if (ret) {
		pr_warn("can't get major %d\n", scull_major);
		goto out;
	}

	scull_class = class_create(THIS_MODULE, "scull");
	if (!scull_class) {
		pr_warn("can't create class\n");
		ret = -EAGAIN;
		goto fail_class;
	}

	ret = scull_setup_cdev(&scull_devices[0], 0);
	if (ret)
		goto fail_scull0;
	ret = scull_setup_cdev(&scull_devices[1], 1);
	if (ret)
		goto fail_scull1;
	ret = scull_setup_cdev(&scull_devices[2], 2);
	if (ret)
		goto fail_scull2;
	ret = scull_setup_cdev(&scull_devices[3], 3);
	if (ret)
		goto fail_scull3;

	scull_proc = proc_create("scull", 0, NULL, &scull_proc_ops);
	if (!scull_proc)
		goto fail_procfs;
	goto out;

fail_procfs:
	scull_remove(&scull_devices[3]);
fail_scull3:
	scull_remove(&scull_devices[2]);
fail_scull2:
	scull_remove(&scull_devices[1]);
fail_scull1:
	scull_remove(&scull_devices[0]);
fail_scull0:
	class_destroy(scull_class);
fail_class:
	unregister_chrdev_region(dev_region, SCULL_MAX_DEVICES);
out:
	return ret;
}

static void __exit scull_exit(void)
{
	proc_remove(scull_proc);
	scull_remove(&scull_devices[0]);
	scull_remove(&scull_devices[1]);
	scull_remove(&scull_devices[2]);
	scull_remove(&scull_devices[3]);
	class_destroy(scull_class);
	unregister_chrdev_region(dev_region, SCULL_MAX_DEVICES);
}

MODULE_DESCRIPTION("Simple implementation of scull driver introduced in LDD");
MODULE_AUTHOR("Colin Vidal <colin@cvidal.org>");
MODULE_LICENSE("GPL v2");

module_init(scull_init);
module_exit(scull_exit);
