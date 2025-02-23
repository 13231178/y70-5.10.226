// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2019, Microsoft Corporation.
 *
 * Author:
 *   Iouri Tarassov <iourit@linux.microsoft.com>
 *
 * Dxgkrnl Graphics Driver
 * VM bus interface implementation
 *
 */

#include <linux/kernel.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/eventfd.h>
#include <linux/hyperv.h>
#include <linux/mman.h>
#include <linux/delay.h>
#include <linux/pagemap.h>
#include "dxgkrnl.h"
#include "dxgvmbus.h"

#undef pr_fmt
#define pr_fmt(fmt)	"dxgk:err: " fmt

static int init_message(struct dxgvmbusmsg *msg, struct dxgadapter *adapter,
			struct dxgprocess *process, u32 size)
{
	bool use_ext_header = dxgglobal->vmbus_ver >=
			      DXGK_VMBUS_INTERFACE_VERSION;

	if (use_ext_header)
		size += sizeof(struct dxgvmb_ext_header);
	msg->size = size;
	if (size <= VMBUSMESSAGEONSTACK) {
		msg->hdr = (void *)msg->msg_on_stack;
		memset(msg->hdr, 0, size);
	} else {
		msg->hdr = kzalloc(size, GFP_ATOMIC);
		if (msg->hdr == NULL)
			return -ENOMEM;
	}
	if (use_ext_header) {
		msg->msg = (char *)&msg->hdr[1];
		msg->hdr->command_offset = sizeof(msg->hdr[0]);
		if (adapter)
			msg->hdr->vgpu_luid = adapter->host_vgpu_luid;
	} else {
		msg->msg = (char *)msg->hdr;
	}
	if (adapter && !dxgglobal->async_msg_enabled)
		msg->channel = &adapter->channel;
	else
		msg->channel = &dxgglobal->channel;
	return 0;
}

static int init_message_res(struct dxgvmbusmsgres *msg,
			    struct dxgadapter *adapter,
			    struct dxgprocess *process,
			    u32 size,
			    u32 result_size)
{
	bool use_ext_header = dxgglobal->vmbus_ver >=
			      DXGK_VMBUS_INTERFACE_VERSION;

	if (use_ext_header)
		size += sizeof(struct dxgvmb_ext_header);
	msg->size = size;
	msg->res_size += (result_size + 7) & ~7;
	size += msg->res_size;
	msg->hdr = kzalloc(size, GFP_ATOMIC);
	if (msg->hdr == NULL) {
		pr_err("Failed to allocate VM bus message: %d", size);
		return -ENOMEM;
	}
	if (use_ext_header) {
		msg->msg = (char *)&msg->hdr[1];
		msg->hdr->command_offset = sizeof(msg->hdr[0]);
		msg->hdr->vgpu_luid = adapter->host_vgpu_luid;
	} else {
		msg->msg = (char *)msg->hdr;
	}
	msg->res = (char *)msg->hdr + msg->size;
	if (dxgglobal->async_msg_enabled)
		msg->channel = &dxgglobal->channel;
	else
		msg->channel = &adapter->channel;
	return 0;
}

static void free_message(struct dxgvmbusmsg *msg, struct dxgprocess *process)
{
	if (msg->hdr && (char *)msg->hdr != msg->msg_on_stack)
		kfree(msg->hdr);
}

int ntstatus2int(struct ntstatus status)
{
	if (NT_SUCCESS(status))
		return (int)status.v;
	switch (status.v) {
	case STATUS_OBJECT_NAME_COLLISION:
		return -EEXIST;
	case STATUS_NO_MEMORY:
		return -ENOMEM;
	case STATUS_INVALID_PARAMETER:
		return -EINVAL;
	case STATUS_OBJECT_NAME_INVALID:
	case STATUS_OBJECT_NAME_NOT_FOUND:
		return -ENOENT;
	case STATUS_TIMEOUT:
		return -EAGAIN;
	case STATUS_BUFFER_TOO_SMALL:
		return -EOVERFLOW;
	case STATUS_DEVICE_REMOVED:
		return -ENODEV;
	case STATUS_ACCESS_DENIED:
		return -EACCES;
	case STATUS_NOT_SUPPORTED:
		return -EPERM;
	case STATUS_ILLEGAL_INSTRUCTION:
		return -EOPNOTSUPP;
	case STATUS_INVALID_HANDLE:
		return -EBADF;
	case STATUS_GRAPHICS_ALLOCATION_BUSY:
		return -EINPROGRESS;
	case STATUS_OBJECT_TYPE_MISMATCH:
		return -EPROTOTYPE;
	case STATUS_NOT_IMPLEMENTED:
		return -EPERM;
	default:
		return -EINVAL;
	}
}

/*
 * Helper functions
 */

static void command_vm_to_host_init2(struct dxgkvmb_command_vm_to_host *command,
				     enum dxgkvmb_commandtype_global t,
				     struct d3dkmthandle process)
{
	command->command_type	= t;
	command->process	= process;
	command->command_id	= 0;
	command->channel_type	= DXGKVMB_VM_TO_HOST;
}

static void command_vgpu_to_host_init1(struct dxgkvmb_command_vgpu_to_host
					*command,
					enum dxgkvmb_commandtype type)
{
	command->command_type	= type;
	command->process.v	= 0;
	command->command_id	= 0;
	command->channel_type	= DXGKVMB_VGPU_TO_HOST;
}

static void command_vgpu_to_host_init2(struct dxgkvmb_command_vgpu_to_host
					*command,
					enum dxgkvmb_commandtype type,
					struct d3dkmthandle process)
{
	command->command_type	= type;
	command->process	= process;
	command->command_id	= 0;
	command->channel_type	= DXGKVMB_VGPU_TO_HOST;
}

static void command_vm_to_host_init1(struct dxgkvmb_command_vm_to_host *command,
				     enum dxgkvmb_commandtype_global type)
{
	command->command_type = type;
	command->process.v = 0;
	command->command_id = 0;
	command->channel_type = DXGKVMB_VM_TO_HOST;
}

static int
dxgvmb_send_sync_msg_ntstatus(struct dxgvmbuschannel *channel,
			      void *command, u32 cmd_size)
{
	struct ntstatus *status;
	int ret;

	status = kzalloc(sizeof(struct ntstatus), GFP_ATOMIC);

	ret = dxgvmb_send_sync_msg(channel, command, cmd_size,
				   status, sizeof(struct ntstatus));
	if (ret >= 0)
		ret = ntstatus2int(*status);

	kfree(status);
	return ret;
}

static int check_iospace_address(unsigned long address, u32 size)
{
	if (address < dxgglobal->mmiospace_base ||
	    size > dxgglobal->mmiospace_size ||
	    address >= (dxgglobal->mmiospace_base +
			dxgglobal->mmiospace_size - size)) {
		pr_err("invalid iospace address %lx", address);
		return -EINVAL;
	}
	return 0;
}

int dxg_unmap_iospace(void *va, u32 size)
{
	unsigned long page_addr;
	int ret = 0;

	page_addr = ((unsigned long)va) & PAGE_MASK;

	dev_dbg(dxgglobaldev, "%s %p %x %x", __func__, va, page_addr, size);

	/*
	 * When an app calls exit(), dxgkrnl is called to close the device
	 * with current->mm equal to NULL.
	 */
	if (current->mm) {
		ret = vm_munmap(page_addr, size);
		if (ret) {
			pr_err("vm_munmap failed %d", ret);
			return -ENOTRECOVERABLE;
		}
	}
	return 0;
}

static u8 *dxg_map_iospace(u64 iospace_address, u32 size,
			   unsigned long protection, bool cached)
{
	struct vm_area_struct *vma;
	unsigned long va;
	int ret = 0;

	dev_dbg(dxgglobaldev, "%s: %llx %x %lx",
		    __func__, iospace_address, size, protection);
	if (check_iospace_address(iospace_address, size) < 0) {
		pr_err("%s: invalid address", __func__);
		return NULL;
	}

	va = vm_mmap(NULL, 0, size, protection, MAP_SHARED | MAP_ANONYMOUS, 0);
	if ((long)va <= 0) {
		pr_err("vm_mmap failed %lx %d", va, size);
		return NULL;
	}

	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, (unsigned long)va);
	if (vma) {
		pgprot_t prot = vma->vm_page_prot;

		if (!cached)
			prot = pgprot_writecombine(prot);
		dev_dbg(dxgglobaldev, "vma: %lx %lx %lx",
			    vma->vm_start, vma->vm_end, va);
		vma->vm_pgoff = iospace_address >> PAGE_SHIFT;
		ret = io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
					 size, prot);
		if (ret)
			pr_err("io_remap_pfn_range failed: %d", ret);
	} else {
		pr_err("failed to find vma: %p %lx", vma, va);
		ret = -ENOMEM;
	}
	mmap_read_unlock(current->mm);

	if (ret) {
		dxg_unmap_iospace((void *)va, size);
		return NULL;
	}
	dev_dbg(dxgglobaldev, "%s end: %lx", __func__, va);
	return (u8 *) (va + iospace_address % PAGE_SIZE);
}

/*
 * Global messages to the host
 */

