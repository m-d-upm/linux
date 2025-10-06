/*
 * Driver for the STRELA CGRA with embedded DMA
 *
 * Copyright (C) 2025 Juan Granja, CEI-UPM.
 * Copyright (C) 2025 Milos Dordevic, CEI-UPM.
 */

#include <linux/of_platform.h>
#include <linux/bitfield.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/sched.h>
#include <linux/param.h>
#include <linux/ioctl.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/jiffies.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/wait.h>

#include "strela.h"

#define STRELA_CONF_TIMEOUT			(5) // seconds
#define STRELA_TIMEOUT				(30) // seconds

#define STRELA_MAX_DEV_SUPPORTED	(4) // number of devices supported by this driver

// declared here, defined in udmabuf driver
struct device*   u_dma_buf_device_search(const char* name, int id);
int              u_dma_buf_device_getmap(struct device *dev, size_t* size, void** virt_addr, dma_addr_t* phys_addr);

struct strela_reg_addr_map {
	void __iomem *strela_ctrl;
	void __iomem *strela_conf_addr;
	void __iomem *strela_conf_size;
	void __iomem *strela_in0_addr;
	void __iomem *strela_in0_size;
	void __iomem *strela_in1_addr;
	void __iomem *strela_in1_size;
	void __iomem *strela_in2_addr;
	void __iomem *strela_in2_size;
	void __iomem *strela_in3_addr;
	void __iomem *strela_in3_size;
	void __iomem *strela_out0_addr;
	void __iomem *strela_out0_size;
	void __iomem *strela_out1_addr;
	void __iomem *strela_out1_size;
	void __iomem *strela_out2_addr;
	void __iomem *strela_out2_size;
	void __iomem *strela_out3_addr;
	void __iomem *strela_out3_size;
	void __iomem *strela_cntr_conf;
	void __iomem *strela_cntr_exec;
	void __iomem *strela_cntr_stall;
	void __iomem *strela_out_arb_hold;
	void __iomem *strela_reset_dma;
	void __iomem *strela_am_opa;
	void __iomem *strela_am_opb;
	void __iomem *strela_am_opr;
};

struct buf_info {
	void *vptr;
	dma_addr_t dmaptr;
	size_t size;
};

struct strela_device {
	struct miscdevice miscdev;
	void __iomem *reg;
	struct strela_reg_addr_map regs;
	struct mutex lock;
	wait_queue_head_t wq_conf;
	wait_queue_head_t wq_exec;
	bool wake_up_int_conf;
	bool wake_up_int_exec;
};

static int strela_open(struct inode *inode, struct file *fp)
{
	return 0;
}

static int strela_release(struct inode *inode, struct file *fp)
{
	return 0;
}

