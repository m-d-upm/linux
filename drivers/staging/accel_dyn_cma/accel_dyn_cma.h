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
};

#define ACCEL_DYN_CMA_IOCTL_BASE 'V'

#define ACCEL_DYN_CMA_IOCTL_ALLOC	_IOWR (ACCEL_DYN_CMA_IOCTL_BASE, 1, struct accel_dyn_cma_alloc_req_ioctl_arg)
#define ACCEL_DYN_CMA_IOCTL_FREE 	_IOW  (ACCEL_DYN_CMA_IOCTL_BASE, 2, int)

struct device*   u_dma_buf_device_search(const char* name, int id);
struct device*   u_dma_buf_device_create(const char* name, int id, size_t size, u64 option, struct device* parent);
int              u_dma_buf_device_remove(struct device *dev);
int              u_dma_buf_device_getmap(struct device *dev, size_t* size, void** virt_addr, dma_addr_t* phys_addr);
int              u_dma_buf_device_sync(struct device *dev, int command, int direction, u64 offset, ssize_t size);
struct bus_type* u_dma_buf_find_available_bus_type(char* name, int name_len);

#endif
