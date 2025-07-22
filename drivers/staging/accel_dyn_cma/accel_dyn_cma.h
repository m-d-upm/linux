/*
 * Driver that provides dynamic CMA using u-dma-buf for accelerators that require CM to perform DMA transactions
 *
 * Copyright (C) 2025 Milos Dordevic, CEI-UPM.
 */

#ifndef ACCEL_DYN_CMA_H
#define ACCEL_DYN_CMA_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct accel_dyn_cma_alloc_req_ioctl_arg {
    u32 size;
    int id;
    char *dev_name; // used so that the buffer can be bound to a specific device
};

#define ACCEL_DYN_CMA_IOCTL_BASE 'V'

#define ACCEL_DYN_CMA_IOCTL_ALLOC	_IOWR (ACCEL_DYN_CMA_IOCTL_BASE, 1, struct accel_dyn_cma_alloc_req_ioctl_arg)
#define ACCEL_DYN_CMA_IOCTL_FREE 	_IOW  (ACCEL_DYN_CMA_IOCTL_BASE, 2, int)

#endif
