/*
 * Driver that provides dynamic CMA using u-dma-buf for accelerators that require CM to perform DMA transactions
 *
 * Copyright (C) 2025 Milos Dordevic, CEI-UPM.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/param.h>
#include <linux/ioctl.h>
#include <linux/errno.h>
#include <linux/fs.h>

#include "accel_dyn_cma.h"

#define ACCEL_DYN_CMA_DRV_NAME "accel_dyn_cma"
#define ACCEL_DYN_CMA_MAX_NUM_BUFS (8)

struct strela_dma_alloc_info {
	struct device *buf_dev;
	u32 size;
};

struct accel_dyn_cma_dev_info {
	struct cdev cdev;
	// buffer IDs correspond to the index into the array
	struct accel_dyn_cma_alloc_info alloc_info[ACCEL_DYN_CMA_MAX_NUM_BUFS];
	struct mutex lock;
};

static int interface_major = -1;
static int interface_minor = -1;

static struct accel_dyn_cma_dev_info accel_dyn_cma_dev = {0};

static void __exit accel_dyn_cma_exit(void)
{
	cdev_del(&accel_dyn_cma_dev.cdev);
	unregister_chrdev_region(MKDEV(interface_major, interface_minor), 1);
}

static int __init accel_dyn_cma_init(void)
{
	dev_t dev_num = 0;
	int result = 0;

	result = alloc_chrdev_region(&dev_num, 0, 1, ACCEL_DYN_CMA_DRV_NAME);
	
	if (result < 0) {
		pr_warn("accel_dyn_cma_dev: failed to allocate character device region\n");
		goto fail;
	}

	interface_major = MAJOR(dev_num);
	interface_minor = MINOR(dev_num);

	// Init cdev and add it to the system
	cdev_init(&accel_dyn_cma_dev.cdev, &accel_dyn_cma_fops);
	accel_dyn_cma_dev.cdev.owner = THIS_MODULE;
	accel_dyn_cma_dev.cdev.ops = &accel_dyn_cma_fops;
	result = cdev_add(&ioctl_d_interface.cdev, dev_num, 1);

	if (result < 0) {
		pr_warn( "accel_dyn_cma_dev: error when adding device\n");
		goto fail;
	}

	pr_info("accel_dyn_cma: module loaded\n");
	return 0;

fail:
	cdev_del(&accel_dyn_cma_dev.cdev);
	unregister_chrdev_region(MKDEV(interface_major, interface_minor), 1);
	
	return result;
}

int accel_dyn_cma_open(struct inode* inode, struct file* filp)
{
	struct accel_dyn_cma_dev_info* accel_dyn_cma_dev;

	accel_dyn_cma_dev = container_of(inode->i_cdev, accel_dyn_cma_dev_info, cdev);
	filp->private_data = accel_dyn_cma_dev;

	return 0;
}

long accel_dyn_cma_ioctl(struct file* filp, unsigned int cmd, unsigned long arg)
{
	long res = 0;
	struct accel_dyn_cma_alloc_req_ioctl_arg alloc_req;
    u8 id;

	struct accel_dyn_cma_dev_info *accel_dyn_cma_dev = (accel_dyn_cma_dev_info*) filp->private_data;

	mutex_lock(&accel_dyn_cma_dev->lock);

  	switch (cmd) {
    	case ACCEL_DYN_CMA_IOCTL_ALLOC: {
			if (copy_from_user(&alloc_req, (void __user *)arg, sizeof(struct accel_dyn_cma_alloc_req_ioctl_arg))) {
				pr_err("accel_dyn_cma: copying of buffer allocation request parameters from user space failed\n");

				res = -EFAULT;

				goto ioctl_fail;
			}

			int free_buff_id = -1;

			for (int i = 0; i < ACCEL_DYN_CMA_MAX_NUM_BUFS; i++) {
				if(accel_dyn_cma_dev->alloc_info[i].size == 0) {
					free_buff_id = i;
					break;
				}
			}

			if(free_buff_id == -1) {
				pr_err("accel_dyn_cma: no more free buffers available for memory allocation\n");

				res = -EFAULT;

				goto ioctl_fail;
			}
			else {
				accel_dyn_cma_dev->alloc_info[free_buff_id].buf_dev = u_dma_buf_device_create(NULL, free_buff_id, alloc_req.size, 0, &accel_dyn_cma_dev->cdev);

				if(IS_ERR_OR_NULL(accel_dyn_cma_dev->alloc_info[free_buff_id].buf_dev)) {
					pr_err("accel_dyn_cma: error when creating buffer\n");

					accel_dyn_cma_dev->alloc_info[free_buff_id].size = 0;
					accel_dyn_cma_dev->alloc_info[free_buff_id].buf_dev = NULL;

					//alloc_req.size = 0;
					//alloc_req.id = -1;

					res = -EFAULT;

					goto ioctl_fail;
				}
				else {
					accel_dyn_cma_dev->alloc_info[free_buff_id].size = alloc_req.size;
					alloc_req.id = free_buff_id;

					if (copy_to_user((void __user *)arg, &alloc_req, sizeof(struct accel_dyn_cma_alloc_req_ioctl_arg))) {
						pr_err("accel_dyn_cma: copying of buffer allocation response parameters to user space failed\n");

        				res = -EFAULT;

						goto ioctl_fail;
      				}
				}
			}
		}
      	break;

		case ACCEL_DYN_CMA_IOCTL_FREE: {
	    	int id = 0;

			if (copy_from_user(&id, (void __user *)ioctl_param, sizeof(int))) {
				pr_err("accel_dyn_cma: copying of buffer ID from user space failed\n");

				res = -EFAULT;

				goto ioctl_fail;
			}

			if((id < 0) || (id >= ACCEL_DYN_CMA_MAX_NUM_BUFS)) {
				pr_err("accel_dyn_cma: invalid buffer ID provided\n");

				res = -EFAULT;

				goto ioctl_fail;
			}

			if((accel_dyn_cma_dev->alloc_info[id].size == 0) || (accel_dyn_cma_dev->alloc_info[id].buf_dev == NULL)) {
				pr_err("accel_dyn_cma: buffer with the provided ID was already deallocated\n");

				res = -EFAULT;

				goto ioctl_fail;
			}

			dma_free_coherent(strela_dev->miscdev.parent, strela_dev->alloc_info[id].size, strela_dev->alloc_info[id].vptr, strela_dev->alloc_info[id].dmaptr);

			res = u_dma_buf_device_remove(accel_dyn_cma_dev->alloc_info[id].buf_dev);
			
			if (res) {
				pr_err("accel_dyn_cma: error when deallocating the buffer\n");

				goto ioctl_fail;
			}

			accel_dyn_cma_dev->alloc_info[id].size = 0;
			accel_dyn_cma_dev->alloc_info[id].buf_dev = NULL;
		}
      	break;

    	default:
      		break;
  	}

	mutex_unlock(&accel_dyn_cma_dev->lock);

	return 0;

ioctl_fail:
	mutex_unlock(&accel_dyn_cma_dev->lock);

	return res;
}

struct file_operations accel_dyn_cma_fops = {
	.owner = THIS_MODULE,
	.read = NULL,
	.write = NULL,
	.open = accel_dyn_cma_open,
	.unlocked_ioctl = accel_dyn_cma_ioctl,
	.release = NULL
};

module_init(accel_dyn_cma_init);
module_exit(accel_dyn_cma_exit);

MODULE_DESCRIPTION("Driver that provides dynamic CMA using u-dma-buf for accelerators that require CM to perform DMA transactions");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Milos Dordevic <milos.dordevic@upm.es>");
MODULE_ALIAS("accel_dyn_cma");
MODULE_LICENSE("GPL v2");
