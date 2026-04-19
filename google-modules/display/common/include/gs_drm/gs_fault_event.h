/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2025 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#ifndef _GS_FAULT_EVENT_H_
#define _GS_FAULT_EVENT_H_

#include <linux/device.h>

/*
 * enum gs_fault_event_type - the type of fault event
 * @GS_FAULT_EVENT_TYPE_PANEL_ERROR: A panel error
 * @GS_FAULT_EVENT_TYPE_DSI_ERROR: A DSI error
 * @GS_FAULT_EVENT_TYPE_FRAME_START_TIMEOUT: A frame start timeout
 * @GS_FAULT_EVENT_TYPE_FRAME_DONE_TIMEOUT: A frame done timeout

 */
enum gs_fault_event_type {
	GS_FAULT_EVENT_TYPE_PANEL_ERROR = 0,
	GS_FAULT_EVENT_TYPE_DSI_ERROR,
	GS_FAULT_EVENT_TYPE_FRAME_START_TIMEOUT,
	GS_FAULT_EVENT_TYPE_FRAME_DONE_TIMEOUT,
	GS_FAULT_EVENT_TYPE_MAX,
};

static inline void gs_fault_event_emit(struct device *dev, enum gs_fault_event_type type,
				       u64 value) {
	char type_str[32];
	char val_str[32];
	char **envp;

	snprintf(type_str, sizeof(type_str), "TYPE=%u", type);
	snprintf(val_str, sizeof(val_str), "VAL=%llu", value);
	envp = (char *[]){"EVENT=display_fault", type_str, val_str, NULL};

	kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, envp);
}

#endif /* _GS_FAULT_EVENT_H_ */