int dxgvmb_send_set_iospace_region(u64 start, u64 len, u32 shared_mem_gpadl)
{
	int ret;
	struct dxgkvmb_command_setiospaceregion *command;
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, NULL, NULL, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;

	command_vm_to_host_init1(&command->hdr,
				 DXGK_VMBCOMMAND_SETIOSPACEREGION);
	command->start = start;
	command->length = len;
	command->shared_page_gpadl = shared_mem_gpadl;
	ret = dxgvmb_send_sync_msg_ntstatus(&dxgglobal->channel, msg.hdr,
					    msg.size);
	if (ret < 0)
		pr_err("send_set_iospace_region failed %x", ret);

	dxgglobal_release_channel_lock();
cleanup:
	free_message(&msg, NULL);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_create_process(struct dxgprocess *process)
{
	int ret;
	struct dxgkvmb_command_createprocess *command;
	struct dxgkvmb_command_createprocess_return result = { 0 };
	struct dxgvmbusmsg msg;
	char s[WIN_MAX_PATH];
	int i;

	ret = init_message(&msg, NULL, process, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;

	command_vm_to_host_init1(&command->hdr, DXGK_VMBCOMMAND_CREATEPROCESS);
	command->process = process;
	command->process_id = process->pid;
	command->linux_process = 1;
	s[0] = 0;
	__get_task_comm(s, WIN_MAX_PATH, current);
	for (i = 0; i < WIN_MAX_PATH; i++) {
		command->process_name[i] = s[i];
		if (s[i] == 0)
			break;
	}

	ret = dxgvmb_send_sync_msg(&dxgglobal->channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0) {
		pr_err("create_process failed %d", ret);
	} else if (result.hprocess.v == 0) {
		pr_err("create_process returned 0 handle");
		ret = -ENOTRECOVERABLE;
	} else {
		process->host_handle = result.hprocess;
		dev_dbg(dxgglobaldev, "create_process returned %x",
			    process->host_handle.v);
	}

	dxgglobal_release_channel_lock();

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_destroy_process(struct d3dkmthandle process)
{
	int ret;
	struct dxgkvmb_command_destroyprocess *command;
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, NULL, NULL, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;
	command_vm_to_host_init2(&command->hdr, DXGK_VMBCOMMAND_DESTROYPROCESS,
				 process);
	ret = dxgvmb_send_sync_msg_ntstatus(&dxgglobal->channel,
					    msg.hdr, msg.size);
	dxgglobal_release_channel_lock();

cleanup:
	free_message(&msg, NULL);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_open_sync_object_nt(struct dxgprocess *process,
				    struct dxgvmbuschannel *channel,
				    struct d3dkmt_opensyncobjectfromnthandle2
				    *args,
				    struct dxgsyncobject *syncobj)
{
	struct dxgkvmb_command_opensyncobject *command;
	struct dxgkvmb_command_opensyncobject_return result = { };
	int ret;
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, NULL, process, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	command_vm_to_host_init2(&command->hdr, DXGK_VMBCOMMAND_OPENSYNCOBJECT,
				 process->host_handle);
	command->device = args->device;
	command->global_sync_object = syncobj->shared_owner->host_shared_handle_nt;
	command->flags = args->flags;
	if (syncobj->monitored_fence)
		command->engine_affinity =
			args->monitored_fence.engine_affinity;

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;

	ret = dxgvmb_send_sync_msg(channel, msg.hdr, msg.size,
				   &result, sizeof(result));

	dxgglobal_release_channel_lock();

	if (ret < 0)
		goto cleanup;

	ret = ntstatus2int(result.status);
	if (ret < 0)
		goto cleanup;

	args->sync_object = result.sync_object;
	if (syncobj->monitored_fence) {
		void *va = dxg_map_iospace(result.guest_cpu_physical_address,
					   PAGE_SIZE, PROT_READ | PROT_WRITE,
					   true);
		if (va == NULL) {
			ret = -ENOMEM;
			goto cleanup;
		}
		args->monitored_fence.fence_value_cpu_va = va;
		args->monitored_fence.fence_value_gpu_va =
		    result.gpu_virtual_address;
		syncobj->mapped_address = va;
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_create_nt_shared_object(struct dxgprocess *process,
					struct d3dkmthandle object,
					struct d3dkmthandle *shared_handle)
{
	struct dxgkvmb_command_createntsharedobject *command;
	int ret;
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, NULL, process, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	command_vm_to_host_init2(&command->hdr,
				 DXGK_VMBCOMMAND_CREATENTSHAREDOBJECT,
				 process->host_handle);
	command->object = object;

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;

	ret = dxgvmb_send_sync_msg(dxgglobal_get_dxgvmbuschannel(),
				   msg.hdr, msg.size, shared_handle,
				   sizeof(*shared_handle));

	dxgglobal_release_channel_lock();

	if (ret < 0)
		goto cleanup;
	if (shared_handle->v == 0) {
		pr_err("failed to create NT shared object");
		ret = -ENOTRECOVERABLE;
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_destroy_nt_shared_object(struct d3dkmthandle shared_handle)
{
	struct dxgkvmb_command_destroyntsharedobject *command;
	int ret;
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, NULL, NULL, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	command_vm_to_host_init1(&command->hdr,
				 DXGK_VMBCOMMAND_DESTROYNTSHAREDOBJECT);
	command->shared_handle = shared_handle;

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;

	ret = dxgvmb_send_sync_msg_ntstatus(dxgglobal_get_dxgvmbuschannel(),
					    msg.hdr, msg.size);

	dxgglobal_release_channel_lock();

cleanup:
	free_message(&msg, NULL);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_destroy_sync_object(struct dxgprocess *process,
				    struct d3dkmthandle sync_object)
{
	struct dxgkvmb_command_destroysyncobject *command;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, NULL, process, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;

	command_vm_to_host_init2(&command->hdr,
				 DXGK_VMBCOMMAND_DESTROYSYNCOBJECT,
				 process->host_handle);
	command->sync_object = sync_object;

	ret = dxgvmb_send_sync_msg_ntstatus(dxgglobal_get_dxgvmbuschannel(),
					    msg.hdr, msg.size);

	dxgglobal_release_channel_lock();

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_share_object_with_host(struct dxgprocess *process,
				struct d3dkmt_shareobjectwithhost *args)
{
	struct dxgkvmb_command_shareobjectwithhost *command;
	struct dxgkvmb_command_shareobjectwithhost_return result = {};
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, NULL, process, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;

	command_vm_to_host_init2(&command->hdr,
				 DXGK_VMBCOMMAND_SHAREOBJECTWITHHOST,
				 process->host_handle);
	command->device_handle = args->device_handle;
	command->object_handle = args->object_handle;

	ret = dxgvmb_send_sync_msg(dxgglobal_get_dxgvmbuschannel(),
				   msg.hdr, msg.size, &result, sizeof(result));

	dxgglobal_release_channel_lock();

	if (ret || !NT_SUCCESS(result.status)) {
		if (ret == 0)
			ret = ntstatus2int(result.status);
		pr_err("DXGK_VMBCOMMAND_SHAREOBJECTWITHHOST failed: %d %x",
			ret, result.status.v);
		goto cleanup;
	}
	args->object_vail_nt_handle = result.vail_nt_handle;

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_present_virtual(struct dxgprocess *process,
				struct d3dkmt_presentvirtual *args,
				__u64 acquire_semaphore_nthandle,
				__u64 release_semaphore_nthandle,
				__u64 composition_memory_nthandle)
{
	struct dxgkvmb_command_presentvirtual *command;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};
	u32 cmd_size = 0;

	cmd_size = sizeof(struct dxgkvmb_command_presentvirtual) + args->private_data_size;

	ret = init_message(&msg, NULL, process, cmd_size);
	if (ret)
		return ret;
	command = (void *)msg.msg;

	command_vm_to_host_init2(&command->hdr,
				 DXGK_VMBCOMMAND_PRESENTVIRTUAL,
				 process->host_handle);
	command->acquire_semaphore_nthandle = acquire_semaphore_nthandle;
	command->release_semaphore_nthandle = release_semaphore_nthandle;
	command->composition_memory_nthandle = composition_memory_nthandle;
	command->private_data_size = args->private_data_size;


	if (args->private_data_size) {
		ret = copy_from_user(&command[1],
				     args->private_data,
				     args->private_data_size);
		if (ret) {
			pr_err("%s failed to copy user data", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;

	ret = dxgvmb_send_sync_msg_ntstatus(dxgglobal_get_dxgvmbuschannel(), msg.hdr,
						    msg.size);

	dxgglobal_release_channel_lock();

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

/*
 * Virtual GPU messages to the host
 */

int dxgvmb_send_open_adapter(struct dxgadapter *adapter)
{
	int ret;
	struct dxgkvmb_command_openadapter *command;
	struct dxgkvmb_command_openadapter_return result = { };
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, adapter, NULL, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	command_vgpu_to_host_init1(&command->hdr, DXGK_VMBCOMMAND_OPENADAPTER);
	command->vmbus_interface_version = dxgglobal->vmbus_ver;
	command->vmbus_last_compatible_interface_version =
	    DXGK_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0)
		goto cleanup;

	ret = ntstatus2int(result.status);
	adapter->host_handle = result.host_adapter_handle;

cleanup:
	free_message(&msg, NULL);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_close_adapter(struct dxgadapter *adapter)
{
	int ret;
	struct dxgkvmb_command_closeadapter *command;
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, adapter, NULL, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	command_vgpu_to_host_init1(&command->hdr, DXGK_VMBCOMMAND_CLOSEADAPTER);
	command->host_handle = adapter->host_handle;

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
	free_message(&msg, NULL);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_get_internal_adapter_info(struct dxgadapter *adapter)
{
	int ret;
	struct dxgkvmb_command_getinternaladapterinfo *command;
	struct dxgkvmb_command_getinternaladapterinfo_return result = { };
	struct dxgvmbusmsg msg;
	u32 result_size = sizeof(result);

	ret = init_message(&msg, adapter, NULL, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	command_vgpu_to_host_init1(&command->hdr,
				   DXGK_VMBCOMMAND_GETINTERNALADAPTERINFO);
	if (dxgglobal->vmbus_ver < DXGK_VMBUS_INTERFACE_VERSION)
		result_size -= sizeof(struct winluid);

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, result_size);
	if (ret >= 0) {
		adapter->host_adapter_luid = result.host_adapter_luid;
		adapter->host_vgpu_luid = result.host_vgpu_luid;
		wcsncpy(adapter->device_description, result.device_description,
			sizeof(adapter->device_description) / sizeof(u16));
		wcsncpy(adapter->device_instance_id, result.device_instance_id,
			sizeof(adapter->device_instance_id) / sizeof(u16));
		dxgglobal->async_msg_enabled = result.async_msg_enabled != 0;
	}
	free_message(&msg, NULL);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

struct d3dkmthandle dxgvmb_send_create_device(struct dxgadapter *adapter,
					struct dxgprocess *process,
					struct d3dkmt_createdevice *args)
{
	int ret;
	struct dxgkvmb_command_createdevice *command;
	struct dxgkvmb_command_createdevice_return result = { };
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr, DXGK_VMBCOMMAND_CREATEDEVICE,
				   process->host_handle);
	command->flags = args->flags;
	command->error_code = &dxgglobal->device_state_counter;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0)
		result.device.v = 0;
	free_message(&msg, process);
cleanup:
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return result.device;
}

int dxgvmb_send_destroy_device(struct dxgadapter *adapter,
			       struct dxgprocess *process,
			       struct d3dkmthandle h)
{
	int ret;
	struct dxgkvmb_command_destroydevice *command;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr, DXGK_VMBCOMMAND_DESTROYDEVICE,
				   process->host_handle);
	command->device = h;

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_flush_device(struct dxgdevice *device,
			     enum dxgdevice_flushschedulerreason reason)
{
	int ret;
	struct dxgkvmb_command_flushdevice *command;
	struct dxgvmbusmsg msg = {.hdr = NULL};
	struct dxgprocess *process = device->process;

	ret = init_message(&msg, device->adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr, DXGK_VMBCOMMAND_FLUSHDEVICE,
				   process->host_handle);
	command->device = device->handle;
	command->reason = reason;

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

struct d3dkmthandle
dxgvmb_send_create_context(struct dxgadapter *adapter,
			   struct dxgprocess *process,
			   struct d3dkmt_createcontextvirtual *args)
{
	struct dxgkvmb_command_createcontextvirtual *command = NULL;
	u32 cmd_size;
	int ret;
	struct d3dkmthandle context = {};
	struct dxgvmbusmsg msg = {.hdr = NULL};

	if (args->priv_drv_data_size > DXG_MAX_VM_BUS_PACKET_SIZE) {
		pr_err("PrivateDriverDataSize is invalid");
		ret = -EINVAL;
		goto cleanup;
	}
	cmd_size = sizeof(struct dxgkvmb_command_createcontextvirtual) +
	    args->priv_drv_data_size - 1;

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_CREATECONTEXTVIRTUAL,
				   process->host_handle);
	command->device = args->device;
	command->node_ordinal = args->node_ordinal;
	command->engine_affinity = args->engine_affinity;
	command->flags = args->flags;
	command->client_hint = args->client_hint;
	command->priv_drv_data_size = args->priv_drv_data_size;
	if (args->priv_drv_data_size) {
		ret = copy_from_user(command->priv_drv_data,
				     args->priv_drv_data,
				     args->priv_drv_data_size);
		if (ret) {
			pr_err("%s Faled to copy private data",
				__func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}
	/* Input command is returned back as output */
	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   command, cmd_size);
	if (ret < 0) {
		goto cleanup;
	} else {
		context = command->context;
		if (args->priv_drv_data_size) {
			ret = copy_to_user(args->priv_drv_data,
					   command->priv_drv_data,
					   args->priv_drv_data_size);
			if (ret) {
				pr_err("%s Faled to copy private data to user",
					__func__);
				ret = -EINVAL;
				dxgvmb_send_destroy_context(adapter, process,
							    context);
				context.v = 0;
			}
		}
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return context;
}

int dxgvmb_send_destroy_context(struct dxgadapter *adapter,
				struct dxgprocess *process,
				struct d3dkmthandle h)
{
	int ret;
	struct dxgkvmb_command_destroycontext *command;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_DESTROYCONTEXT,
				   process->host_handle);
	command->context = h;

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_create_paging_queue(struct dxgprocess *process,
				    struct dxgdevice *device,
				    struct d3dkmt_createpagingqueue *args,
				    struct dxgpagingqueue *pqueue)
{
	struct dxgkvmb_command_createpagingqueue_return result;
	struct dxgkvmb_command_createpagingqueue *command;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, device->adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_CREATEPAGINGQUEUE,
				   process->host_handle);
	command->args = *args;
	args->paging_queue.v = 0;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size, &result,
				   sizeof(result));
	if (ret < 0) {
		pr_err("send_create_paging_queue failed %x", ret);
		goto cleanup;
	}

	args->paging_queue = result.paging_queue;
	args->sync_object = result.sync_object;
	args->fence_cpu_virtual_address =
	    dxg_map_iospace(result.fence_storage_physical_address, PAGE_SIZE,
			    PROT_READ | PROT_WRITE, true);
	if (args->fence_cpu_virtual_address == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}
	pqueue->mapped_address = args->fence_cpu_virtual_address;
	pqueue->handle = args->paging_queue;

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_destroy_paging_queue(struct dxgprocess *process,
				     struct dxgadapter *adapter,
				     struct d3dkmthandle h)
{
	int ret;
	struct dxgkvmb_command_destroypagingqueue *command;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_DESTROYPAGINGQUEUE,
				   process->host_handle);
	command->paging_queue = h;

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

static int
copy_private_data(struct d3dkmt_createallocation *args,
		  struct dxgkvmb_command_createallocation *command,
		  struct d3dddi_allocationinfo *input_alloc_info,
		  struct d3dkmt_createstandardallocation *standard_alloc)
{
	struct dxgkvmb_command_createallocation_allocinfo *alloc_info;
	struct d3dddi_allocationinfo *input_alloc;
	int ret = 0;
	int i;
	u8 *private_data_dest = (u8 *) &command[1] +
	    (args->alloc_count *
	     sizeof(struct dxgkvmb_command_createallocation_allocinfo));

	if (args->private_runtime_data_size) {
		ret = copy_from_user(private_data_dest,
				     args->private_runtime_data,
				     args->private_runtime_data_size);
		if (ret) {
			pr_err("%s failed to copy runtime data", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
		private_data_dest += args->private_runtime_data_size;
	}

	if (args->flags.standard_allocation) {
		dev_dbg(dxgglobaldev, "private data offset %d",
			(u32) (private_data_dest - (u8 *) command));

		args->priv_drv_data_size = sizeof(*args->standard_allocation);
		memcpy(private_data_dest, standard_alloc,
		       sizeof(*standard_alloc));
		private_data_dest += args->priv_drv_data_size;
	} else if (args->priv_drv_data_size) {
		ret = copy_from_user(private_data_dest,
				     args->priv_drv_data,
				     args->priv_drv_data_size);
		if (ret) {
			pr_err("%s failed to copy private data", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
		private_data_dest += args->priv_drv_data_size;
	}

	alloc_info = (void *)&command[1];
	input_alloc = input_alloc_info;
	if (input_alloc_info[0].sysmem)
		command->flags.existing_sysmem = 1;
	for (i = 0; i < args->alloc_count; i++) {
		alloc_info->flags = input_alloc->flags.value;
		alloc_info->vidpn_source_id = input_alloc->vidpn_source_id;
		alloc_info->priv_drv_data_size =
		    input_alloc->priv_drv_data_size;
		/* Init to zero in case it is not sysmem. */
		alloc_info->sysmem_pages_rle_size = 0;
		if (input_alloc->priv_drv_data_size) {
			ret = copy_from_user(private_data_dest,
					     input_alloc->priv_drv_data,
					     input_alloc->priv_drv_data_size);
			if (ret) {
				pr_err("%s failed to copy alloc data",
					__func__);
				ret = -EINVAL;
				goto cleanup;
			}
			private_data_dest += input_alloc->priv_drv_data_size;
		}
		alloc_info++;
		input_alloc++;
	}

cleanup:
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

static u64 calculate_max_rle_data_size(
	struct dxgkvmb_command_getallocationsize_return *allocation_size_result)
{
	/* Allocation sizes obtained from GetAllocationSize. */
	u64 *allocation_sizes = (u64 *)&allocation_size_result[1];
	u64 num_pages = 0;
	int i;

	for (i = 0; i < allocation_size_result->alloc_count; i++)
		num_pages += (allocation_sizes[i] + PAGE_SIZE - 1) / PAGE_SIZE;

	return num_pages * sizeof(u64);
}

static int
copy_sysmem_pages_rle_data(struct d3dkmt_createallocation *args,
		  struct dxgkvmb_command_createallocation *command,
		  struct d3dddi_allocationinfo *input_alloc_info,
		  struct dxgkvmb_command_getallocationsize_return *allocation_size_result,
		  struct dxgallocation **dxgalloc,
		  u32 sysmem_pages_rle_limit,
		  u32 sysmem_pages_rle_start_offset)
{
	struct dxgkvmb_command_createallocation_allocinfo *alloc_info;
	struct d3dddi_allocationinfo *input_alloc;
	int ret = 0;
	int ret1 = 0;
	int i;
	/* Output location (after all private data). */
	u64 *sysmem_pages_rle_dest = (u64 *)((u8 *) &command[1] +
	    args->alloc_count *
		sizeof(struct dxgkvmb_command_createallocation_allocinfo) +
		sysmem_pages_rle_start_offset);
	/* Allocation sizes obtained from GetAllocationSize. */
	u64 *allocation_sizes = (u64 *) &allocation_size_result[1];
	/* Counts how many RLE entries we've used total (command space limitations). */
	u32 sysmem_pages_rle_used = 0;
	/* Size of current sysmem region we want to encode. */
	u64 curr_alloc_size = 0;
	/* Number of pages in sysmem region. */
	u32 npages = 0;
	/* Pages we've encoded so far. */
	u32 pages_processed = 0;
	/* Current page being encoded. */
	u64 curr_page = 0;
	/* Starting page of the RLE region. */
	u64 base_page = 0;
	/* Number of pages in the RLE region. */
	u32 pages_seen = 0;
	/* Page iterator. */
	struct page **page_in;

	alloc_info = (void *)&command[1];
	input_alloc = input_alloc_info;
	for (i = 0; i < args->alloc_count; i++) {
		/*
		 * Construct RLE encoded sysmem pages.
		 * See host-side driver usage for more in-depth comments.
		 */
		alloc_info->sysmem_pages_rle_size = 0;

		/* Grab next size and increment allocation_sizes forward. */
		curr_alloc_size = *allocation_sizes++;
		/* Ignore empty sizes. */
		if (input_alloc->priv_drv_data_size && curr_alloc_size > 0) {
			npages = curr_alloc_size >> PAGE_SHIFT;
			dxgalloc[i]->cpu_address = (void *)input_alloc->sysmem;

			/* Grab the pages from user. */
			dxgalloc[i]->pages = vzalloc(npages * sizeof(void *));
			if (dxgalloc[i]->pages == NULL) {
				ret = -ENOMEM;
				goto cleanup;
			}
			ret1 = get_user_pages_fast((unsigned long)input_alloc->sysmem, npages,
				args->flags.read_only == 0, dxgalloc[i]->pages);
			if (ret1 != npages) {
				pr_err("get_user_pages_fast failed: %d", ret1);
				if (ret1 > 0 && ret1 < npages)
					release_pages(dxgalloc[i]->pages, ret1);
				vfree(dxgalloc[i]->pages);
				dxgalloc[i]->pages = NULL;
				ret = -ENOMEM;
				goto cleanup;
			}

			page_in = dxgalloc[i]->pages;
			curr_page = 0;
			base_page = 0;
			pages_seen = 0;
			/* We go 1 more loop (<=) on purpose, so we can flush out the last page. */
			for (pages_processed = 0; pages_processed <= npages; ++pages_processed) {
				curr_page = (u64)page_to_phys(*page_in);
				/* In the first loop there is no base page, set it by hand. */
				if (base_page == 0)
					base_page = curr_page;

				/* A new page or we hit the maximum chunk size, or is last page. */
				if ((pages_processed != 0 &&
						curr_page != base_page + pages_seen * PAGE_SIZE) ||
						pages_seen == PAGE_SIZE ||
						pages_processed == npages) {
					if (sysmem_pages_rle_used >= sysmem_pages_rle_limit) {
						pr_err("Hit RLE limit for sysmem, aborting");
						ret = -EOVERFLOW;
						goto cleanup;
					}

					/* Write resulting RLE encoded page range. */
					*sysmem_pages_rle_dest++ = base_page | (pages_seen - 1);
					/* Move base to current. */
					base_page = curr_page;
					/* Reset count. */
					pages_seen = 1;

					/* Keep track of total used and the list length. */
					++sysmem_pages_rle_used;
					++alloc_info->sysmem_pages_rle_size;
				} else {
					/* If this is a continuation, keep counting. */
					++pages_seen;
				}

				/* Move to the next page (unless at last step). */
				if (pages_processed < npages - 1)
					++page_in;
			}
		}

		++alloc_info;
		++input_alloc;
	}

cleanup:
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int create_existing_sysmem(struct dxgdevice *device,
			   struct dxgkvmb_command_allocinfo_return *host_alloc,
			   struct dxgallocation *dxgalloc,
			   bool read_only,
			   const void *sysmem)
{
	int ret1 = 0;
	void *kmem = NULL;
	int ret = 0;
	struct dxgkvmb_command_setexistingsysmemstore *set_store_command;
	struct dxgkvmb_command_setexistingsysmempages *set_pages_command;
	u64 alloc_size = host_alloc->allocation_size;
	u32 npages = alloc_size >> PAGE_SHIFT;
	struct dxgvmbusmsg msg = {.hdr = NULL};
	const u32 max_pfns_in_message =
		(DXG_MAX_VM_BUS_PACKET_SIZE - sizeof(*set_pages_command) -
		PAGE_SIZE) / sizeof(__u64);
	u32 alloc_offset_in_pages = 0;
	struct page **page_in;
	u64 *pfn;
	u32 pages_to_send;
	u32 i;

	/*
	 * Create a guest physical address list and set it as the allocation
	 * backing store in the host. This is done after creating the host
	 * allocation, because only now the allocation size is known.
	 */

	dev_dbg(dxgglobaldev, "   Alloc size: %lld", alloc_size);

	dxgalloc->cpu_address = (void *)sysmem;

	dxgalloc->pages = vzalloc(npages * sizeof(void *));
	if (dxgalloc->pages == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}
	ret1 = get_user_pages_fast((unsigned long)sysmem, npages, !read_only,
				  dxgalloc->pages);
	if (ret1 != npages) {
		pr_err("get_user_pages_fast failed: %d", ret1);
		if (ret1 > 0 && ret1 < npages)
			release_pages(dxgalloc->pages, ret1);
		vfree(dxgalloc->pages);
		dxgalloc->pages = NULL;
		ret = -ENOMEM;
		goto cleanup;
	}
	if (!dxgglobal->map_guest_pages_enabled) {
		ret = init_message(&msg, device->adapter, device->process,
				sizeof(*set_store_command));
		if (ret)
			goto cleanup;
		set_store_command = (void *)msg.msg;

		kmem = vmap(dxgalloc->pages, npages, VM_MAP, PAGE_KERNEL);
		if (kmem == NULL) {
			pr_err("vmap failed");
			ret = -ENOMEM;
			goto cleanup;
		}

		command_vgpu_to_host_init2(&set_store_command->hdr,
					DXGK_VMBCOMMAND_SETEXISTINGSYSMEMSTORE,
					device->process->host_handle);
		set_store_command->device = device->handle;
		set_store_command->allocation = host_alloc->allocation;
		set_store_command->gpadl = dxgalloc->gpadl;
		ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr,
						    msg.size);
		if (ret < 0)
			pr_err("failed to set existing store: %x", ret);
	} else {
		/*
		 * Send the list of the allocation PFNs to the host. The host
		 * will map the pages for GPU access.
		 */

		ret = init_message(&msg, device->adapter, device->process,
				sizeof(*set_pages_command) +
				max_pfns_in_message * sizeof(u64));
		if (ret)
			goto cleanup;
		set_pages_command = (void *)msg.msg;
		command_vgpu_to_host_init2(&set_pages_command->hdr,
					DXGK_VMBCOMMAND_SETEXISTINGSYSMEMPAGES,
					device->process->host_handle);
		set_pages_command->device = device->handle;
		set_pages_command->allocation = host_alloc->allocation;

		page_in = dxgalloc->pages;
		while (alloc_offset_in_pages < npages) {
			pfn = (u64 *)((char *)msg.msg +
				sizeof(*set_pages_command));
			pages_to_send = min(npages - alloc_offset_in_pages,
					    max_pfns_in_message);
			set_pages_command->num_pages = pages_to_send;
			set_pages_command->alloc_offset_in_pages =
				alloc_offset_in_pages;

			for (i = 0; i < pages_to_send; i++)
				*pfn++ = page_to_pfn(*page_in++);

			ret = dxgvmb_send_sync_msg_ntstatus(msg.channel,
							    msg.hdr,
							    msg.size);
			if (ret < 0) {
				pr_err("failed to set existing pages: %x", ret);
				break;
			}
			alloc_offset_in_pages += pages_to_send;
		}
	}

cleanup:
	if (kmem)
		vunmap(kmem);
	free_message(&msg, device->process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

static int
process_allocation_handles(struct dxgprocess *process,
			   struct dxgdevice *device,
			   struct d3dkmt_createallocation *args,
			   struct dxgkvmb_command_createallocation_return *res,
			   struct dxgallocation **dxgalloc,
			   struct dxgresource *resource)
{
	int ret = 0;
	int i;

	hmgrtable_lock(&process->handle_table, DXGLOCK_EXCL);
	if (args->flags.create_resource) {
		ret = hmgrtable_assign_handle(&process->handle_table, resource,
					      HMGRENTRY_TYPE_DXGRESOURCE,
					      res->resource);
		if (ret < 0) {
			pr_err("failed to assign resource handle %x",
				   res->resource.v);
		} else {
			resource->handle = res->resource;
			resource->handle_valid = 1;
		}
	}
	for (i = 0; i < args->alloc_count; i++) {
		struct dxgkvmb_command_allocinfo_return *host_alloc;

		host_alloc = &res->allocation_info[i];
		ret = hmgrtable_assign_handle(&process->handle_table,
					      dxgalloc[i],
					      HMGRENTRY_TYPE_DXGALLOCATION,
					      host_alloc->allocation);
		if (ret < 0) {
			pr_err("failed to assign alloc handle %x %d %d",
				   host_alloc->allocation.v,
				   args->alloc_count, i);
			break;
		}
		dxgalloc[i]->alloc_handle = host_alloc->allocation;
		dxgalloc[i]->handle_valid = 1;
	}
	hmgrtable_unlock(&process->handle_table, DXGLOCK_EXCL);

	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

static int
create_local_allocations(struct dxgprocess *process,
			 struct dxgdevice *device,
			 struct d3dkmt_createallocation *args,
			 struct d3dkmt_createallocation *__user input_args,
			 struct d3dddi_allocationinfo *alloc_info,
			 struct dxgkvmb_command_createallocation_return *result,
			 struct dxgresource *resource,
			 struct dxgallocation **dxgalloc,
			 u32 destroy_buffer_size)
{
	int i;
	int alloc_count = args->alloc_count;
	u8 *alloc_private_data = NULL;
	int ret = 0;
	int ret1;
	struct dxgkvmb_command_destroyallocation *destroy_buf;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, device->adapter, process,
			    destroy_buffer_size);
	if (ret)
		goto cleanup;
	destroy_buf = (void *)msg.msg;

	/* Prepare the command to destroy allocation in case of failure */
	command_vgpu_to_host_init2(&destroy_buf->hdr,
				   DXGK_VMBCOMMAND_DESTROYALLOCATION,
				   process->host_handle);
	destroy_buf->device = args->device;
	destroy_buf->resource = args->resource;
	destroy_buf->alloc_count = alloc_count;
	destroy_buf->flags.assume_not_in_use = 1;
	for (i = 0; i < alloc_count; i++) {
		dev_dbg(dxgglobaldev, "host allocation: %d %x",
			     i, result->allocation_info[i].allocation.v);
		destroy_buf->allocations[i] =
		    result->allocation_info[i].allocation;
	}

	if (args->flags.create_resource) {
		dev_dbg(dxgglobaldev, "new resource: %x", result->resource.v);
		ret = copy_to_user(&input_args->resource, &result->resource,
				   sizeof(struct d3dkmthandle));
		if (ret) {
			pr_err("%s failed to copy resource handle", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	alloc_private_data = (u8 *) result +
	    sizeof(struct dxgkvmb_command_createallocation_return) +
	    sizeof(struct dxgkvmb_command_allocinfo_return) * (alloc_count - 1);

	for (i = 0; i < alloc_count; i++) {
		struct dxgkvmb_command_allocinfo_return *host_alloc;
		struct d3dddi_allocationinfo *user_alloc;

		host_alloc = &result->allocation_info[i];
		user_alloc = &alloc_info[i];
		dxgalloc[i]->num_pages =
		    host_alloc->allocation_size >> PAGE_SHIFT;
		/*
		 * Here we used to call create_existing_sysmem, but now it's handled in
		 * CreateAllocation.
		 */
		dxgalloc[i]->cached = host_alloc->allocation_flags.cached;
		if (host_alloc->priv_drv_data_size) {
			ret = copy_to_user(user_alloc->priv_drv_data,
					   alloc_private_data,
					   host_alloc->priv_drv_data_size);
			if (ret) {
				pr_err("%s failed to copy private data",
					__func__);
				ret = -EINVAL;
				goto cleanup;
			}
			alloc_private_data += host_alloc->priv_drv_data_size;
		}
		ret = copy_to_user(&args->allocation_info[i].allocation,
				   &host_alloc->allocation,
				   sizeof(struct d3dkmthandle));
		if (ret) {
			pr_err("%s failed to copy alloc handle", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	ret = process_allocation_handles(process, device, args, result,
					 dxgalloc, resource);
	if (ret < 0)
		goto cleanup;

	ret = copy_to_user(&input_args->global_share, &args->global_share,
			   sizeof(struct d3dkmthandle));
	if (ret) {
		pr_err("%s failed to copy global share", __func__);
		ret = -EINVAL;
	}

cleanup:

	if (ret < 0) {
		/* Free local handles before freeing the handles in the host */
		dxgdevice_acquire_alloc_list_lock(device);
		if (dxgalloc)
			for (i = 0; i < alloc_count; i++)
				if (dxgalloc[i])
					dxgallocation_free_handle(dxgalloc[i]);
		if (resource && args->flags.create_resource)
			dxgresource_free_handle(resource);
		dxgdevice_release_alloc_list_lock(device);

		/* Destroy allocations in the host to unmap gpadls */
		ret1 = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr,
						     msg.size);
		if (ret1 < 0)
			pr_err("failed to destroy allocations: %x", ret1);

		dxgdevice_acquire_alloc_list_lock(device);
		if (dxgalloc) {
			for (i = 0; i < alloc_count; i++) {
				if (dxgalloc[i]) {
					dxgalloc[i]->alloc_handle.v = 0;
					dxgallocation_destroy(dxgalloc[i]);
					dxgalloc[i] = NULL;
				}
			}
		}
		if (resource && args->flags.create_resource) {
			/*
			 * Prevent the resource memory from freeing.
			 * It will be freed in the top level function.
			 */
			kref_get(&resource->resource_kref);
			dxgresource_destroy(resource);
		}
		dxgdevice_release_alloc_list_lock(device);
	}

	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

static int
get_allocation_size_private_data_copy(struct d3dkmt_createallocation *args,
		  struct dxgkvmb_command_getallocationsize *command,
		  struct d3dddi_allocationinfo *input_alloc_info)
{
	struct d3dddi_allocationinfo *input_alloc;
	int ret = 0;
	int i;
	/* The list of data sizes. */
	u32 *private_data_size_dest = (u32 *) &command[1];
	/* Write the private driver data after the list of data sizes. */
	u8 *private_data_dest = (u8 *) &command[1] + (args->alloc_count * sizeof(u32));

	input_alloc = input_alloc_info;
	for (i = 0; i < args->alloc_count; i++) {
		/* Copy the size and move the size pointer forward. */
		*private_data_size_dest++ = input_alloc->priv_drv_data_size;
		if (input_alloc->priv_drv_data_size) {
			ret = copy_from_user(private_data_dest,
					     input_alloc->priv_drv_data,
					     input_alloc->priv_drv_data_size);
			if (ret) {
				pr_err("%s failed to copy alloc data",
					__func__);
				ret = -EINVAL;
				goto cleanup;
			}
			private_data_dest += input_alloc->priv_drv_data_size;
		}
		input_alloc++;
	}

cleanup:
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_get_allocation_size(struct dxgprocess *process,
				  struct dxgdevice *device,
				  struct d3dkmt_createallocation *args,
				  struct d3dddi_allocationinfo *alloc_info,
				  struct dxgkvmb_command_getallocationsize_return *result,
				  u32 result_size)
{
	struct dxgkvmb_command_getallocationsize *command = NULL;
	int ret = -EINVAL;
	int i;
	u32 cmd_size = 0;
	u32 priv_drv_data_size = 0;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	/* Compute the total private driver size. */
	for (i = 0; i < args->alloc_count; i++) {
		if (alloc_info[i].priv_drv_data_size >=
		    DXG_MAX_VM_BUS_PACKET_SIZE) {
			ret = -EOVERFLOW;
			goto cleanup;
		} else {
			priv_drv_data_size += alloc_info[i].priv_drv_data_size;
		}
		if (priv_drv_data_size >= DXG_MAX_VM_BUS_PACKET_SIZE) {
			ret = -EOVERFLOW;
			goto cleanup;
		}
	}

	cmd_size = sizeof(struct dxgkvmb_command_getallocationsize) +
		/* Each private data size. */
		args->alloc_count * sizeof(u32) +
		/* The actual private data. */
		priv_drv_data_size;
	if (cmd_size > DXG_MAX_VM_BUS_PACKET_SIZE) {
		ret = -EOVERFLOW;
		goto cleanup;
	}

	dev_dbg(dxgglobaldev, "command size, driver_data_size %d %d %ld",
			cmd_size, priv_drv_data_size,
			sizeof(struct dxgkvmb_command_getallocationsize));

	ret = init_message(&msg, device->adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_GETALLOCATIONSIZE,
				   process->host_handle);
	command->device = args->device;
	command->alloc_count = args->alloc_count;

	ret = get_allocation_size_private_data_copy(args, command, alloc_info);
	if (ret < 0)
		goto cleanup;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   result, result_size);
	if (ret < 0) {
		pr_err("send_get_allocation_size failed %x", ret);
		goto cleanup;
	}

	/* Implies host driver issue (not giving right number of results). */
	if (result->alloc_count != args->alloc_count) {
		pr_err("send_get_allocation_size mismatch, expected: %d, found %d",
		       args->alloc_count, result->alloc_count);
		ret = -EINVAL;
		goto cleanup;
	}

	ret = ntstatus2int(result->status);

cleanup:
	free_message(&msg, process);

	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_create_allocation(struct dxgprocess *process,
				  struct dxgdevice *device,
				  struct d3dkmt_createallocation *args,
				  struct d3dkmt_createallocation *__user
				  input_args,
				  struct dxgresource *resource,
				  struct dxgallocation **dxgalloc,
				  struct d3dddi_allocationinfo *alloc_info,
				  struct d3dkmt_createstandardallocation
				  *standard_alloc)
{
	struct dxgkvmb_command_createallocation *command = NULL;
	struct dxgkvmb_command_createallocation_return *result = NULL;
	struct dxgkvmb_command_getallocationsize_return *allocation_size_result = NULL;
	int ret = -EINVAL;
	int i;
	u32 result_size = 0;
	u32 allocation_size_result_size = 0;
	u32 cmd_size = 0;
	u32 destroy_buffer_size = 0;
	u32 priv_drv_data_size = 0;
	u32 sysmem_pages_rle_limit = 0;
	bool sysmem = false;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	if (args->private_runtime_data_size >= DXG_MAX_VM_BUS_PACKET_SIZE ||
	    args->priv_drv_data_size >= DXG_MAX_VM_BUS_PACKET_SIZE) {
		ret = -EOVERFLOW;
		goto cleanup;
	}

	/*
	 * Preallocate the buffer, which will be used for destruction in case
	 * of a failure
	 */
	destroy_buffer_size = sizeof(struct dxgkvmb_command_destroyallocation) +
	    args->alloc_count * sizeof(struct d3dkmthandle);

	/* Compute the total private driver size */

	for (i = 0; i < args->alloc_count; i++) {
		if (alloc_info[i].priv_drv_data_size >=
		    DXG_MAX_VM_BUS_PACKET_SIZE) {
			ret = -EOVERFLOW;
			goto cleanup;
		} else {
			priv_drv_data_size += alloc_info[i].priv_drv_data_size;
		}
		if (priv_drv_data_size >= DXG_MAX_VM_BUS_PACKET_SIZE) {
			ret = -EOVERFLOW;
			goto cleanup;
		}
	}

	/* Check for sysmem by the first element. If set, all must be sysmem. */
	sysmem = args->alloc_count > 0 && alloc_info[0].sysmem;
	for (i = 1; i < args->alloc_count; i++) {
		if (!alloc_info[i].sysmem) {
			ret = -EINVAL;
			goto cleanup;
		}
	}

	/*
	 * Private driver data for the result includes only per allocation
	 * private data
	 */
	result_size = sizeof(struct dxgkvmb_command_createallocation_return) +
	    (args->alloc_count - 1) *
	    sizeof(struct dxgkvmb_command_allocinfo_return) +
	    priv_drv_data_size;
	result = vzalloc(result_size);
	if (result == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	/* Private drv data for the command includes the global private data */
	priv_drv_data_size += args->priv_drv_data_size;

	allocation_size_result_size = sizeof(struct dxgkvmb_command_getallocationsize_return) +
		/* Each returned size. */
		args->alloc_count * sizeof(u64);
	allocation_size_result = vzalloc(allocation_size_result_size);
	if (allocation_size_result == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	/* Get the allocation sizes ahead of time, only if using sysmem. */
	if (sysmem) {
		ret = dxgvmb_send_get_allocation_size(
			process, device, args, alloc_info,
			allocation_size_result, allocation_size_result_size);
		if (ret < 0)
			goto cleanup;
	}

	cmd_size = sizeof(struct dxgkvmb_command_createallocation) +
	    args->alloc_count *
	    sizeof(struct dxgkvmb_command_createallocation_allocinfo) +
	    args->private_runtime_data_size + priv_drv_data_size;

	/*
	 * Since RLE size is hard to predict, we max out the cmd size and store a limit.
	 * The alternative is calculating RLE here and storing it (or calculating it twice).
	 */
	if (sysmem) {
		/* How many bytes are left, and each sysmem RLE entry is a u64. */
		sysmem_pages_rle_limit =
			calculate_max_rle_data_size(allocation_size_result);
		cmd_size = cmd_size + sysmem_pages_rle_limit;
	}

	if (cmd_size > DXG_MAX_VM_BUS_PACKET_SIZE) {
		ret = -EOVERFLOW;
		goto cleanup;
	}

	dev_dbg(dxgglobaldev, "command size, driver_data_size %d %d %ld %ld",
		    cmd_size, priv_drv_data_size,
		    sizeof(struct dxgkvmb_command_createallocation),
		    sizeof(struct dxgkvmb_command_createallocation_allocinfo));

	ret = init_message(&msg, device->adapter, process,
			   cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_CREATEALLOCATION,
				   process->host_handle);
	command->device = args->device;
	command->flags = args->flags;
	command->resource = args->resource;
	command->private_runtime_resource_handle =
	    args->private_runtime_resource_handle;
	command->alloc_count = args->alloc_count;
	command->private_runtime_data_size = args->private_runtime_data_size;
	command->priv_drv_data_size = args->priv_drv_data_size;

	ret = copy_private_data(args, command, alloc_info, standard_alloc);
	if (ret < 0)
		goto cleanup;

	if (sysmem) {
		ret = copy_sysmem_pages_rle_data(args, command, alloc_info, allocation_size_result,
			dxgalloc, sysmem_pages_rle_limit,
			/* When the RLE data starts (after all the private data). */
			args->private_runtime_data_size + priv_drv_data_size);
		if (ret < 0)
			goto cleanup;
	}

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   result, result_size);
	if (ret < 0) {
		pr_err("send_create_allocation failed %x", ret);
		goto cleanup;
	}

	ret = ntstatus2int(result->status);
	if (ret < 0)
		goto cleanup;

	ret = create_local_allocations(process, device, args, input_args,
				       alloc_info, result, resource, dxgalloc,
				       destroy_buffer_size);

cleanup:
	if (result)
		vfree(result);

	if (allocation_size_result)
		vfree(allocation_size_result);

	free_message(&msg, process);

	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_destroy_allocation(struct dxgprocess *process,
				   struct dxgdevice *device,
				   struct d3dkmt_destroyallocation2 *args,
				   struct d3dkmthandle *alloc_handles)
{
	struct dxgkvmb_command_destroyallocation *destroy_buffer;
	u32 destroy_buffer_size;
	int ret;
	int allocations_size = args->alloc_count * sizeof(struct d3dkmthandle);
	struct dxgvmbusmsg msg = {.hdr = NULL};

	destroy_buffer_size = sizeof(struct dxgkvmb_command_destroyallocation) +
	    allocations_size;

	ret = init_message(&msg, device->adapter, process,
			    destroy_buffer_size);
	if (ret)
		goto cleanup;
	destroy_buffer = (void *)msg.msg;

	command_vgpu_to_host_init2(&destroy_buffer->hdr,
				   DXGK_VMBCOMMAND_DESTROYALLOCATION,
				   process->host_handle);
	destroy_buffer->device = args->device;
	destroy_buffer->resource = args->resource;
	destroy_buffer->alloc_count = args->alloc_count;
	destroy_buffer->flags = args->flags;
	if (allocations_size)
		memcpy(destroy_buffer->allocations, alloc_handles,
		       allocations_size);

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);

cleanup:

	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_query_clock_calibration(struct dxgprocess *process,
					struct dxgadapter *adapter,
					struct d3dkmt_queryclockcalibration
					*args,
					struct d3dkmt_queryclockcalibration
					*__user inargs)
{
	struct dxgkvmb_command_queryclockcalibration *command;
	struct dxgkvmb_command_queryclockcalibration_return result;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_QUERYCLOCKCALIBRATION,
				   process->host_handle);
	command->args = *args;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0)
		goto cleanup;
	ret = copy_to_user(&inargs->clock_data, &result.clock_data,
			   sizeof(result.clock_data));
	if (ret) {
		pr_err("%s failed to copy clock data", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	ret = ntstatus2int(result.status);

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_flush_heap_transitions(struct dxgprocess *process,
				       struct dxgadapter *adapter,
				       struct d3dkmt_flushheaptransitions *args)
{
	struct dxgkvmb_command_flushheaptransitions *command;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;
	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_FLUSHHEAPTRANSITIONS,
				   process->host_handle);
	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_query_alloc_residency(struct dxgprocess *process,
				      struct dxgadapter *adapter,
				      struct d3dkmt_queryallocationresidency
				      *args)
{
	int ret = -EINVAL;
	struct dxgkvmb_command_queryallocationresidency *command = NULL;
	u32 cmd_size = sizeof(*command);
	u32 alloc_size = 0;
	u32 result_allocation_size = 0;
	struct dxgkvmb_command_queryallocationresidency_return *result = NULL;
	u32 result_size = sizeof(*result);
	struct dxgvmbusmsgres msg = {.hdr = NULL};

	if (args->allocation_count > DXG_MAX_VM_BUS_PACKET_SIZE) {
		ret = -EINVAL;
		goto cleanup;
	}

	if (args->allocation_count) {
		alloc_size = args->allocation_count *
			     sizeof(struct d3dkmthandle);
		cmd_size += alloc_size;
		result_allocation_size = args->allocation_count *
		    sizeof(args->residency_status[0]);
	} else {
		result_allocation_size = sizeof(args->residency_status[0]);
	}
	result_size += result_allocation_size;

	ret = init_message_res(&msg, adapter, process, cmd_size, result_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;
	result = msg.res;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_QUERYALLOCATIONRESIDENCY,
				   process->host_handle);
	command->args = *args;
	if (alloc_size) {
		ret = copy_from_user(&command[1], args->allocations,
				     alloc_size);
		if (ret) {
			pr_err("%s failed to copy alloc handles", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   result, msg.res_size);
	if (ret < 0)
		goto cleanup;

	ret = ntstatus2int(result->status);
	if (ret < 0)
		goto cleanup;

	ret = copy_to_user(args->residency_status, &result[1],
			   result_allocation_size);
	if (ret) {
		pr_err("%s failed to copy residency status", __func__);
		ret = -EINVAL;
	}

cleanup:
	free_message((struct dxgvmbusmsg *)&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_escape(struct dxgprocess *process,
		       struct dxgadapter *adapter,
		       struct d3dkmt_escape *args)
{
	int ret;
	struct dxgkvmb_command_escape *command = NULL;
	u32 cmd_size = sizeof(*command);
	struct dxgvmbusmsg msg = {.hdr = NULL};

	if (args->priv_drv_data_size > DXG_MAX_VM_BUS_PACKET_SIZE) {
		ret = -EINVAL;
		goto cleanup;
	}

	cmd_size = cmd_size - sizeof(args->priv_drv_data[0]) +
	    args->priv_drv_data_size;

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;
	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_ESCAPE,
				   process->host_handle);
	command->adapter = args->adapter;
	command->device = args->device;
	command->type = args->type;
	command->flags = args->flags;
	command->priv_drv_data_size = args->priv_drv_data_size;
	command->context = args->context;
	if (args->priv_drv_data_size) {
		ret = copy_from_user(command->priv_drv_data,
				     args->priv_drv_data,
				     args->priv_drv_data_size);
		if (ret) {
			pr_err("%s failed to copy priv data", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   command->priv_drv_data,
				   args->priv_drv_data_size);
	if (ret < 0)
		goto cleanup;

	if (args->priv_drv_data_size) {
		ret = copy_to_user(args->priv_drv_data,
				   command->priv_drv_data,
				   args->priv_drv_data_size);
		if (ret) {
			pr_err("%s failed to copy priv data", __func__);
			ret = -EINVAL;
		}
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_query_vidmem_info(struct dxgprocess *process,
				  struct dxgadapter *adapter,
				  struct d3dkmt_queryvideomemoryinfo *args,
				  struct d3dkmt_queryvideomemoryinfo *__user
				  output)
{
	int ret;
	struct dxgkvmb_command_queryvideomemoryinfo *command;
	struct dxgkvmb_command_queryvideomemoryinfo_return result = { };
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;
	command_vgpu_to_host_init2(&command->hdr,
				   dxgk_vmbcommand_queryvideomemoryinfo,
				   process->host_handle);
	command->adapter = args->adapter;
	command->memory_segment_group = args->memory_segment_group;
	command->physical_adapter_index = args->physical_adapter_index;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0)
		goto cleanup;

	ret = copy_to_user(&output->budget, &result.budget,
			   sizeof(output->budget));
	if (ret) {
		pr_err("%s failed to copy budget", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	ret = copy_to_user(&output->current_usage, &result.current_usage,
			   sizeof(output->current_usage));
	if (ret) {
		pr_err("%s failed to copy current usage", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	ret = copy_to_user(&output->current_reservation,
			   &result.current_reservation,
			   sizeof(output->current_reservation));
	if (ret) {
		pr_err("%s failed to copy reservation", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	ret = copy_to_user(&output->available_for_reservation,
			   &result.available_for_reservation,
			   sizeof(output->available_for_reservation));
	if (ret) {
		pr_err("%s failed to copy avail reservation", __func__);
		ret = -EINVAL;
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_get_device_state(struct dxgprocess *process,
				 struct dxgadapter *adapter,
				 struct d3dkmt_getdevicestate *args,
				 struct d3dkmt_getdevicestate *__user output)
{
	int ret;
	struct dxgkvmb_command_getdevicestate *command;
	struct dxgkvmb_command_getdevicestate_return result = { };
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   dxgk_vmbcommand_getdevicestate,
				   process->host_handle);
	command->args = *args;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0)
		goto cleanup;

	ret = ntstatus2int(result.status);
	if (ret < 0)
		goto cleanup;

	ret = copy_to_user(output, &result.args, sizeof(result.args));
	if (ret) {
		pr_err("%s failed to copy output args", __func__);
		ret = -EINVAL;
	}

	if (args->state_type == _D3DKMT_DEVICESTATE_EXECUTION)
		args->execution_state = result.args.execution_state;

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_open_resource(struct dxgprocess *process,
			      struct dxgadapter *adapter,
			      struct d3dkmthandle device,
			      struct d3dkmthandle global_share,
			      u32 allocation_count,
			      u32 total_priv_drv_data_size,
			      struct d3dkmthandle *resource_handle,
			      struct d3dkmthandle *alloc_handles)
{
	struct dxgkvmb_command_openresource *command;
	struct dxgkvmb_command_openresource_return *result;
	struct d3dkmthandle *handles;
	int ret;
	int i;
	u32 result_size = allocation_count * sizeof(struct d3dkmthandle) +
			   sizeof(*result);
	struct dxgvmbusmsgres msg = {.hdr = NULL};

	ret = init_message_res(&msg, adapter, process, sizeof(*command),
			       result_size);
	if (ret)
		goto cleanup;
	command = msg.msg;
	result = msg.res;

	command_vgpu_to_host_init2(&command->hdr, DXGK_VMBCOMMAND_OPENRESOURCE,
				   process->host_handle);
	command->device = device;
	command->nt_security_sharing = 1;
	command->global_share = global_share;
	command->allocation_count = allocation_count;
	command->total_priv_drv_data_size = total_priv_drv_data_size;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   result, msg.res_size);
	if (ret < 0)
		goto cleanup;

	ret = ntstatus2int(result->status);
	if (ret < 0)
		goto cleanup;

	*resource_handle = result->resource;
	handles = (struct d3dkmthandle *) &result[1];
	for (i = 0; i < allocation_count; i++)
		alloc_handles[i] = handles[i];

cleanup:
	free_message((struct dxgvmbusmsg *)&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_get_stdalloc_data(struct dxgdevice *device,
				  enum d3dkmdt_standardallocationtype alloctype,
				  struct d3dkmdt_gdisurfacedata *alloc_data,
				  u32 physical_adapter_index,
				  u32 *alloc_priv_driver_size,
				  void *priv_alloc_data,
				  u32 *res_priv_data_size,
				  void *priv_res_data)
{
	struct dxgkvmb_command_getstandardallocprivdata *command;
	struct dxgkvmb_command_getstandardallocprivdata_return *result = NULL;
	u32 result_size = sizeof(*result);
	int ret;
	struct dxgvmbusmsgres msg = {.hdr = NULL};

	if (priv_alloc_data)
		result_size += *alloc_priv_driver_size;
	if (priv_res_data)
		result_size += *res_priv_data_size;
	ret = init_message_res(&msg, device->adapter, device->process,
			       sizeof(*command), result_size);
	if (ret)
		goto cleanup;
	command = msg.msg;
	result = msg.res;

	command_vgpu_to_host_init2(&command->hdr,
			DXGK_VMBCOMMAND_DDIGETSTANDARDALLOCATIONDRIVERDATA,
			device->process->host_handle);

	command->alloc_type = alloctype;
	command->priv_driver_data_size = *alloc_priv_driver_size;
	command->physical_adapter_index = physical_adapter_index;
	command->priv_driver_resource_size = *res_priv_data_size;
	switch (alloctype) {
	case _D3DKMDT_STANDARDALLOCATION_GDISURFACE:
		command->gdi_surface = *alloc_data;
		break;
	case _D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
	case _D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
	case _D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
	default:
		pr_err("Invalid standard alloc type");
		goto cleanup;
	}

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   result, msg.res_size);
	if (ret < 0)
		goto cleanup;

	ret = ntstatus2int(result->status);
	if (ret < 0)
		goto cleanup;

	if (*alloc_priv_driver_size &&
	    result->priv_driver_data_size != *alloc_priv_driver_size) {
		pr_err("Priv data size mismatch");
		goto cleanup;
	}
	if (*res_priv_data_size &&
	    result->priv_driver_resource_size != *res_priv_data_size) {
		pr_err("Resource priv data size mismatch");
		goto cleanup;
	}
	*alloc_priv_driver_size = result->priv_driver_data_size;
	*res_priv_data_size = result->priv_driver_resource_size;
	if (priv_alloc_data) {
		memcpy(priv_alloc_data, &result[1],
		       result->priv_driver_data_size);
	}
	if (priv_res_data) {
		memcpy(priv_res_data,
		       (char *)(&result[1]) + result->priv_driver_data_size,
		       result->priv_driver_resource_size);
	}

cleanup:

	free_message((struct dxgvmbusmsg *)&msg, device->process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_make_resident(struct dxgprocess *process,
			      struct dxgadapter *adapter,
			      struct d3dddi_makeresident *args)
{
	int ret;
	u32 cmd_size;
	struct dxgkvmb_command_makeresident_return result = { };
	struct dxgkvmb_command_makeresident *command = NULL;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	cmd_size = (args->alloc_count - 1) * sizeof(struct d3dkmthandle) +
		   sizeof(struct dxgkvmb_command_makeresident);

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	ret = copy_from_user(command->allocations, args->allocation_list,
			     args->alloc_count *
			     sizeof(struct d3dkmthandle));
	if (ret) {
		pr_err("%s failed to copy alloc handles", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_MAKERESIDENT,
				   process->host_handle);
	command->alloc_count = args->alloc_count;
	command->paging_queue = args->paging_queue;
	command->flags = args->flags;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0) {
		pr_err("send_make_resident failed %x", ret);
		goto cleanup;
	}

	args->paging_fence_value = result.paging_fence_value;
	args->num_bytes_to_trim = result.num_bytes_to_trim;
	ret = ntstatus2int(result.status);

cleanup:

	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_evict(struct dxgprocess *process,
		      struct dxgadapter *adapter,
		      struct d3dkmt_evict *args)
{
	int ret;
	u32 cmd_size;
	struct dxgkvmb_command_evict_return result = { };
	struct dxgkvmb_command_evict *command = NULL;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	cmd_size = (args->alloc_count - 1) * sizeof(struct d3dkmthandle) +
	    sizeof(struct dxgkvmb_command_evict);
	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;
	ret = copy_from_user(command->allocations, args->allocations,
			     args->alloc_count *
			     sizeof(struct d3dkmthandle));
	if (ret) {
		pr_err("%s failed to copy alloc handles", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_EVICT, process->host_handle);
	command->alloc_count = args->alloc_count;
	command->device = args->device;
	command->flags = args->flags;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0) {
		pr_err("send_evict failed %x", ret);
		goto cleanup;
	}
	args->num_bytes_to_trim = result.num_bytes_to_trim;

cleanup:

	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_submit_command(struct dxgprocess *process,
			       struct dxgadapter *adapter,
			       struct d3dkmt_submitcommand *args)
{
	int ret;
	u32 cmd_size;
	struct dxgkvmb_command_submitcommand *command;
	u32 hbufsize = args->num_history_buffers * sizeof(struct d3dkmthandle);
	struct dxgvmbusmsg msg = {.hdr = NULL};

	cmd_size = sizeof(struct dxgkvmb_command_submitcommand) +
	    hbufsize + args->priv_drv_data_size;

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	ret = copy_from_user(&command[1], args->history_buffer_array,
			     hbufsize);
	if (ret) {
		pr_err("%s failed to copy history buffer", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	ret = copy_from_user((u8 *) &command[1] + hbufsize,
			     args->priv_drv_data, args->priv_drv_data_size);
	if (ret) {
		pr_err("%s failed to copy history priv data", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_SUBMITCOMMAND,
				   process->host_handle);
	command->args = *args;

	if (dxgglobal->async_msg_enabled) {
		command->hdr.async_msg = 1;
		ret = dxgvmb_send_async_msg(msg.channel, msg.hdr, msg.size);
	} else {
		ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr,
						    msg.size);
	}

cleanup:

	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_map_gpu_va(struct dxgprocess *process,
			   struct d3dkmthandle device,
			   struct dxgadapter *adapter,
			   struct d3dddi_mapgpuvirtualaddress *args)
{
	struct dxgkvmb_command_mapgpuvirtualaddress *command;
	struct dxgkvmb_command_mapgpuvirtualaddress_return result;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_MAPGPUVIRTUALADDRESS,
				   process->host_handle);
	command->args = *args;
	command->device = device;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size, &result,
				   sizeof(result));
	if (ret < 0)
		goto cleanup;
	args->virtual_address = result.virtual_address;
	args->paging_fence_value = result.paging_fence_value;
	ret = ntstatus2int(result.status);

cleanup:

	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_reserve_gpu_va(struct dxgprocess *process,
			       struct dxgadapter *adapter,
			       struct d3dddi_reservegpuvirtualaddress *args)
{
	struct dxgkvmb_command_reservegpuvirtualaddress *command;
	struct dxgkvmb_command_reservegpuvirtualaddress_return result;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_RESERVEGPUVIRTUALADDRESS,
				   process->host_handle);
	command->args = *args;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size, &result,
				   sizeof(result));
	args->virtual_address = result.virtual_address;
	ret = ntstatus2int(result.status);

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_free_gpu_va(struct dxgprocess *process,
			    struct dxgadapter *adapter,
			    struct d3dkmt_freegpuvirtualaddress *args)
{
	struct dxgkvmb_command_freegpuvirtualaddress *command;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_FREEGPUVIRTUALADDRESS,
				   process->host_handle);
	command->args = *args;

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_update_gpu_va(struct dxgprocess *process,
			      struct dxgadapter *adapter,
			      struct d3dkmt_updategpuvirtualaddress *args)
{
	struct dxgkvmb_command_updategpuvirtualaddress *command;
	u32 cmd_size;
	u32 op_size;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	if (args->num_operations == 0 ||
	    (DXG_MAX_VM_BUS_PACKET_SIZE /
	     sizeof(struct d3dddi_updategpuvirtualaddress_operation)) <
	    args->num_operations) {
		ret = -EINVAL;
		pr_err("Invalid number of operations: %d",
			   args->num_operations);
		goto cleanup;
	}

	op_size = args->num_operations *
	    sizeof(struct d3dddi_updategpuvirtualaddress_operation);
	cmd_size = sizeof(struct dxgkvmb_command_updategpuvirtualaddress) +
	    op_size - sizeof(args->operations[0]);

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_UPDATEGPUVIRTUALADDRESS,
				   process->host_handle);
	command->fence_value = args->fence_value;
	command->device = args->device;
	command->context = args->context;
	command->fence_object = args->fence_object;
	command->num_operations = args->num_operations;
	command->flags = args->flags.value;
	ret = copy_from_user(command->operations, args->operations,
			     op_size);
	if (ret) {
		pr_err("%s failed to copy operations", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

static void set_result(struct d3dkmt_createsynchronizationobject2 *args,
		       u64 fence_gpu_va, u8 *va)
{
	args->info.periodic_monitored_fence.fence_gpu_virtual_address =
	    fence_gpu_va;
	args->info.periodic_monitored_fence.fence_cpu_virtual_address = va;
}

int
dxgvmb_send_create_sync_object(struct dxgprocess *process,
			       struct dxgadapter *adapter,
			       struct d3dkmt_createsynchronizationobject2 *args,
			       struct dxgsyncobject *syncobj)
{
	struct dxgkvmb_command_createsyncobject_return result = { };
	struct dxgkvmb_command_createsyncobject *command;
	int ret;
	u8 *va = 0;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_CREATESYNCOBJECT,
				   process->host_handle);
	command->args = *args;
	command->client_hint = 1;	/* CLIENTHINT_UMD */

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size, &result,
				   sizeof(result));
	if (ret < 0) {
		pr_err("%s failed %d", __func__, ret);
		goto cleanup;
	}
	args->sync_object = result.sync_object;
	if (syncobj->shared)
		args->info.shared_handle = result.global_sync_object;

	if (syncobj->monitored_fence) {
		va = dxg_map_iospace(result.fence_storage_address, PAGE_SIZE,
				     PROT_READ | PROT_WRITE, true);
		if (va == NULL) {
			ret = -ENOMEM;
			goto cleanup;
		}
		if (args->info.type == _D3DDDI_MONITORED_FENCE) {
			args->info.monitored_fence.fence_gpu_virtual_address =
			    result.fence_gpu_va;
			args->info.monitored_fence.fence_cpu_virtual_address =
			    va;
			{
				unsigned long value;

				dev_dbg(dxgglobaldev, "fence cpu va: %p", va);
				ret = copy_from_user(&value, va,
						     sizeof(u64));
				if (ret) {
					pr_err("failed to read fence");
					ret = -EINVAL;
				} else {
					dev_dbg(dxgglobaldev, "fence value:%lx",
						value);
				}
			}
		} else {
			set_result(args, result.fence_gpu_va, va);
		}
		syncobj->mapped_address = va;
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_signal_sync_object(struct dxgprocess *process,
				   struct dxgadapter *adapter,
				   struct d3dddicb_signalflags flags,
				   u64 legacy_fence_value,
				   struct d3dkmthandle context,
				   u32 object_count,
				   struct d3dkmthandle __user *objects,
				   u32 context_count,
				   struct d3dkmthandle __user *contexts,
				   u32 fence_count,
				   u64 __user *fences,
				   struct eventfd_ctx *cpu_event_handle,
				   struct d3dkmthandle device,
				   bool user_address)
{
	int ret;
	struct dxgkvmb_command_signalsyncobject *command;
	u32 object_size = object_count * sizeof(struct d3dkmthandle);
	u32 context_size = context_count * sizeof(struct d3dkmthandle);
	u32 fence_size = fences ? fence_count * sizeof(u64) : 0;
	u8 *current_pos;
	u32 cmd_size = sizeof(struct dxgkvmb_command_signalsyncobject) +
	    object_size + context_size + fence_size;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	if (context.v)
		cmd_size += sizeof(struct d3dkmthandle);

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_SIGNALSYNCOBJECT,
				   process->host_handle);

	if (flags.enqueue_cpu_event)
		command->cpu_event_handle = (u64) cpu_event_handle;
	else
		command->device = device;
	command->flags = flags;
	command->fence_value = legacy_fence_value;
	command->object_count = object_count;
	command->context_count = context_count;
	current_pos = (u8 *) &command[1];
	if (user_address) {
		ret = copy_from_user(current_pos, objects, object_size);
		if (ret) {
			pr_err("Failed to read objects %p %d",
				objects, object_size);
			ret = -EINVAL;
			goto cleanup;
		}
	} else {
		memcpy(current_pos, objects, object_size);
	}
	current_pos += object_size;
	if (context.v) {
		command->context_count++;
		*(struct d3dkmthandle *) current_pos = context;
		current_pos += sizeof(struct d3dkmthandle);
	}
	if (context_size) {
		if (user_address) {
			ret = copy_from_user(current_pos, contexts, context_size);
			if (ret) {
				pr_err("Failed to read contexts %p %d",
					contexts, context_size);
				ret = -EINVAL;
				goto cleanup;
			}
		} else {
			memcpy(current_pos, contexts, context_size);
		}
		current_pos += context_size;
	}
	if (fence_size) {
		if (user_address) {
			ret = copy_from_user(current_pos, fences, fence_size);
			if (ret) {
				pr_err("Failed to read fences %p %d",
					fences, fence_size);
				ret = -EINVAL;
				goto cleanup;
			}
		} else {
			memcpy(current_pos, fences, fence_size);
		}
	}

	if (dxgglobal->async_msg_enabled) {
		command->hdr.async_msg = 1;
		ret = dxgvmb_send_async_msg(msg.channel, msg.hdr, msg.size);
	} else {
		ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr,
						    msg.size);
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_wait_sync_object_cpu(struct dxgprocess *process,
				     struct dxgadapter *adapter,
				     struct
				     d3dkmt_waitforsynchronizationobjectfromcpu
				     *args,
				     bool user_address,
				     u64 cpu_event)
{
	int ret = -EINVAL;
	struct dxgkvmb_command_waitforsyncobjectfromcpu *command;
	u32 object_size = args->object_count * sizeof(struct d3dkmthandle);
	u32 fence_size = args->object_count * sizeof(u64);
	u8 *current_pos;
	u32 cmd_size = sizeof(*command) + object_size + fence_size;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMCPU,
				   process->host_handle);
	command->device = args->device;
	command->flags = args->flags;
	command->object_count = args->object_count;
	command->guest_event_pointer = (u64) cpu_event;
	current_pos = (u8 *) &command[1];
	if (user_address) {
		ret = copy_from_user(current_pos, args->objects, object_size);
		if (ret) {
			pr_err("%s failed to copy objects", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
		current_pos += object_size;
		ret = copy_from_user(current_pos, args->fence_values,
				     fence_size);
		if (ret) {
			pr_err("%s failed to copy fences", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	} else {
		memcpy(current_pos, args->objects, object_size);
		current_pos += object_size;
		memcpy(current_pos, args->fence_values, fence_size);
	}

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_wait_sync_object_gpu(struct dxgprocess *process,
				     struct dxgadapter *adapter,
				     struct d3dkmthandle context,
				     u32 object_count,
				     struct d3dkmthandle *objects,
				     u64 *fences,
				     bool legacy_fence)
{
	int ret;
	struct dxgkvmb_command_waitforsyncobjectfromgpu *command;
	u32 fence_size = object_count * sizeof(u64);
	u32 object_size = object_count * sizeof(struct d3dkmthandle);
	u8 *current_pos;
	u32 cmd_size = object_size + fence_size - sizeof(u64) +
	    sizeof(struct dxgkvmb_command_waitforsyncobjectfromgpu);
	struct dxgvmbusmsg msg = {.hdr = NULL};

	if (object_count == 0 || object_count > D3DDDI_MAX_OBJECT_WAITED_ON) {
		ret = -EINVAL;
		goto cleanup;
	}
	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMGPU,
				   process->host_handle);
	command->context = context;
	command->object_count = object_count;
	command->legacy_fence_object = legacy_fence;
	current_pos = (u8 *) command->fence_values;
	memcpy(current_pos, fences, fence_size);
	current_pos += fence_size;
	memcpy(current_pos, objects, object_size);

	if (dxgglobal->async_msg_enabled) {
		command->hdr.async_msg = 1;
		ret = dxgvmb_send_async_msg(msg.channel, msg.hdr, msg.size);
	} else {
		ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr,
						    msg.size);
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_lock2(struct dxgprocess *process,
		      struct dxgadapter *adapter,
		      struct d3dkmt_lock2 *args,
		      struct d3dkmt_lock2 *__user outargs)
{
	int ret;
	struct dxgkvmb_command_lock2 *command;
	struct dxgkvmb_command_lock2_return result = { };
	struct dxgallocation *alloc = NULL;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_LOCK2, process->host_handle);
	command->args = *args;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0)
		goto cleanup;

	ret = ntstatus2int(result.status);
	if (ret < 0)
		goto cleanup;

	hmgrtable_lock(&process->handle_table, DXGLOCK_EXCL);
	alloc = hmgrtable_get_object_by_type(&process->handle_table,
					     HMGRENTRY_TYPE_DXGALLOCATION,
					     args->allocation, true);
	if (alloc == NULL) {
		pr_err("%s invalid alloc", __func__);
		ret = -EINVAL;
	} else {
		if (alloc->cpu_address) {
			args->data = alloc->cpu_address;
			if (alloc->cpu_address_mapped)
				alloc->cpu_address_refcount++;
		} else {
			u64 offset = (u64)result.cpu_visible_buffer_offset;

			args->data = dxg_map_iospace(offset,
					alloc->num_pages << PAGE_SHIFT,
					PROT_READ | PROT_WRITE, alloc->cached);
			if (args->data) {
				alloc->cpu_address_refcount = 1;
				alloc->cpu_address_mapped = true;
				alloc->cpu_address = args->data;
			}
		}
		if (args->data == NULL) {
			ret = -ENOMEM;
		} else {
			ret = copy_to_user(&outargs->data, &args->data,
					   sizeof(args->data));
			if (ret) {
				pr_err("%s failed to copy data", __func__);
				ret = -EINVAL;
				alloc->cpu_address_refcount--;
				if (alloc->cpu_address_refcount == 0) {
					dxg_unmap_iospace(alloc->cpu_address,
					   alloc->num_pages << PAGE_SHIFT);
					alloc->cpu_address_mapped = false;
					alloc->cpu_address = NULL;
				}
			}
		}
	}
	hmgrtable_unlock(&process->handle_table, DXGLOCK_EXCL);

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_unlock2(struct dxgprocess *process,
			struct dxgadapter *adapter,
			struct d3dkmt_unlock2 *args)
{
	int ret;
	struct dxgkvmb_command_unlock2 *command;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_UNLOCK2,
				   process->host_handle);
	command->args = *args;

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_update_alloc_property(struct dxgprocess *process,
				      struct dxgadapter *adapter,
				      struct d3dddi_updateallocproperty *args,
				      struct d3dddi_updateallocproperty *__user
				      inargs)
{
	int ret;
	int ret1;
	struct dxgkvmb_command_updateallocationproperty *command;
	struct dxgkvmb_command_updateallocationproperty_return result = { };
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_UPDATEALLOCATIONPROPERTY,
				   process->host_handle);
	command->args = *args;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));

	if (ret < 0)
		goto cleanup;
	ret = ntstatus2int(result.status);
	/* STATUS_PENING is a success code > 0 */
	if (ret == STATUS_PENDING) {
		ret1 = copy_to_user(&inargs->paging_fence_value,
				    &result.paging_fence_value,
				    sizeof(u64));
		if (ret1) {
			pr_err("%s failed to copy paging fence", __func__);
			ret = -EINVAL;
		}
	}
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_mark_device_as_error(struct dxgprocess *process,
				     struct dxgadapter *adapter,
				     struct d3dkmt_markdeviceaserror *args)
{
	struct dxgkvmb_command_markdeviceaserror *command;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_MARKDEVICEASERROR,
				   process->host_handle);
	command->args = *args;
	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_set_allocation_priority(struct dxgprocess *process,
				struct dxgadapter *adapter,
				struct d3dkmt_setallocationpriority *args)
{
	u32 cmd_size = sizeof(struct dxgkvmb_command_setallocationpriority);
	u32 alloc_size = 0;
	u32 priority_size = 0;
	struct dxgkvmb_command_setallocationpriority *command;
	int ret;
	struct d3dkmthandle *allocations;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	if (args->allocation_count > DXG_MAX_VM_BUS_PACKET_SIZE) {
		ret = -EINVAL;
		goto cleanup;
	}
	if (args->resource.v) {
		priority_size = sizeof(u32);
		if (args->allocation_count != 0) {
			ret = -EINVAL;
			goto cleanup;
		}
	} else {
		if (args->allocation_count == 0) {
			ret = -EINVAL;
			goto cleanup;
		}
		alloc_size = args->allocation_count *
			     sizeof(struct d3dkmthandle);
		cmd_size += alloc_size;
		priority_size = sizeof(u32) * args->allocation_count;
	}
	cmd_size += priority_size;

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_SETALLOCATIONPRIORITY,
				   process->host_handle);
	command->device = args->device;
	command->allocation_count = args->allocation_count;
	command->resource = args->resource;
	allocations = (struct d3dkmthandle *) &command[1];
	ret = copy_from_user(allocations, args->allocation_list,
			     alloc_size);
	if (ret) {
		pr_err("%s failed to copy alloc handle", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	ret = copy_from_user((u8 *) allocations + alloc_size,
				args->priorities, priority_size);
	if (ret) {
		pr_err("%s failed to copy alloc priority", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_get_allocation_priority(struct dxgprocess *process,
				struct dxgadapter *adapter,
				struct d3dkmt_getallocationpriority *args)
{
	u32 cmd_size = sizeof(struct dxgkvmb_command_getallocationpriority);
	u32 result_size;
	u32 alloc_size = 0;
	u32 priority_size = 0;
	struct dxgkvmb_command_getallocationpriority *command;
	struct dxgkvmb_command_getallocationpriority_return *result;
	int ret;
	struct d3dkmthandle *allocations;
	struct dxgvmbusmsgres msg = {.hdr = NULL};

	if (args->allocation_count > DXG_MAX_VM_BUS_PACKET_SIZE) {
		ret = -EINVAL;
		goto cleanup;
	}
	if (args->resource.v) {
		priority_size = sizeof(u32);
		if (args->allocation_count != 0) {
			ret = -EINVAL;
			goto cleanup;
		}
	} else {
		if (args->allocation_count == 0) {
			ret = -EINVAL;
			goto cleanup;
		}
		alloc_size = args->allocation_count *
			sizeof(struct d3dkmthandle);
		cmd_size += alloc_size;
		priority_size = sizeof(u32) * args->allocation_count;
	}
	result_size = sizeof(*result) + priority_size;

	ret = init_message_res(&msg, adapter, process, cmd_size, result_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;
	result = msg.res;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_GETALLOCATIONPRIORITY,
				   process->host_handle);
	command->device = args->device;
	command->allocation_count = args->allocation_count;
	command->resource = args->resource;
	allocations = (struct d3dkmthandle *) &command[1];
	ret = copy_from_user(allocations, args->allocation_list,
			     alloc_size);
	if (ret) {
		pr_err("%s failed to copy alloc handles", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr,
				   msg.size + msg.res_size,
				   result, msg.res_size);
	if (ret < 0)
		goto cleanup;

	ret = ntstatus2int(result->status);
	if (ret < 0)
		goto cleanup;

	ret = copy_to_user(args->priorities,
			   (u8 *) result + sizeof(*result),
			   priority_size);
	if (ret) {
		pr_err("%s failed to copy priorities", __func__);
		ret = -EINVAL;
	}

cleanup:
	free_message((struct dxgvmbusmsg *)&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_set_context_sch_priority(struct dxgprocess *process,
					 struct dxgadapter *adapter,
					 struct d3dkmthandle context,
					 int priority,
					 bool in_process)
{
	struct dxgkvmb_command_setcontextschedulingpriority2 *command;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_SETCONTEXTSCHEDULINGPRIORITY,
				   process->host_handle);
	command->context = context;
	command->priority = priority;
	command->in_process = in_process;
	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_get_context_sch_priority(struct dxgprocess *process,
					 struct dxgadapter *adapter,
					 struct d3dkmthandle context,
					 int *priority,
					 bool in_process)
{
	struct dxgkvmb_command_getcontextschedulingpriority *command;
	struct dxgkvmb_command_getcontextschedulingpriority_return result = { };
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_GETCONTEXTSCHEDULINGPRIORITY,
				   process->host_handle);
	command->context = context;
	command->in_process = in_process;
	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret >= 0) {
		ret = ntstatus2int(result.status);
		*priority = result.priority;
	}
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_offer_allocations(struct dxgprocess *process,
				  struct dxgadapter *adapter,
				  struct d3dkmt_offerallocations *args)
{
	struct dxgkvmb_command_offerallocations *command;
	int ret = -EINVAL;
	u32 alloc_size = sizeof(struct d3dkmthandle) * args->allocation_count;
	u32 cmd_size = sizeof(struct dxgkvmb_command_offerallocations) +
			alloc_size - sizeof(struct d3dkmthandle);
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_OFFERALLOCATIONS,
				   process->host_handle);
	command->flags = args->flags;
	command->priority = args->priority;
	command->device = args->device;
	command->allocation_count = args->allocation_count;
	if (args->resources) {
		command->resources = true;
		ret = copy_from_user(command->allocations, args->resources,
				     alloc_size);
	} else {
		ret = copy_from_user(command->allocations,
				     args->allocations, alloc_size);
	}
	if (ret) {
		pr_err("%s failed to copy input handles", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_reclaim_allocations(struct dxgprocess *process,
				    struct dxgadapter *adapter,
				    struct d3dkmthandle device,
				    struct d3dkmt_reclaimallocations2 *args,
				    u64  __user *paging_fence_value)
{
	struct dxgkvmb_command_reclaimallocations *command;
	struct dxgkvmb_command_reclaimallocations_return *result;
	int ret;
	u32 alloc_size = sizeof(struct d3dkmthandle) * args->allocation_count;
	u32 cmd_size = sizeof(struct dxgkvmb_command_reclaimallocations) +
	    alloc_size - sizeof(struct d3dkmthandle);
	u32 result_size = sizeof(*result);
	struct dxgvmbusmsgres msg = {.hdr = NULL};

	if (args->results)
		result_size += (args->allocation_count - 1) *
				sizeof(enum d3dddi_reclaim_result);

	ret = init_message_res(&msg, adapter, process, cmd_size, result_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;
	result = msg.res;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_RECLAIMALLOCATIONS,
				   process->host_handle);
	command->device = device;
	command->paging_queue = args->paging_queue;
	command->allocation_count = args->allocation_count;
	command->write_results = args->results != NULL;
	if (args->resources) {
		command->resources = true;
		ret = copy_from_user(command->allocations, args->resources,
					 alloc_size);
	} else {
		ret = copy_from_user(command->allocations,
					 args->allocations, alloc_size);
	}
	if (ret) {
		pr_err("%s failed to copy input handles", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   result, msg.res_size);
	if (ret < 0)
		goto cleanup;
	ret = copy_to_user(paging_fence_value,
			   &result->paging_fence_value, sizeof(u64));
	if (ret) {
		pr_err("%s failed to copy paging fence", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	ret = ntstatus2int(result->status);
	if (NT_SUCCESS(result->status) && args->results) {
		ret = copy_to_user(args->results, result->discarded,
				   sizeof(result->discarded[0]) *
				   args->allocation_count);
		if (ret) {
			pr_err("%s failed to copy results", __func__);
			ret = -EINVAL;
		}
	}

cleanup:
	free_message((struct dxgvmbusmsg *)&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_change_vidmem_reservation(struct dxgprocess *process,
					  struct dxgadapter *adapter,
					  struct d3dkmthandle other_process,
					  struct
					  d3dkmt_changevideomemoryreservation
					  *args)
{
	struct dxgkvmb_command_changevideomemoryreservation *command;
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_CHANGEVIDEOMEMORYRESERVATION,
				   process->host_handle);
	command->args = *args;
	command->args.process = other_process.v;

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_create_hwqueue(struct dxgprocess *process,
			       struct dxgadapter *adapter,
			       struct d3dkmt_createhwqueue *args,
			       struct d3dkmt_createhwqueue *__user inargs,
			       struct dxghwqueue *hwqueue)
{
	struct dxgkvmb_command_createhwqueue *command = NULL;
	u32 cmd_size = sizeof(struct dxgkvmb_command_createhwqueue);
	int ret;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	if (args->priv_drv_data_size > DXG_MAX_VM_BUS_PACKET_SIZE) {
		pr_err("invalid private driver data size");
		ret = -EINVAL;
		goto cleanup;
	}

	if (args->priv_drv_data_size)
		cmd_size += args->priv_drv_data_size - 1;

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_CREATEHWQUEUE,
				   process->host_handle);
	command->context = args->context;
	command->flags = args->flags;
	command->priv_drv_data_size = args->priv_drv_data_size;
	if (args->priv_drv_data_size) {
		ret = copy_from_user(command->priv_drv_data,
				     args->priv_drv_data,
				     args->priv_drv_data_size);
		if (ret) {
			pr_err("%s failed to copy private data", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   command, cmd_size);
	if (ret < 0)
		goto cleanup;

	ret = ntstatus2int(command->status);
	if (ret < 0) {
		pr_err("dxgvmb_send_sync_msg failed: %x", command->status.v);
		goto cleanup;
	}

	ret = hmgrtable_assign_handle_safe(&process->handle_table, hwqueue,
					   HMGRENTRY_TYPE_DXGHWQUEUE,
					   command->hwqueue);
	if (ret < 0)
		goto cleanup;

	ret = hmgrtable_assign_handle_safe(&process->handle_table,
				NULL,
				HMGRENTRY_TYPE_MONITOREDFENCE,
				command->hwqueue_progress_fence);
	if (ret < 0)
		goto cleanup;

	hwqueue->handle = command->hwqueue;
	hwqueue->progress_fence_sync_object = command->hwqueue_progress_fence;

	hwqueue->progress_fence_mapped_address =
		dxg_map_iospace((u64)command->hwqueue_progress_fence_cpuva,
				PAGE_SIZE, PROT_READ | PROT_WRITE, true);
	if (hwqueue->progress_fence_mapped_address == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	ret = copy_to_user(&inargs->queue, &command->hwqueue,
			   sizeof(struct d3dkmthandle));
	if (ret) {
		pr_err("%s failed to copy hwqueue handle", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	ret = copy_to_user(&inargs->queue_progress_fence,
			   &command->hwqueue_progress_fence,
			   sizeof(struct d3dkmthandle));
	if (ret) {
		pr_err("%s failed to progress fence", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	ret = copy_to_user(&inargs->queue_progress_fence_cpu_va,
			   &hwqueue->progress_fence_mapped_address,
			   sizeof(inargs->queue_progress_fence_cpu_va));
	if (ret) {
		pr_err("%s failed to copy fence cpu va", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	ret = copy_to_user(&inargs->queue_progress_fence_gpu_va,
			   &command->hwqueue_progress_fence_gpuva,
			   sizeof(u64));
	if (ret) {
		pr_err("%s failed to copy fence gpu va", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	if (args->priv_drv_data_size) {
		ret = copy_to_user(args->priv_drv_data,
				   command->priv_drv_data,
				   args->priv_drv_data_size);
		if (ret) {
			pr_err("%s failed to copy private data", __func__);
			ret = -EINVAL;
		}
	}

cleanup:
	if (ret < 0) {
		pr_err("%s failed %x", __func__, ret);
		if (hwqueue->handle.v) {
			hmgrtable_free_handle_safe(&process->handle_table,
						   HMGRENTRY_TYPE_DXGHWQUEUE,
						   hwqueue->handle);
			hwqueue->handle.v = 0;
		}
		if (command && command->hwqueue.v)
			dxgvmb_send_destroy_hwqueue(process, adapter,
						    command->hwqueue);
	}
	free_message(&msg, process);
	return ret;
}

int dxgvmb_send_destroy_hwqueue(struct dxgprocess *process,
				struct dxgadapter *adapter,
				struct d3dkmthandle handle)
{
	int ret;
	struct dxgkvmb_command_destroyhwqueue *command;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_DESTROYHWQUEUE,
				   process->host_handle);
	command->hwqueue = handle;

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_query_adapter_info(struct dxgprocess *process,
				   struct dxgadapter *adapter,
				   struct d3dkmt_queryadapterinfo *args)
{
	struct dxgkvmb_command_queryadapterinfo *command;
	u32 cmd_size = sizeof(*command) + args->private_data_size - 1;
	int ret;
	u32 private_data_size;
	void *private_data;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	ret = copy_from_user(command->private_data,
			     args->private_data, args->private_data_size);
	if (ret) {
		pr_err("%s Faled to copy private data", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_QUERYADAPTERINFO,
				   process->host_handle);
	command->private_data_size = args->private_data_size;
	command->query_type = args->type;

	if (dxgglobal->vmbus_ver >= DXGK_VMBUS_INTERFACE_VERSION) {
		private_data = msg.msg;
		private_data_size = command->private_data_size +
				    sizeof(struct ntstatus);
	} else {
		private_data = command->private_data;
		private_data_size = command->private_data_size;
	}

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   private_data, private_data_size);
	if (ret < 0)
		goto cleanup;

	if (dxgglobal->vmbus_ver >= DXGK_VMBUS_INTERFACE_VERSION) {
		ret = ntstatus2int(*(struct ntstatus *)private_data);
		if (ret < 0)
			goto cleanup;
		private_data = (char *)private_data + sizeof(struct ntstatus);
	}

	switch (args->type) {
	case _KMTQAITYPE_ADAPTERTYPE:
	case _KMTQAITYPE_ADAPTERTYPE_RENDER:
		{
			struct d3dkmt_adaptertype *adapter_type =
			    (void *)private_data;
			adapter_type->paravirtualized = 1;
			adapter_type->display_supported = 0;
			adapter_type->post_device = 0;
			adapter_type->indirect_display_device = 0;
			adapter_type->acg_supported = 0;
			adapter_type->support_set_timings_from_vidpn = 0;
			break;
		}
	default:
		break;
	}
	ret = copy_to_user(args->private_data, private_data,
			   args->private_data_size);
	if (ret) {
		pr_err("%s Faled to copy private data to user", __func__);
		ret = -EINVAL;
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_submit_command_hwqueue(struct dxgprocess *process,
				       struct dxgadapter *adapter,
				       struct d3dkmt_submitcommandtohwqueue
				       *args)
{
	int ret = -EINVAL;
	u32 cmd_size;
	struct dxgkvmb_command_submitcommandtohwqueue *command;
	u32 primaries_size = args->num_primaries * sizeof(struct d3dkmthandle);
	struct dxgvmbusmsg msg = {.hdr = NULL};

	cmd_size = sizeof(*command) + args->priv_drv_data_size + primaries_size;
	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	if (primaries_size) {
		ret = copy_from_user(&command[1], args->written_primaries,
					 primaries_size);
		if (ret) {
			pr_err("%s failed to copy primaries handles", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}
	if (args->priv_drv_data_size) {
		ret = copy_from_user((char *)&command[1] + primaries_size,
				      args->priv_drv_data,
				      args->priv_drv_data_size);
		if (ret) {
			pr_err("%s failed to copy primaries data", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_SUBMITCOMMANDTOHWQUEUE,
				   process->host_handle);
	command->args = *args;

	if (dxgglobal->async_msg_enabled) {
		command->hdr.async_msg = 1;
		ret = dxgvmb_send_async_msg(msg.channel, msg.hdr, msg.size);
	} else {
		ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr,
						    msg.size);
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_query_statistics(struct dxgprocess *process,
				 struct dxgadapter *adapter,
				 struct d3dkmt_querystatistics *args)
{
	struct dxgkvmb_command_querystatistics *command;
	struct dxgkvmb_command_querystatistics_return *result;
	int ret;
	struct dxgvmbusmsgres msg = {.hdr = NULL};

	ret = init_message_res(&msg, adapter, process, sizeof(*command),
			       sizeof(*result));
	if (ret)
		goto cleanup;
	command = msg.msg;
	result = msg.res;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_QUERYSTATISTICS,
				   process->host_handle);
	command->args = *args;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   result, msg.res_size);
	if (ret < 0)
		goto cleanup;

	args->result = result->result;
	ret = ntstatus2int(result->status);

cleanup:
	free_message((struct dxgvmbusmsg *)&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}