static long strela_ioctl(struct file *fp, unsigned int ioctl_num, unsigned long ioctl_param)
{
	long ret = 0;
	
	struct strela_device *strela_dev = fp->private_data;

	mutex_lock(&strela_dev->lock);

	switch (ioctl_num) {
		case IOCTL_STRELA_CONTROL: {
			struct strela_ctrl strela_ctrl;

			//dev_info(strela_dev->miscdev.parent, "Copying new settings from user\n");

			if (copy_from_user(&strela_ctrl, (void __user *)ioctl_param, sizeof(struct strela_ctrl))) {
				dev_err(strela_dev->miscdev.parent, "STRELA: Copying of CSRs config from user failed\n");

				ret = -EFAULT;
				goto ioctl_fail;
			}

			struct device* in_buf_dev = u_dma_buf_device_search(NULL, strela_ctrl.in_buf_id);

			if(IS_ERR_OR_NULL(in_buf_dev)) {
				dev_err(strela_dev->miscdev.parent, "STRELA: Failed to obtain input buffer handle, check if the provided ID is correct\n");

				ret = -EFAULT;
				goto ioctl_fail;
			}

			struct device* out_buf_dev = u_dma_buf_device_search(NULL, strela_ctrl.out_buf_id);

			if(IS_ERR_OR_NULL(out_buf_dev)) {
				dev_err(strela_dev->miscdev.parent, "STRELA: Failed to obtain output buffer handle, check if the provided ID is correct\n");

				ret = -EFAULT;
				goto ioctl_fail;
			}

			struct buf_info in_buf_info;
			struct buf_info out_buf_info;

			if(u_dma_buf_device_getmap(in_buf_dev, &in_buf_info.size, &in_buf_info.vptr, &in_buf_info.dmaptr)) {
				dev_err(strela_dev->miscdev.parent, "STRELA: Failed to obtain input buffer address map\n");

				ret = -EFAULT;
				goto ioctl_fail;
			}

			if(u_dma_buf_device_getmap(out_buf_dev, &out_buf_info.size, &out_buf_info.vptr, &out_buf_info.dmaptr)) {
				dev_err(strela_dev->miscdev.parent, "STRELA: Failed to obtain output buffer address map\n");

				ret = -EFAULT;
				goto ioctl_fail;
			}

			//dev_info(strela_dev->miscdev.parent, "Setting config values\n");

			// configure STRELA device's DMA addresses
			iowrite32(in_buf_info.dmaptr + strela_ctrl.csrs.conf_offs, strela_dev->regs.strela_conf_addr);
			iowrite32(strela_ctrl.csrs.conf_count * 4U, strela_dev->regs.strela_conf_size);

			//dev_info(strela_dev->miscdev.parent, "Setting values for IN registers\n");

			iowrite32(in_buf_info.dmaptr + strela_ctrl.csrs.in0_offs * 4U, strela_dev->regs.strela_in0_addr);
			iowrite32(STRELA_IN_BITS_STRIDE_COUNT(strela_ctrl.csrs.in0_stride, strela_ctrl.csrs.in0_count), strela_dev->regs.strela_in0_size);
			iowrite32(in_buf_info.dmaptr + strela_ctrl.csrs.in1_offs * 4U, strela_dev->regs.strela_in1_addr);
			iowrite32(STRELA_IN_BITS_STRIDE_COUNT(strela_ctrl.csrs.in1_stride, strela_ctrl.csrs.in1_count), strela_dev->regs.strela_in1_size);
			iowrite32(in_buf_info.dmaptr + strela_ctrl.csrs.in2_offs * 4U, strela_dev->regs.strela_in2_addr);
			iowrite32(STRELA_IN_BITS_STRIDE_COUNT(strela_ctrl.csrs.in2_stride, strela_ctrl.csrs.in2_count), strela_dev->regs.strela_in2_size);
			iowrite32(in_buf_info.dmaptr + strela_ctrl.csrs.in3_offs * 4U, strela_dev->regs.strela_in3_addr);
			iowrite32(STRELA_IN_BITS_STRIDE_COUNT(strela_ctrl.csrs.in3_stride, strela_ctrl.csrs.in3_count), strela_dev->regs.strela_in3_size);

			//dev_info(strela_dev->miscdev.parent, "Setting values for OUT registers\n");

			iowrite32(out_buf_info.dmaptr + strela_ctrl.csrs.out0_offs * 4U, strela_dev->regs.strela_out0_addr);
			iowrite32(strela_ctrl.csrs.out0_count * 4U, strela_dev->regs.strela_out0_size);
			iowrite32(out_buf_info.dmaptr + strela_ctrl.csrs.out1_offs * 4U, strela_dev->regs.strela_out1_addr);
			iowrite32(strela_ctrl.csrs.out1_count * 4U, strela_dev->regs.strela_out1_size);
			iowrite32(out_buf_info.dmaptr + strela_ctrl.csrs.out2_offs * 4U, strela_dev->regs.strela_out2_addr);
			iowrite32(strela_ctrl.csrs.out2_count * 4U, strela_dev->regs.strela_out2_size);
			iowrite32(out_buf_info.dmaptr + strela_ctrl.csrs.out3_offs * 4U, strela_dev->regs.strela_out3_addr);
			iowrite32(strela_ctrl.csrs.out3_count * 4U, strela_dev->regs.strela_out3_size);

			iowrite32(1U, strela_dev->regs.strela_out_arb_hold);

			//dev_info(strela_dev->miscdev.parent, "STRELA device on hold\n");

			break;
		}

		case IOCTL_STRELA_CONFIG: {
			// TO-DO: flush data L1 cache either here or in user-space library

			//dev_info(strela_dev->miscdev.parent, "Resetting STRELA\n");
			// reset STRELA CGRA and DMA
			iowrite32(STRELA_CTRL_BIT_CLEAR_STATE, strela_dev->regs.strela_ctrl);
			iowrite32(STRELA_CTRL_BIT_CLEAR_CONFIG, strela_dev->regs.strela_ctrl);
			iowrite32(1U, strela_dev->regs.strela_reset_dma);

			//dev_info(strela_dev->miscdev.parent, "Loading new config into STRELA\n");
			// start config read
			iowrite32(STRELA_CTRL_BIT_LOAD_CONFIG, strela_dev->regs.strela_ctrl);

			/*
			end_jiffies = jiffies + STRELA_CONF_TIMEOUT * HZ;

			// wait for it to finish
			do {
				if(time_after_eq(jiffies, end_jiffies)) {
					ret = -EIO;
					goto ioctl_fail;
				}
				cpu_relax();
			} while (!(ioread32(strela_dev->regs.strela_ctrl) & STRELA_CTRL_BIT_DONE_CONFIG));
			*/

			ret = wait_event_interruptible(strela_dev->wq_conf, strela_dev->wake_up_int_conf == true);
			strela_dev->wake_up_int_conf = false;

			//dev_info(strela_dev->miscdev.parent, "New config loaded into STRELA\n");

			break;
		}

		case IOCTL_STRELA_EXEC: {
			// TO-DO: flush data L1 cache either here or in user-space library

			//dev_info(strela_dev->miscdev.parent, "Starting STRELA execution\n");

			// start execution
			iowrite32(STRELA_CTRL_BIT_START_EXEC, strela_dev->regs.strela_ctrl);

			/*
			end_jiffies = jiffies + STRELA_TIMEOUT * HZ;

			// wait for it to finish
			do {
				if(time_after_eq(jiffies, end_jiffies)){
					ret = -EIO;
					goto ioctl_fail;
				}
				cpu_relax();
			} while (!(ioread32(strela_dev->regs.strela_ctrl) & STRELA_CTRL_BIT_DONE_EXEC));
			*/
			
			ret = wait_event_interruptible(strela_dev->wq_exec, strela_dev->wake_up_int_exec == true);
			strela_dev->wake_up_int_exec = false;

			// TO-DO: flush data L1 cache either here or in user-space library

			//dev_info(strela_dev->miscdev.parent, "STRELA finished executing\n");

			break;
		}
	}

ioctl_fail:
	mutex_unlock(&strela_dev->lock);

	return ret;
}

