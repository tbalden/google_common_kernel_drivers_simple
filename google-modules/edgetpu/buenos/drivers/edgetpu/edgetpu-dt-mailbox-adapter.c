// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compatibility layer to find mailboxes in the device tree and allocate edgetpu_mailboxes for them.
 *
 * Copyright (C) 2025 Google LLC
 */

#include "edgetpu-dt-mailbox-adapter.h"
#include "edgetpu-mailbox.h"

/* Mailbox index for kernel control interface */
#define KERNEL_MAILBOX_INDEX 0

/* Mailbox index for in-kernel virtual inference interface, if enabled */
#define IKV_MAILBOX_INDEX 1

/* Mailbox index for Inter-IP Fence signaling mailbox, if enabled */
#define IIF_MAILBOX_INDEX 2

/*
 * Helper function to allocate and initialize a specific mailbox based on its index.
 */
static struct edgetpu_mailbox *dedicated_mailbox(struct edgetpu_mailbox_manager *mgr, uint idx)
{
	struct edgetpu_mailbox *mailbox;
	void __iomem *csr_base;
	unsigned long flags;

	csr_base = mgr->etdev->regs.mem + mgr->get_context_csr_base(idx);
	mailbox = edgetpu_mailbox_alloc(mgr, csr_base, idx);

	write_lock_irqsave(&mgr->mailboxes_lock, flags);
	mgr->mailboxes[idx] = mailbox;
	write_unlock_irqrestore(&mgr->mailboxes_lock, flags);

	return mailbox;
}

struct edgetpu_mailbox *edgetpu_mailbox_kci(struct edgetpu_mailbox_manager *mgr)
{
	return dedicated_mailbox(mgr, KERNEL_MAILBOX_INDEX);
}

struct edgetpu_mailbox *edgetpu_mailbox_ikv(struct edgetpu_mailbox_manager *mgr)
{
	return dedicated_mailbox(mgr, IKV_MAILBOX_INDEX);
}

struct edgetpu_mailbox *edgetpu_mailbox_iif(struct edgetpu_mailbox_manager *mgr)
{
	if (mgr && mgr->use_iif)
		return dedicated_mailbox(mgr, IIF_MAILBOX_INDEX);

	return NULL;
}

/* TODO(b/376971597) Remove once dedicated mailbox owners handle their own IRQs */
void edgetpu_mailbox_release_dedicated_mailbox(struct edgetpu_mailbox_manager *mgr,
					       struct edgetpu_mailbox *mailbox)
{
	unsigned long flags;

	write_lock_irqsave(&mgr->mailboxes_lock, flags);
	mgr->mailboxes[mailbox->mailbox_id] = NULL;
	write_unlock_irqrestore(&mgr->mailboxes_lock, flags);

	kfree(mailbox);
}
