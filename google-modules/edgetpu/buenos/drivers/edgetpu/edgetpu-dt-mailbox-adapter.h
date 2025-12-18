/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Compatibility layer to find mailboxes in the device tree and allocate edgetpu_mailboxes for them.
 *
 * Copyright (C) 2025 Google LLC
 */

#ifndef __EDGETPU_DT_MAILBOX_ADAPTER_H__
#define __EDGETPU_DT_MAILBOX_ADAPTER_H__

#include "edgetpu-internal.h"
#include "edgetpu-mailbox.h"

/* requests the mailbox for KCI */
struct edgetpu_mailbox *edgetpu_mailbox_kci(struct edgetpu_mailbox_manager *mgr);

/* requests the mailbox for in-kernel VII */
struct edgetpu_mailbox *edgetpu_mailbox_ikv(struct edgetpu_mailbox_manager *mgr);

/* requests the mailbox for Inter-IP Fence signaling to the TPU */
struct edgetpu_mailbox *edgetpu_mailbox_iif(struct edgetpu_mailbox_manager *mgr);

/*
 * TODO(b/376971597) Dedicated mailboxes still need to be included in manager->mailboxes until
 *                   their IRQ handlers are separated from edgetpu_mailbox_irq_handler. Once that
 *                   is done, mailboxes owners can call kfree directly instead of this function.
 */
void edgetpu_mailbox_release_dedicated_mailbox(struct edgetpu_mailbox_manager *mgr,
					       struct edgetpu_mailbox *mailbox);

#endif /* __EDGETPU_DT_MAILBOX_ADAPTER_H__ */
