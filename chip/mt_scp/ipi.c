/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Inter-Processor Communication (IPC) and Inter-Processor Interrupt (IPI)
 *
 * IPC is a communication bridge between AP and SCP.  AP/SCP sends an IPC
 * interrupt to SCP/AP to inform to collect the commmunication mesesages in the
 * shared buffer.
 *
 * There are 4 IPCs in the current architecture, from IPC0 to IPC3.  The
 * priority of IPC is proportional to its IPC index. IPC3 has the highest
 * priority and IPC0 has the lowest one.
 *
 * IPC0 may contain zero or more IPIs.  Each IPI represents a task or a service,
 * e.g. host command, or video encoding.  IPIs are recognized by IPI ID, which
 * should sync across AP and SCP.  Shared buffer should designated which IPI
 * ID it talks to.
 *
 * Currently, we don't have IPC handlers for IPC1, IPC2, and IPC3.
 */

#include "console.h"
#include "hooks.h"
#include "host_command.h"
#include "ipi_chip.h"
#include "system.h"
#include "task.h"
#include "util.h"

#define CPRINTF(format, args...) cprintf(CC_IPI, format, ##args)
#define CPRINTS(format, args...) cprints(CC_IPI, format, ##args)

#define IPI_MAX_REQUEST_SIZE CONFIG_IPC_SHARED_OBJ_BUF_SIZE
#define IPI_MAX_RESPONSE_SIZE CONFIG_IPC_SHARED_OBJ_BUF_SIZE

static struct mutex ipi_lock;
/* IPC0 shared objects, including send object and receive object. */
static struct ipc_shared_obj *const scp_send_obj =
	(struct ipc_shared_obj *)CONFIG_IPC_SHARED_OBJ_ADDR;
static struct ipc_shared_obj *const scp_recv_obj =
	(struct ipc_shared_obj *)(CONFIG_IPC_SHARED_OBJ_ADDR +
				  sizeof(struct ipc_shared_obj));

/* Check if SCP to AP IPI is in use. */
static inline int is_ipi_busy(void)
{
	return SCP_HOST_INT & IPC_SCP2HOST_BIT;
}

/* If IPI is declared as a wake-up source, wake AP up. */
static inline void try_to_wakeup_ap(int32_t id)
{
	if (*ipi_wakeup_table[id])
		SCP_SPM_INT = SPM_INT_A2SPM;
}

/* Send data from SCP to AP. */
int ipi_send(int32_t id, void *buf, uint32_t len, int wait)
{
	/*
	 * TODO(b:117917141): Evaluate if we can remove this once we have the
	 * video/camera feature code base.
	 */
	if (wait && in_interrupt_context())
		/* Prevent from infinity wait when be in ISR context. */
		return EC_ERROR_BUSY;

	if (len > sizeof(scp_send_obj->buffer))
		return EC_ERROR_INVAL;

	task_disable_irq(SCP_IRQ_IPC0);
	mutex_lock(&ipi_lock);

	/* Check if there is already an IPI pending in AP. */
	if (is_ipi_busy()) {
		/*
		 * If the following conditions meet,
		 *   1) There is an IPI pending in AP.
		 *   2) The incoming IPI is a wakeup IPI.
		 * then it assumes that AP is in suspend state.
		 * Send a AP wakeup request to SPM.
		 *
		 * The incoming IPI will be checked if it's a wakeup source.
		 */
		try_to_wakeup_ap(id);

		mutex_unlock(&ipi_lock);
		task_enable_irq(SCP_IRQ_IPC0);

		return EC_ERROR_BUSY;
	}


	scp_send_obj->id = id;
	scp_send_obj->len = len;
	memcpy(scp_send_obj->buffer, buf, len);

	/* Send IPI to AP: interrutp AP to receive IPI messages. */
	try_to_wakeup_ap(id);
	SCP_HOST_INT = IPC_SCP2HOST_BIT;

	while (wait && is_ipi_busy())
		;

	mutex_unlock(&ipi_lock);
	task_enable_irq(SCP_IRQ_IPC0);

	return EC_SUCCESS;
}

static void ipi_handler(void)
{
	if (scp_recv_obj->id >= IPI_COUNT) {
		CPRINTS("#ERR IPI %d", scp_recv_obj->id);
		return;
	}

	/*
	 * Pass the buffer to handler. Each handler should be in charge of
	 * the buffer copying/reading before returning from handler.
	 */
	ipi_handler_table[scp_recv_obj->id](
		scp_recv_obj->id, scp_recv_obj->buffer, scp_recv_obj->len);
}

void ipi_inform_ap(void)
{
	struct scp_run_t scp_run;
	int ret;

	scp_run.signaled = 1;
	strncpy(scp_run.fw_ver, system_get_version(SYSTEM_IMAGE_RW),
		SCP_FW_VERSION_LEN);
	scp_run.dec_capability = 0;
	scp_run.enc_capability = 0;

	ret = ipi_send(IPI_SCP_INIT, (void *)&scp_run, sizeof(scp_run), 1);

	if (ret)
		ccprintf("Failed to send initialization IPC messages.\n");
}

static void ipi_enable_ipc0_deferred(void)
{
	/* Clear IPC0 IRQs. */
	SCP_GIPC_IN = SCP_GPIC_IN_CLEAR_ALL;

	/* All tasks are up, we can safely enable IPC0 IRQ now. */
	SCP_INTC_IRQ_ENABLE |= IPC0_IRQ_EN;
	task_enable_irq(SCP_IRQ_IPC0);

	/* Inform AP that SCP is inited.  */
	ipi_inform_ap();

	CPRINTS("ipi init");
}
DECLARE_DEFERRED(ipi_enable_ipc0_deferred);

/* Initialize IPI. */
static void ipi_init(void)
{
	/* Clear send share buffer. */
	memset(scp_send_obj, 0, sizeof(struct ipc_shared_obj));

	/* Enable IRQ after all tasks are up.  */
	hook_call_deferred(&ipi_enable_ipc0_deferred_data, 0);
}
DECLARE_HOOK(HOOK_INIT, ipi_init, HOOK_PRIO_DEFAULT);

void ipc_handler(void)
{
	/* TODO(b/117917141): We only support IPC_ID(0) for now. */
	if (SCP_GIPC_IN & SCP_GIPC_IN_CLEAR_IPCN(0))
		ipi_handler();

	SCP_GIPC_IN = SCP_GPIC_IN_CLEAR_ALL;
}
DECLARE_IRQ(SCP_IRQ_IPC0, ipc_handler, 4);