static irqreturn_t strela_conf_process(int irq, void *data)
{
	struct strela_device *strela_dev = (struct strela_device *)data;

	strela_dev->wake_up_int_conf = true;
	wake_up_interruptible(&strela_dev->wq_conf);

	dev_info(strela_dev->miscdev.parent, "IRQ: %d handled\n", irq);

	return IRQ_HANDLED;
}

static irqreturn_t strela_conf_irq_check(int irq, void *data)
{
	struct strela_device *strela_dev = (struct strela_device *)data;
	u32 status_reg = ioread32(strela_dev->regs.strela_ctrl);

	if (status_reg & STRELA_CTRL_BIT_PENDING_INT_CONFIG)
	{
		iowrite32(STRELA_CTRL_BIT_CLEAR_INT_CONFIG, strela_dev->regs.strela_ctrl);

		return IRQ_WAKE_THREAD;
	}

	return IRQ_NONE;
}

static irqreturn_t strela_exec_process(int irq, void *data)
{
	struct strela_device *strela_dev = (struct strela_device *)data;
	
	strela_dev->wake_up_int_exec = true;
	wake_up_interruptible(&strela_dev->wq_exec);

	dev_info(strela_dev->miscdev.parent, "IRQ: %d handled\n", irq);

	return IRQ_HANDLED;
}

static irqreturn_t strela_exec_irq_check(int irq, void *data)
{
	struct strela_device *strela_dev = (struct strela_device *)data;
	u32 status_reg = ioread32(strela_dev->regs.strela_ctrl);

	if (status_reg & STRELA_CTRL_BIT_PENDING_INT_EXEC)
	{	
		iowrite32(STRELA_CTRL_BIT_CLEAR_INT_EXEC, strela_dev->regs.strela_ctrl);

		return IRQ_WAKE_THREAD;
	}

	return IRQ_NONE;
}

