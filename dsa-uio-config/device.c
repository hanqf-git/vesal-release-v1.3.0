// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights reserved. */
#include <string.h>
#include <search.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "common.h"
#include "dsa.h"
#include "device.h"
#include "user_device.h"

static int dd;

void
wq_info_get(void *wq, struct wq_info *wq_info)
{
	switch (dd) {

	case USER:
		ud_wq_info_get(wq, wq_info);
		break;

	default:
		ERR("Unknown wq type %d\n", dd);
	}
}

int
iommu_disabled(void)
{
	return dd == USER && ud_iommu_disabled();
}

int
driver_init(struct tcfg *tcfg)
{
	int rc;

	rc = 0;
	dd = tcfg->driver;
	if (dd == USER)
		rc = user_driver_init(tcfg);

	return rc;
}

uint64_t rte_mem_virt2iova(void *p)
{
	return dd == IDXD ? (uint64_t)p : user_virt2iova(p);
}
