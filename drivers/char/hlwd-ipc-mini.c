/*
 * Nintendo Wii Hollywood IPC (MINI) Character Device Driver
 *
 * Copyright (C) 2025 Michael "Techflash" Garofalo
 *
 * Based on drivers/char/xenon-ipc.c:
 * Copyright (C) 2010 Herbert Poetzl
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <asm/hlwd-ipc-mini.h>

#define DRV_NAME	"hlwd-ipc-mini-chardev"
#define DRV_VERSION	"0.1"


/* single access for now */

static unsigned long is_active;

static ssize_t ipc_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	struct ipc_request_mini resp;
	int ret;

	if ((count != sizeof(struct ipc_request_mini)) || *ppos)
		return -EINVAL;

	ret = ipc_receive_mini(&resp, 3);
	if (ret) {
		pr_err("ipc_read: ipc_receive_mini failed with: %d\n", ret);
		return -EIO;
	}

	if (copy_to_user(buf, &resp, sizeof(struct ipc_request_mini)))
		return -EFAULT;

	return sizeof(struct ipc_request_mini);
}

/*
 * Exepcts in format (similar-ish to the actual MINI IPC format):
 * 4B: [code]
 * 4B: [number of args]
 * number of args * 4B: [args, up to (inclusive) 6 of them]
 */
static ssize_t ipc_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	u32 req[8];
	int ret;

	if (count > 32 || count < 8 || *ppos)
		return -EINVAL;

	if (copy_from_user(req, buf, count))
		return -EFAULT;

	if (req[1] > 6)
		return -EINVAL;

	/* TODO: there's probably a cleaner way to do this */
	switch (req[1]) {
	case 0: {
		ret = ipc_post_mini(req[0], 0, 0);
		break;
	}
	case 1: {
		ret = ipc_post_mini(req[0], 0, 1, req[2]);
		break;
	}
	case 2: {
		ret = ipc_post_mini(req[0], 0, 2, req[2], req[3]);
		break;
	}
	case 3: {
		ret = ipc_post_mini(req[0], 0, 3, req[2], req[3], req[4]);
		break;
	}
	case 4: {
		ret = ipc_post_mini(req[0], 0, 4, req[2], req[3], req[4], req[5]);
		break;
	}
	case 5: {
		ret = ipc_post_mini(req[0], 0, 5, req[2], req[3], req[4], req[5], req[6]);
		break;
	}
	case 6: {
		ret = ipc_post_mini(req[0], 0, 5, req[2], req[3], req[4], req[5], req[6], req[7]);
		break;
	}
	}
	if (ret)
		return ret;

	return count;
}

static long ipc_ioctl(struct file *file,
		      unsigned int cmd, unsigned long arg)
{
	return -ENODEV;
}

static int ipc_open(struct inode *inode, struct file *file)
{
	if (test_and_set_bit(0, &is_active))
		return -EBUSY;

	return nonseekable_open(inode, file);
}

static int ipc_release(struct inode *inode, struct file *file)
{
	clear_bit(0, &is_active);
	return 0;
}


const struct file_operations ipc_fops = {
	.owner		= THIS_MODULE,
	.read		= ipc_read,
	.write		= ipc_write,
	.unlocked_ioctl	= ipc_ioctl,
	.open		= ipc_open,
	.release	= ipc_release,
};

static struct miscdevice ipc_dev = {
	.minor =  MISC_DYNAMIC_MINOR,
	"hlwd-ipc-mini",
	&ipc_fops
};

static int __init ipc_init(void)
{
	int ret = 0;

	pr_info("Hollywood IPC (MINI) char driver version " DRV_VERSION "\n");

	ret = misc_register(&ipc_dev);
	return ret;
}

static void __exit ipc_exit(void)
{
	misc_deregister(&ipc_dev);
}

module_init(ipc_init);
module_exit(ipc_exit);

MODULE_AUTHOR("Michael \"Techflash\" Garofalo");
MODULE_DESCRIPTION("Character Interface for \"MINI\" firmware on Hollywood IPC");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