static const struct file_operations strela_fops = {
	.owner		= THIS_MODULE,
	.open		= strela_open,
	.release	= strela_release,
	.unlocked_ioctl = strela_ioctl
};

static int strela_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct strela_device *strela_dev = NULL;

	struct resource *res = NULL;

	int ret = 0;

	int irq_conf = -1;
	int irq_exec = -1;

	static int strela_dev_num = 0;
	char strela_dev_name[10];

	if (strela_dev_num >= STRELA_MAX_DEV_SUPPORTED)
	{
		dev_err(dev, "STRELA: Maximum number of devices supported by the driver reached\n");
		return -ENOMEM;
	}

	// allocate memory for the STRELA device
	strela_dev = devm_kzalloc(dev, sizeof(struct strela_device), GFP_KERNEL);

	if (!strela_dev)
	{
		dev_err(dev, "STRELA: Failed to acquire resources for allocating memory for STRELA device\n");
		return -ENOMEM;
	}

    snprintf(strela_dev_name, sizeof(strela_dev_name), "strela%d", strela_dev_num);
	++strela_dev_num;

	// configure STRELA dev representation
	strela_dev->miscdev.fops = &strela_fops;
	strela_dev->miscdev.parent = dev;
	strela_dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	strela_dev->miscdev.name = strela_dev_name;

	mutex_init(&strela_dev->lock);
	init_waitqueue_head(&strela_dev->wq_conf);
	init_waitqueue_head(&strela_dev->wq_exec);
	
	// map regmap to kernel memory
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (!res) {
		dev_err(dev, "STRELA: Failed to acquire resources for register-to-kernel memory mapping\n");
		return -EINVAL;
	}

	strela_dev->reg = devm_platform_get_and_ioremap_resource(pdev, 0, &res);

	if (IS_ERR(strela_dev->reg)) {
		ret = dev_err_probe(dev, PTR_ERR(strela_dev->reg), "STRELA: Could not map the register region\n");
		goto fail;
	};

	strela_dev->regs.strela_ctrl = strela_dev->reg + STRELA_CTRL_A;
	strela_dev->regs.strela_conf_addr = strela_dev->reg + STRELA_CONF_ADDR_A;
	strela_dev->regs.strela_conf_size = strela_dev->reg + STRELA_CONF_SIZE_A;
	strela_dev->regs.strela_in0_addr = strela_dev->reg + STRELA_IN0_ADDR_A;
	strela_dev->regs.strela_in0_size = strela_dev->reg + STRELA_IN0_SIZE_A;
	strela_dev->regs.strela_in1_addr = strela_dev->reg + STRELA_IN1_ADDR_A;
	strela_dev->regs.strela_in1_size = strela_dev->reg + STRELA_IN1_SIZE_A;
	strela_dev->regs.strela_in2_addr = strela_dev->reg + STRELA_IN2_ADDR_A;
	strela_dev->regs.strela_in2_size = strela_dev->reg + STRELA_IN2_SIZE_A;
	strela_dev->regs.strela_in3_addr = strela_dev->reg + STRELA_IN3_ADDR_A;
	strela_dev->regs.strela_in3_size = strela_dev->reg + STRELA_IN3_SIZE_A;
	strela_dev->regs.strela_out0_addr = strela_dev->reg + STRELA_OUT0_ADDR_A;
	strela_dev->regs.strela_out0_size = strela_dev->reg + STRELA_OUT0_SIZE_A;
	strela_dev->regs.strela_out1_addr = strela_dev->reg + STRELA_OUT1_ADDR_A;
	strela_dev->regs.strela_out1_size = strela_dev->reg + STRELA_OUT1_SIZE_A;
	strela_dev->regs.strela_out2_addr = strela_dev->reg + STRELA_OUT2_ADDR_A;
	strela_dev->regs.strela_out2_size = strela_dev->reg + STRELA_OUT2_SIZE_A;
	strela_dev->regs.strela_out3_addr = strela_dev->reg + STRELA_OUT3_ADDR_A;
	strela_dev->regs.strela_out3_size = strela_dev->reg + STRELA_OUT3_SIZE_A;
	strela_dev->regs.strela_cntr_conf = strela_dev->reg + STRELA_CNTR_CONF_A;
	strela_dev->regs.strela_cntr_exec = strela_dev->reg + STRELA_CNTR_EXEC_A;
	strela_dev->regs.strela_cntr_stall = strela_dev->reg + STRELA_CNTR_STALL_A;
	strela_dev->regs.strela_out_arb_hold = strela_dev->reg + STRELA_OUT_ARB_HOLD_A;
	strela_dev->regs.strela_reset_dma = strela_dev->reg + STRELA_RESET_DMA_A;
	strela_dev->regs.strela_am_opa = strela_dev->reg + STRELA_AM_OPA;
	strela_dev->regs.strela_am_opb = strela_dev->reg + STRELA_AM_OPB;
	strela_dev->regs.strela_am_opr = strela_dev->reg + STRELA_AM_OPR;

	// test register remap by accessing a scratch register of STRELA, register A
	// write a dummy value and then read it
	
	u32 dummy_reg_value = 0xbeefcafe;
	u32 read_reg_value = 0;

	iowrite32(dummy_reg_value, strela_dev->regs.strela_am_opa);

	read_reg_value = ioread32(strela_dev->regs.strela_am_opa);

	if(dummy_reg_value != read_reg_value) {
		dev_err(dev, "STRELA: There was a problem with accessing the registers after they were memory mapped\n");
		goto fail;
	}

	//if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32))) {
	//	dev_err(dev, "STRELA: No suitable embedded DMA available\n");
	//	goto fail;
	//}

	// register misc device
	ret = misc_register(&strela_dev->miscdev);

	if (ret < 0) {
		dev_err(dev, "STRELA: Could not register misc device\n");
		goto fail;
	}

	dev_set_drvdata(dev, strela_dev);

	irq_conf = platform_get_irq_byname_optional(pdev, "config_done");
	if (irq_conf > 0)
	{
		dev_info(dev, "requesting config loading completed IRQ: %d\n", irq_conf);

		if (request_threaded_irq(irq_conf, strela_conf_irq_check, strela_conf_process, IRQF_ONESHOT | IRQF_SHARED, dev_name(dev), strela_dev)) 
		{
			dev_err(dev, "failure when requesting IRQ %d for config loading completed events\n", irq_conf);
			goto irq_fail;
		}
	}
	else {
		dev_err(dev, "no IRQ provided for config loading completed events\n");
		goto irq_fail;
	}

	irq_exec = platform_get_irq_byname_optional(pdev, "exec_done");
	if (irq_exec > 0)
	{
		dev_info(dev, "requesting execution completed IRQ: %d\n", irq_exec);

		if (request_threaded_irq(irq_exec, strela_exec_irq_check, strela_exec_process, IRQF_ONESHOT | IRQF_SHARED, dev_name(dev), strela_dev)) 
		{
			dev_err(dev, "failure when requesting IRQ %d for execution completed events\n", irq_exec);
			goto irq_conf_fail;
		}
	}
	else {
		dev_err(dev, "no IRQ provided for execution completed events\n");
		goto irq_conf_fail;
	}

	dev_info(dev, "Registering STRELA device\n");

	return 0;

irq_conf_fail:
	free_irq(irq_conf, strela_dev);
irq_fail:
	misc_deregister(&strela_dev->miscdev);
fail:

	if (strela_dev)
 		kfree(strela_dev);

	return ret;
};

static int strela_remove(struct platform_device *pdev)
{
	// driver not removed during the runtime of the system

	return 0;
};

static void strela_shutdown(struct platform_device *pdev)
{
	return;
};

static const struct of_device_id strela_of_match[] = {
	{ .compatible = "cei,strela", },
	{ },
};

MODULE_DEVICE_TABLE(of, strela_of_match);

static struct platform_driver strela_driver = {
	.driver	= {
		.name			= "cei,strela",
		.of_match_table		= strela_of_match,
		.suppress_bind_attrs	= true,
	},
	.probe	= strela_probe,
	.remove	= strela_remove,
	.shutdown = strela_shutdown,
};

module_driver(strela_driver, platform_driver_register, platform_driver_unregister);

MODULE_DESCRIPTION("Simple driver for the STRELA CGRA with embedded DMA module");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Milos Dordevic <milos.dordevic@upm.es>");
MODULE_ALIAS("strela");
MODULE_LICENSE("GPL v2");
