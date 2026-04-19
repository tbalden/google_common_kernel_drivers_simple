/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM_SMMU_V3_TELEMETRY_CALLBACKS_H
#define __ARM_SMMU_V3_TELEMETRY_CALLBACKS_H

#include <linux/types.h>

struct arm_smmu_v3_telemetry_cb {
	bool (*is_enabled)(void);
	void (*map)(void *cookie, size_t size, unsigned int count);
	void (*unmap)(void *cookie, size_t size, unsigned int count);
	void (*s1_pages_tel)(int count);
	void (*atomic_pages_tel)(int count);
};

#endif /* __ARM_SMMU_V3_TELEMETRY_CALLBACKS_H */
