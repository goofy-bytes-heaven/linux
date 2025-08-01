// SPDX-License-Identifier: GPL-2.0-only
/*
 * Framework for buffer objects that can be shared across devices/subsystems.
 *
 * Copyright(C) 2011 Linaro Limited. All rights reserved.
 * Author: Sumit Semwal <sumit.semwal@ti.com>
 *
 * Many thanks to linaro-mm-sig list, and specially
 * Arnd Bergmann <arnd@arndb.de>, Rob Clark <rob@ti.com> and
 * Daniel Vetter <daniel@ffwll.ch> for their support in creation and
 * refining of this idea.
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/anon_inodes.h>
#include <linux/export.h>
#include <linux/debugfs.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/sync_file.h>
#include <linux/poll.h>
#include <linux/dma-resv.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>

#include <uapi/linux/dma-buf.h>
#include <uapi/linux/magic.h>

#include "dma-buf-sysfs-stats.h"

static inline int is_dma_buf_file(struct file *);

static DEFINE_MUTEX(dmabuf_list_mutex);
static LIST_HEAD(dmabuf_list);

static void __dma_buf_list_add(struct dma_buf *dmabuf)
{
	mutex_lock(&dmabuf_list_mutex);
	list_add(&dmabuf->list_node, &dmabuf_list);
	mutex_unlock(&dmabuf_list_mutex);
}

static void __dma_buf_list_del(struct dma_buf *dmabuf)
{
	if (!dmabuf)
		return;

	mutex_lock(&dmabuf_list_mutex);
	list_del(&dmabuf->list_node);
	mutex_unlock(&dmabuf_list_mutex);
}

/**
 * dma_buf_iter_begin - begin iteration through global list of all DMA buffers
 *
 * Returns the first buffer in the global list of DMA-bufs that's not in the
 * process of being destroyed. Increments that buffer's reference count to
 * prevent buffer destruction. Callers must release the reference, either by
 * continuing iteration with dma_buf_iter_next(), or with dma_buf_put().
 *
 * Return:
 * * First buffer from global list, with refcount elevated
 * * NULL if no active buffers are present
 */
struct dma_buf *dma_buf_iter_begin(void)
{
	struct dma_buf *ret = NULL, *dmabuf;

	/*
	 * The list mutex does not protect a dmabuf's refcount, so it can be
	 * zeroed while we are iterating. We cannot call get_dma_buf() since the
	 * caller may not already own a reference to the buffer.
	 */
	mutex_lock(&dmabuf_list_mutex);
	list_for_each_entry(dmabuf, &dmabuf_list, list_node) {
		if (file_ref_get(&dmabuf->file->f_ref)) {
			ret = dmabuf;
			break;
		}
	}
	mutex_unlock(&dmabuf_list_mutex);
	return ret;
}

/**
 * dma_buf_iter_next - continue iteration through global list of all DMA buffers
 * @dmabuf:	[in]	pointer to dma_buf
 *
 * Decrements the reference count on the provided buffer. Returns the next
 * buffer from the remainder of the global list of DMA-bufs with its reference
 * count incremented. Callers must release the reference, either by continuing
 * iteration with dma_buf_iter_next(), or with dma_buf_put().
 *
 * Return:
 * * Next buffer from global list, with refcount elevated
 * * NULL if no additional active buffers are present
 */
struct dma_buf *dma_buf_iter_next(struct dma_buf *dmabuf)
{
	struct dma_buf *ret = NULL;

	/*
	 * The list mutex does not protect a dmabuf's refcount, so it can be
	 * zeroed while we are iterating. We cannot call get_dma_buf() since the
	 * caller may not already own a reference to the buffer.
	 */
	mutex_lock(&dmabuf_list_mutex);
	dma_buf_put(dmabuf);
	list_for_each_entry_continue(dmabuf, &dmabuf_list, list_node) {
		if (file_ref_get(&dmabuf->file->f_ref)) {
			ret = dmabuf;
			break;
		}
	}
	mutex_unlock(&dmabuf_list_mutex);
	return ret;
}

static char *dmabuffs_dname(struct dentry *dentry, char *buffer, int buflen)
{
	struct dma_buf *dmabuf;
	char name[DMA_BUF_NAME_LEN];
	ssize_t ret = 0;

	dmabuf = dentry->d_fsdata;
	spin_lock(&dmabuf->name_lock);
	if (dmabuf->name)
		ret = strscpy(name, dmabuf->name, sizeof(name));
	spin_unlock(&dmabuf->name_lock);

	return dynamic_dname(buffer, buflen, "/%s:%s",
			     dentry->d_name.name, ret > 0 ? name : "");
}

static void dma_buf_release(struct dentry *dentry)
{
	struct dma_buf *dmabuf;

	dmabuf = dentry->d_fsdata;
	if (unlikely(!dmabuf))
		return;

	BUG_ON(dmabuf->vmapping_counter);

	/*
	 * If you hit this BUG() it could mean:
	 * * There's a file reference imbalance in dma_buf_poll / dma_buf_poll_cb or somewhere else
	 * * dmabuf->cb_in/out.active are non-0 despite no pending fence callback
	 */
	BUG_ON(dmabuf->cb_in.active || dmabuf->cb_out.active);

	dma_buf_stats_teardown(dmabuf);
	dmabuf->ops->release(dmabuf);

	if (dmabuf->resv == (struct dma_resv *)&dmabuf[1])
		dma_resv_fini(dmabuf->resv);

	WARN_ON(!list_empty(&dmabuf->attachments));
	module_put(dmabuf->owner);
	kfree(dmabuf->name);
	kfree(dmabuf);
}

static int dma_buf_file_release(struct inode *inode, struct file *file)
{
	if (!is_dma_buf_file(file))
		return -EINVAL;

	__dma_buf_list_del(file->private_data);

	return 0;
}

static const struct dentry_operations dma_buf_dentry_ops = {
	.d_dname = dmabuffs_dname,
	.d_release = dma_buf_release,
};

static struct vfsmount *dma_buf_mnt;

static int dma_buf_fs_init_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx;

	ctx = init_pseudo(fc, DMA_BUF_MAGIC);
	if (!ctx)
		return -ENOMEM;
	ctx->dops = &dma_buf_dentry_ops;
	return 0;
}

static struct file_system_type dma_buf_fs_type = {
	.name = "dmabuf",
	.init_fs_context = dma_buf_fs_init_context,
	.kill_sb = kill_anon_super,
};

static int dma_buf_mmap_internal(struct file *file, struct vm_area_struct *vma)
{
	struct dma_buf *dmabuf;

	if (!is_dma_buf_file(file))
		return -EINVAL;

	dmabuf = file->private_data;

	/* check if buffer supports mmap */
	if (!dmabuf->ops->mmap)
		return -EINVAL;

	/* check for overflowing the buffer's size */
	if (vma->vm_pgoff + vma_pages(vma) >
	    dmabuf->size >> PAGE_SHIFT)
		return -EINVAL;

	return dmabuf->ops->mmap(dmabuf, vma);
}

static loff_t dma_buf_llseek(struct file *file, loff_t offset, int whence)
{
	struct dma_buf *dmabuf;
	loff_t base;

	if (!is_dma_buf_file(file))
		return -EBADF;

	dmabuf = file->private_data;

	/* only support discovering the end of the buffer,
	 * but also allow SEEK_SET to maintain the idiomatic
	 * SEEK_END(0), SEEK_CUR(0) pattern.
	 */
	if (whence == SEEK_END)
		base = dmabuf->size;
	else if (whence == SEEK_SET)
		base = 0;
	else
		return -EINVAL;

	if (offset != 0)
		return -EINVAL;

	return base + offset;
}

/**
 * DOC: implicit fence polling
 *
 * To support cross-device and cross-driver synchronization of buffer access
 * implicit fences (represented internally in the kernel with &struct dma_fence)
 * can be attached to a &dma_buf. The glue for that and a few related things are
 * provided in the &dma_resv structure.
 *
 * Userspace can query the state of these implicitly tracked fences using poll()
 * and related system calls:
 *
 * - Checking for EPOLLIN, i.e. read access, can be use to query the state of the
 *   most recent write or exclusive fence.
 *
 * - Checking for EPOLLOUT, i.e. write access, can be used to query the state of
 *   all attached fences, shared and exclusive ones.
 *
 * Note that this only signals the completion of the respective fences, i.e. the
 * DMA transfers are complete. Cache flushing and any other necessary
 * preparations before CPU access can begin still need to happen.
 *
 * As an alternative to poll(), the set of fences on DMA buffer can be
 * exported as a &sync_file using &dma_buf_sync_file_export.
 */

static void dma_buf_poll_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct dma_buf_poll_cb_t *dcb = (struct dma_buf_poll_cb_t *)cb;
	struct dma_buf *dmabuf = container_of(dcb->poll, struct dma_buf, poll);
	unsigned long flags;

	spin_lock_irqsave(&dcb->poll->lock, flags);
	wake_up_locked_poll(dcb->poll, dcb->active);
	dcb->active = 0;
	spin_unlock_irqrestore(&dcb->poll->lock, flags);
	dma_fence_put(fence);
	/* Paired with get_file in dma_buf_poll */
	fput(dmabuf->file);
}

static bool dma_buf_poll_add_cb(struct dma_resv *resv, bool write,
				struct dma_buf_poll_cb_t *dcb)
{
	struct dma_resv_iter cursor;
	struct dma_fence *fence;
	int r;

	dma_resv_for_each_fence(&cursor, resv, dma_resv_usage_rw(write),
				fence) {
		dma_fence_get(fence);
		r = dma_fence_add_callback(fence, &dcb->cb, dma_buf_poll_cb);
		if (!r)
			return true;
		dma_fence_put(fence);
	}

	return false;
}

static __poll_t dma_buf_poll(struct file *file, poll_table *poll)
{
	struct dma_buf *dmabuf;
	struct dma_resv *resv;
	__poll_t events;

	dmabuf = file->private_data;
	if (!dmabuf || !dmabuf->resv)
		return EPOLLERR;

	resv = dmabuf->resv;

	poll_wait(file, &dmabuf->poll, poll);

	events = poll_requested_events(poll) & (EPOLLIN | EPOLLOUT);
	if (!events)
		return 0;

	dma_resv_lock(resv, NULL);

	if (events & EPOLLOUT) {
		struct dma_buf_poll_cb_t *dcb = &dmabuf->cb_out;

		/* Check that callback isn't busy */
		spin_lock_irq(&dmabuf->poll.lock);
		if (dcb->active)
			events &= ~EPOLLOUT;
		else
			dcb->active = EPOLLOUT;
		spin_unlock_irq(&dmabuf->poll.lock);

		if (events & EPOLLOUT) {
			/* Paired with fput in dma_buf_poll_cb */
			get_file(dmabuf->file);

			if (!dma_buf_poll_add_cb(resv, true, dcb))
				/* No callback queued, wake up any other waiters */
				dma_buf_poll_cb(NULL, &dcb->cb);
			else
				events &= ~EPOLLOUT;
		}
	}

	if (events & EPOLLIN) {
		struct dma_buf_poll_cb_t *dcb = &dmabuf->cb_in;

		/* Check that callback isn't busy */
		spin_lock_irq(&dmabuf->poll.lock);
		if (dcb->active)
			events &= ~EPOLLIN;
		else
			dcb->active = EPOLLIN;
		spin_unlock_irq(&dmabuf->poll.lock);

		if (events & EPOLLIN) {
			/* Paired with fput in dma_buf_poll_cb */
			get_file(dmabuf->file);

			if (!dma_buf_poll_add_cb(resv, false, dcb))
				/* No callback queued, wake up any other waiters */
				dma_buf_poll_cb(NULL, &dcb->cb);
			else
				events &= ~EPOLLIN;
		}
	}

	dma_resv_unlock(resv);
	return events;
}

/**
 * dma_buf_set_name - Set a name to a specific dma_buf to track the usage.
 * It could support changing the name of the dma-buf if the same
 * piece of memory is used for multiple purpose between different devices.
 *
 * @dmabuf: [in]     dmabuf buffer that will be renamed.
 * @buf:    [in]     A piece of userspace memory that contains the name of
 *                   the dma-buf.
 *
 * Returns 0 on success. If the dma-buf buffer is already attached to
 * devices, return -EBUSY.
 *
 */
static long dma_buf_set_name(struct dma_buf *dmabuf, const char __user *buf)
{
	char *name = strndup_user(buf, DMA_BUF_NAME_LEN);

	if (IS_ERR(name))
		return PTR_ERR(name);

	spin_lock(&dmabuf->name_lock);
	kfree(dmabuf->name);
	dmabuf->name = name;
	spin_unlock(&dmabuf->name_lock);

	return 0;
}

#if IS_ENABLED(CONFIG_SYNC_FILE)
static long dma_buf_export_sync_file(struct dma_buf *dmabuf,
				     void __user *user_data)
{
	struct dma_buf_export_sync_file arg;
	enum dma_resv_usage usage;
	struct dma_fence *fence = NULL;
	struct sync_file *sync_file;
	int fd, ret;

	if (copy_from_user(&arg, user_data, sizeof(arg)))
		return -EFAULT;

	if (arg.flags & ~DMA_BUF_SYNC_RW)
		return -EINVAL;

	if ((arg.flags & DMA_BUF_SYNC_RW) == 0)
		return -EINVAL;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	usage = dma_resv_usage_rw(arg.flags & DMA_BUF_SYNC_WRITE);
	ret = dma_resv_get_singleton(dmabuf->resv, usage, &fence);
	if (ret)
		goto err_put_fd;

	if (!fence)
		fence = dma_fence_get_stub();

	sync_file = sync_file_create(fence);

	dma_fence_put(fence);

	if (!sync_file) {
		ret = -ENOMEM;
		goto err_put_fd;
	}

	arg.fd = fd;
	if (copy_to_user(user_data, &arg, sizeof(arg))) {
		ret = -EFAULT;
		goto err_put_file;
	}

	fd_install(fd, sync_file->file);

	return 0;

err_put_file:
	fput(sync_file->file);
err_put_fd:
	put_unused_fd(fd);
	return ret;
}

static long dma_buf_import_sync_file(struct dma_buf *dmabuf,
				     const void __user *user_data)
{
	struct dma_buf_import_sync_file arg;
	struct dma_fence *fence, *f;
	enum dma_resv_usage usage;
	struct dma_fence_unwrap iter;
	unsigned int num_fences;
	int ret = 0;

	if (copy_from_user(&arg, user_data, sizeof(arg)))
		return -EFAULT;

	if (arg.flags & ~DMA_BUF_SYNC_RW)
		return -EINVAL;

	if ((arg.flags & DMA_BUF_SYNC_RW) == 0)
		return -EINVAL;

	fence = sync_file_get_fence(arg.fd);
	if (!fence)
		return -EINVAL;

	usage = (arg.flags & DMA_BUF_SYNC_WRITE) ? DMA_RESV_USAGE_WRITE :
						   DMA_RESV_USAGE_READ;

	num_fences = 0;
	dma_fence_unwrap_for_each(f, &iter, fence)
		++num_fences;

	if (num_fences > 0) {
		dma_resv_lock(dmabuf->resv, NULL);

		ret = dma_resv_reserve_fences(dmabuf->resv, num_fences);
		if (!ret) {
			dma_fence_unwrap_for_each(f, &iter, fence)
				dma_resv_add_fence(dmabuf->resv, f, usage);
		}

		dma_resv_unlock(dmabuf->resv);
	}

	dma_fence_put(fence);

	return ret;
}
#endif

static long dma_buf_ioctl(struct file *file,
			  unsigned int cmd, unsigned long arg)
{
	struct dma_buf *dmabuf;
	struct dma_buf_sync sync;
	enum dma_data_direction direction;
	int ret;

	dmabuf = file->private_data;

	switch (cmd) {
	case DMA_BUF_IOCTL_SYNC:
		if (copy_from_user(&sync, (void __user *) arg, sizeof(sync)))
			return -EFAULT;

		if (sync.flags & ~DMA_BUF_SYNC_VALID_FLAGS_MASK)
			return -EINVAL;

		switch (sync.flags & DMA_BUF_SYNC_RW) {
		case DMA_BUF_SYNC_READ:
			direction = DMA_FROM_DEVICE;
			break;
		case DMA_BUF_SYNC_WRITE:
			direction = DMA_TO_DEVICE;
			break;
		case DMA_BUF_SYNC_RW:
			direction = DMA_BIDIRECTIONAL;
			break;
		default:
			return -EINVAL;
		}

		if (sync.flags & DMA_BUF_SYNC_END)
			ret = dma_buf_end_cpu_access(dmabuf, direction);
		else
			ret = dma_buf_begin_cpu_access(dmabuf, direction);

		return ret;

	case DMA_BUF_SET_NAME_A:
	case DMA_BUF_SET_NAME_B:
		return dma_buf_set_name(dmabuf, (const char __user *)arg);

#if IS_ENABLED(CONFIG_SYNC_FILE)
	case DMA_BUF_IOCTL_EXPORT_SYNC_FILE:
		return dma_buf_export_sync_file(dmabuf, (void __user *)arg);
	case DMA_BUF_IOCTL_IMPORT_SYNC_FILE:
		return dma_buf_import_sync_file(dmabuf, (const void __user *)arg);
#endif

	default:
		return -ENOTTY;
	}
}

static void dma_buf_show_fdinfo(struct seq_file *m, struct file *file)
{
	struct dma_buf *dmabuf = file->private_data;

	seq_printf(m, "size:\t%zu\n", dmabuf->size);
	/* Don't count the temporary reference taken inside procfs seq_show */
	seq_printf(m, "count:\t%ld\n", file_count(dmabuf->file) - 1);
	seq_printf(m, "exp_name:\t%s\n", dmabuf->exp_name);
	spin_lock(&dmabuf->name_lock);
	if (dmabuf->name)
		seq_printf(m, "name:\t%s\n", dmabuf->name);
	spin_unlock(&dmabuf->name_lock);
}

static const struct file_operations dma_buf_fops = {
	.release	= dma_buf_file_release,
	.mmap		= dma_buf_mmap_internal,
	.llseek		= dma_buf_llseek,
	.poll		= dma_buf_poll,
	.unlocked_ioctl	= dma_buf_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.show_fdinfo	= dma_buf_show_fdinfo,
};

/*
 * is_dma_buf_file - Check if struct file* is associated with dma_buf
 */
static inline int is_dma_buf_file(struct file *file)
{
	return file->f_op == &dma_buf_fops;
}

static struct file *dma_buf_getfile(size_t size, int flags)
{
	static atomic64_t dmabuf_inode = ATOMIC64_INIT(0);
	struct inode *inode = alloc_anon_inode(dma_buf_mnt->mnt_sb);
	struct file *file;

	if (IS_ERR(inode))
		return ERR_CAST(inode);

	inode->i_size = size;
	inode_set_bytes(inode, size);

	/*
	 * The ->i_ino acquired from get_next_ino() is not unique thus
	 * not suitable for using it as dentry name by dmabuf stats.
	 * Override ->i_ino with the unique and dmabuffs specific
	 * value.
	 */
	inode->i_ino = atomic64_inc_return(&dmabuf_inode);
	flags &= O_ACCMODE | O_NONBLOCK;
	file = alloc_file_pseudo(inode, dma_buf_mnt, "dmabuf",
				 flags, &dma_buf_fops);
	if (IS_ERR(file))
		goto err_alloc_file;

	return file;

err_alloc_file:
	iput(inode);
	return file;
}

/**
 * DOC: dma buf device access
 *
 * For device DMA access to a shared DMA buffer the usual sequence of operations
 * is fairly simple:
 *
 * 1. The exporter defines his exporter instance using
 *    DEFINE_DMA_BUF_EXPORT_INFO() and calls dma_buf_export() to wrap a private
 *    buffer object into a &dma_buf. It then exports that &dma_buf to userspace
 *    as a file descriptor by calling dma_buf_fd().
 *
 * 2. Userspace passes this file-descriptors to all drivers it wants this buffer
 *    to share with: First the file descriptor is converted to a &dma_buf using
 *    dma_buf_get(). Then the buffer is attached to the device using
 *    dma_buf_attach().
 *
 *    Up to this stage the exporter is still free to migrate or reallocate the
 *    backing storage.
 *
 * 3. Once the buffer is attached to all devices userspace can initiate DMA
 *    access to the shared buffer. In the kernel this is done by calling
 *    dma_buf_map_attachment() and dma_buf_unmap_attachment().
 *
 * 4. Once a driver is done with a shared buffer it needs to call
 *    dma_buf_detach() (after cleaning up any mappings) and then release the
 *    reference acquired with dma_buf_get() by calling dma_buf_put().
 *
 * For the detailed semantics exporters are expected to implement see
 * &dma_buf_ops.
 */

/**
 * dma_buf_export - Creates a new dma_buf, and associates an anon file
 * with this buffer, so it can be exported.
 * Also connect the allocator specific data and ops to the buffer.
 * Additionally, provide a name string for exporter; useful in debugging.
 *
 * @exp_info:	[in]	holds all the export related information provided
 *			by the exporter. see &struct dma_buf_export_info
 *			for further details.
 *
 * Returns, on success, a newly created struct dma_buf object, which wraps the
 * supplied private data and operations for struct dma_buf_ops. On either
 * missing ops, or error in allocating struct dma_buf, will return negative
 * error.
 *
 * For most cases the easiest way to create @exp_info is through the
 * %DEFINE_DMA_BUF_EXPORT_INFO macro.
 */
struct dma_buf *dma_buf_export(const struct dma_buf_export_info *exp_info)
{
	struct dma_buf *dmabuf;
	struct dma_resv *resv = exp_info->resv;
	struct file *file;
	size_t alloc_size = sizeof(struct dma_buf);
	int ret;

	if (WARN_ON(!exp_info->priv || !exp_info->ops
		    || !exp_info->ops->map_dma_buf
		    || !exp_info->ops->unmap_dma_buf
		    || !exp_info->ops->release))
		return ERR_PTR(-EINVAL);

	if (WARN_ON(!exp_info->ops->pin != !exp_info->ops->unpin))
		return ERR_PTR(-EINVAL);

	if (!try_module_get(exp_info->owner))
		return ERR_PTR(-ENOENT);

	file = dma_buf_getfile(exp_info->size, exp_info->flags);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err_module;
	}

	if (!exp_info->resv)
		alloc_size += sizeof(struct dma_resv);
	else
		/* prevent &dma_buf[1] == dma_buf->resv */
		alloc_size += 1;
	dmabuf = kzalloc(alloc_size, GFP_KERNEL);
	if (!dmabuf) {
		ret = -ENOMEM;
		goto err_file;
	}

	dmabuf->priv = exp_info->priv;
	dmabuf->ops = exp_info->ops;
	dmabuf->size = exp_info->size;
	dmabuf->exp_name = exp_info->exp_name;
	dmabuf->owner = exp_info->owner;
	spin_lock_init(&dmabuf->name_lock);
	init_waitqueue_head(&dmabuf->poll);
	dmabuf->cb_in.poll = dmabuf->cb_out.poll = &dmabuf->poll;
	dmabuf->cb_in.active = dmabuf->cb_out.active = 0;
	INIT_LIST_HEAD(&dmabuf->attachments);

	if (!resv) {
		dmabuf->resv = (struct dma_resv *)&dmabuf[1];
		dma_resv_init(dmabuf->resv);
	} else {
		dmabuf->resv = resv;
	}

	ret = dma_buf_stats_setup(dmabuf, file);
	if (ret)
		goto err_dmabuf;

	file->private_data = dmabuf;
	file->f_path.dentry->d_fsdata = dmabuf;
	dmabuf->file = file;

	__dma_buf_list_add(dmabuf);

	return dmabuf;

err_dmabuf:
	if (!resv)
		dma_resv_fini(dmabuf->resv);
	kfree(dmabuf);
err_file:
	fput(file);
err_module:
	module_put(exp_info->owner);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_export, "DMA_BUF");

/**
 * dma_buf_fd - returns a file descriptor for the given struct dma_buf
 * @dmabuf:	[in]	pointer to dma_buf for which fd is required.
 * @flags:      [in]    flags to give to fd
 *
 * On success, returns an associated 'fd'. Else, returns error.
 */
int dma_buf_fd(struct dma_buf *dmabuf, int flags)
{
	int fd;

	if (!dmabuf || !dmabuf->file)
		return -EINVAL;

	fd = get_unused_fd_flags(flags);
	if (fd < 0)
		return fd;

	fd_install(fd, dmabuf->file);

	return fd;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_fd, "DMA_BUF");

/**
 * dma_buf_get - returns the struct dma_buf related to an fd
 * @fd:	[in]	fd associated with the struct dma_buf to be returned
 *
 * On success, returns the struct dma_buf associated with an fd; uses
 * file's refcounting done by fget to increase refcount. returns ERR_PTR
 * otherwise.
 */
struct dma_buf *dma_buf_get(int fd)
{
	struct file *file;

	file = fget(fd);

	if (!file)
		return ERR_PTR(-EBADF);

	if (!is_dma_buf_file(file)) {
		fput(file);
		return ERR_PTR(-EINVAL);
	}

	return file->private_data;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_get, "DMA_BUF");

/**
 * dma_buf_put - decreases refcount of the buffer
 * @dmabuf:	[in]	buffer to reduce refcount of
 *
 * Uses file's refcounting done implicitly by fput().
 *
 * If, as a result of this call, the refcount becomes 0, the 'release' file
 * operation related to this fd is called. It calls &dma_buf_ops.release vfunc
 * in turn, and frees the memory allocated for dmabuf when exported.
 */
void dma_buf_put(struct dma_buf *dmabuf)
{
	if (WARN_ON(!dmabuf || !dmabuf->file))
		return;

	fput(dmabuf->file);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_put, "DMA_BUF");

static void mangle_sg_table(struct sg_table *sg_table)
{
#ifdef CONFIG_DMABUF_DEBUG
	int i;
	struct scatterlist *sg;

	/* To catch abuse of the underlying struct page by importers mix
	 * up the bits, but take care to preserve the low SG_ bits to
	 * not corrupt the sgt. The mixing is undone on unmap
	 * before passing the sgt back to the exporter.
	 */
	for_each_sgtable_sg(sg_table, sg, i)
		sg->page_link ^= ~0xffUL;
#endif

}

static inline bool
dma_buf_attachment_is_dynamic(struct dma_buf_attachment *attach)
{
	return !!attach->importer_ops;
}

static bool
dma_buf_pin_on_map(struct dma_buf_attachment *attach)
{
	return attach->dmabuf->ops->pin &&
		(!dma_buf_attachment_is_dynamic(attach) ||
		 !IS_ENABLED(CONFIG_DMABUF_MOVE_NOTIFY));
}

/**
 * DOC: locking convention
 *
 * In order to avoid deadlock situations between dma-buf exports and importers,
 * all dma-buf API users must follow the common dma-buf locking convention.
 *
 * Convention for importers
 *
 * 1. Importers must hold the dma-buf reservation lock when calling these
 *    functions:
 *
 *     - dma_buf_pin()
 *     - dma_buf_unpin()
 *     - dma_buf_map_attachment()
 *     - dma_buf_unmap_attachment()
 *     - dma_buf_vmap()
 *     - dma_buf_vunmap()
 *
 * 2. Importers must not hold the dma-buf reservation lock when calling these
 *    functions:
 *
 *     - dma_buf_attach()
 *     - dma_buf_dynamic_attach()
 *     - dma_buf_detach()
 *     - dma_buf_export()
 *     - dma_buf_fd()
 *     - dma_buf_get()
 *     - dma_buf_put()
 *     - dma_buf_mmap()
 *     - dma_buf_begin_cpu_access()
 *     - dma_buf_end_cpu_access()
 *     - dma_buf_map_attachment_unlocked()
 *     - dma_buf_unmap_attachment_unlocked()
 *     - dma_buf_vmap_unlocked()
 *     - dma_buf_vunmap_unlocked()
 *
 * Convention for exporters
 *
 * 1. These &dma_buf_ops callbacks are invoked with unlocked dma-buf
 *    reservation and exporter can take the lock:
 *
 *     - &dma_buf_ops.attach()
 *     - &dma_buf_ops.detach()
 *     - &dma_buf_ops.release()
 *     - &dma_buf_ops.begin_cpu_access()
 *     - &dma_buf_ops.end_cpu_access()
 *     - &dma_buf_ops.mmap()
 *
 * 2. These &dma_buf_ops callbacks are invoked with locked dma-buf
 *    reservation and exporter can't take the lock:
 *
 *     - &dma_buf_ops.pin()
 *     - &dma_buf_ops.unpin()
 *     - &dma_buf_ops.map_dma_buf()
 *     - &dma_buf_ops.unmap_dma_buf()
 *     - &dma_buf_ops.vmap()
 *     - &dma_buf_ops.vunmap()
 *
 * 3. Exporters must hold the dma-buf reservation lock when calling these
 *    functions:
 *
 *     - dma_buf_move_notify()
 */

/**
 * dma_buf_dynamic_attach - Add the device to dma_buf's attachments list
 * @dmabuf:		[in]	buffer to attach device to.
 * @dev:		[in]	device to be attached.
 * @importer_ops:	[in]	importer operations for the attachment
 * @importer_priv:	[in]	importer private pointer for the attachment
 *
 * Returns struct dma_buf_attachment pointer for this attachment. Attachments
 * must be cleaned up by calling dma_buf_detach().
 *
 * Optionally this calls &dma_buf_ops.attach to allow device-specific attach
 * functionality.
 *
 * Returns:
 *
 * A pointer to newly created &dma_buf_attachment on success, or a negative
 * error code wrapped into a pointer on failure.
 *
 * Note that this can fail if the backing storage of @dmabuf is in a place not
 * accessible to @dev, and cannot be moved to a more suitable place. This is
 * indicated with the error code -EBUSY.
 */
struct dma_buf_attachment *
dma_buf_dynamic_attach(struct dma_buf *dmabuf, struct device *dev,
		       const struct dma_buf_attach_ops *importer_ops,
		       void *importer_priv)
{
	struct dma_buf_attachment *attach;
	int ret;

	if (WARN_ON(!dmabuf || !dev))
		return ERR_PTR(-EINVAL);

	if (WARN_ON(importer_ops && !importer_ops->move_notify))
		return ERR_PTR(-EINVAL);

	attach = kzalloc(sizeof(*attach), GFP_KERNEL);
	if (!attach)
		return ERR_PTR(-ENOMEM);

	attach->dev = dev;
	attach->dmabuf = dmabuf;
	if (importer_ops)
		attach->peer2peer = importer_ops->allow_peer2peer;
	attach->importer_ops = importer_ops;
	attach->importer_priv = importer_priv;

	if (dmabuf->ops->attach) {
		ret = dmabuf->ops->attach(dmabuf, attach);
		if (ret)
			goto err_attach;
	}
	dma_resv_lock(dmabuf->resv, NULL);
	list_add(&attach->node, &dmabuf->attachments);
	dma_resv_unlock(dmabuf->resv);

	return attach;

err_attach:
	kfree(attach);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_dynamic_attach, "DMA_BUF");

/**
 * dma_buf_attach - Wrapper for dma_buf_dynamic_attach
 * @dmabuf:	[in]	buffer to attach device to.
 * @dev:	[in]	device to be attached.
 *
 * Wrapper to call dma_buf_dynamic_attach() for drivers which still use a static
 * mapping.
 */
struct dma_buf_attachment *dma_buf_attach(struct dma_buf *dmabuf,
					  struct device *dev)
{
	return dma_buf_dynamic_attach(dmabuf, dev, NULL, NULL);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_attach, "DMA_BUF");

/**
 * dma_buf_detach - Remove the given attachment from dmabuf's attachments list
 * @dmabuf:	[in]	buffer to detach from.
 * @attach:	[in]	attachment to be detached; is free'd after this call.
 *
 * Clean up a device attachment obtained by calling dma_buf_attach().
 *
 * Optionally this calls &dma_buf_ops.detach for device-specific detach.
 */
void dma_buf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
	if (WARN_ON(!dmabuf || !attach || dmabuf != attach->dmabuf))
		return;

	dma_resv_lock(dmabuf->resv, NULL);
	list_del(&attach->node);
	dma_resv_unlock(dmabuf->resv);

	if (dmabuf->ops->detach)
		dmabuf->ops->detach(dmabuf, attach);

	kfree(attach);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_detach, "DMA_BUF");

/**
 * dma_buf_pin - Lock down the DMA-buf
 * @attach:	[in]	attachment which should be pinned
 *
 * Only dynamic importers (who set up @attach with dma_buf_dynamic_attach()) may
 * call this, and only for limited use cases like scanout and not for temporary
 * pin operations. It is not permitted to allow userspace to pin arbitrary
 * amounts of buffers through this interface.
 *
 * Buffers must be unpinned by calling dma_buf_unpin().
 *
 * Returns:
 * 0 on success, negative error code on failure.
 */
int dma_buf_pin(struct dma_buf_attachment *attach)
{
	struct dma_buf *dmabuf = attach->dmabuf;
	int ret = 0;

	WARN_ON(!attach->importer_ops);

	dma_resv_assert_held(dmabuf->resv);

	if (dmabuf->ops->pin)
		ret = dmabuf->ops->pin(attach);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_pin, "DMA_BUF");

/**
 * dma_buf_unpin - Unpin a DMA-buf
 * @attach:	[in]	attachment which should be unpinned
 *
 * This unpins a buffer pinned by dma_buf_pin() and allows the exporter to move
 * any mapping of @attach again and inform the importer through
 * &dma_buf_attach_ops.move_notify.
 */
void dma_buf_unpin(struct dma_buf_attachment *attach)
{
	struct dma_buf *dmabuf = attach->dmabuf;

	WARN_ON(!attach->importer_ops);

	dma_resv_assert_held(dmabuf->resv);

	if (dmabuf->ops->unpin)
		dmabuf->ops->unpin(attach);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_unpin, "DMA_BUF");

/**
 * dma_buf_map_attachment - Returns the scatterlist table of the attachment;
 * mapped into _device_ address space. Is a wrapper for map_dma_buf() of the
 * dma_buf_ops.
 * @attach:	[in]	attachment whose scatterlist is to be returned
 * @direction:	[in]	direction of DMA transfer
 *
 * Returns sg_table containing the scatterlist to be returned; returns ERR_PTR
 * on error. May return -EINTR if it is interrupted by a signal.
 *
 * On success, the DMA addresses and lengths in the returned scatterlist are
 * PAGE_SIZE aligned.
 *
 * A mapping must be unmapped by using dma_buf_unmap_attachment(). Note that
 * the underlying backing storage is pinned for as long as a mapping exists,
 * therefore users/importers should not hold onto a mapping for undue amounts of
 * time.
 *
 * Important: Dynamic importers must wait for the exclusive fence of the struct
 * dma_resv attached to the DMA-BUF first.
 */
struct sg_table *dma_buf_map_attachment(struct dma_buf_attachment *attach,
					enum dma_data_direction direction)
{
	struct sg_table *sg_table;
	signed long ret;

	might_sleep();

	if (WARN_ON(!attach || !attach->dmabuf))
		return ERR_PTR(-EINVAL);

	dma_resv_assert_held(attach->dmabuf->resv);

	if (dma_buf_pin_on_map(attach)) {
		ret = attach->dmabuf->ops->pin(attach);
		/*
		 * Catch exporters making buffers inaccessible even when
		 * attachments preventing that exist.
		 */
		WARN_ON_ONCE(ret == -EBUSY);
		if (ret)
			return ERR_PTR(ret);
	}

	sg_table = attach->dmabuf->ops->map_dma_buf(attach, direction);
	if (!sg_table)
		sg_table = ERR_PTR(-ENOMEM);
	if (IS_ERR(sg_table))
		goto error_unpin;

	/*
	 * Importers with static attachments don't wait for fences.
	 */
	if (!dma_buf_attachment_is_dynamic(attach)) {
		ret = dma_resv_wait_timeout(attach->dmabuf->resv,
					    DMA_RESV_USAGE_KERNEL, true,
					    MAX_SCHEDULE_TIMEOUT);
		if (ret < 0)
			goto error_unmap;
	}
	mangle_sg_table(sg_table);

#ifdef CONFIG_DMA_API_DEBUG
	{
		struct scatterlist *sg;
		u64 addr;
		int len;
		int i;

		for_each_sgtable_dma_sg(sg_table, sg, i) {
			addr = sg_dma_address(sg);
			len = sg_dma_len(sg);
			if (!PAGE_ALIGNED(addr) || !PAGE_ALIGNED(len)) {
				pr_debug("%s: addr %llx or len %x is not page aligned!\n",
					 __func__, addr, len);
			}
		}
	}
#endif /* CONFIG_DMA_API_DEBUG */
	return sg_table;

error_unmap:
	attach->dmabuf->ops->unmap_dma_buf(attach, sg_table, direction);
	sg_table = ERR_PTR(ret);

error_unpin:
	if (dma_buf_pin_on_map(attach))
		attach->dmabuf->ops->unpin(attach);

	return sg_table;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_map_attachment, "DMA_BUF");

/**
 * dma_buf_map_attachment_unlocked - Returns the scatterlist table of the attachment;
 * mapped into _device_ address space. Is a wrapper for map_dma_buf() of the
 * dma_buf_ops.
 * @attach:	[in]	attachment whose scatterlist is to be returned
 * @direction:	[in]	direction of DMA transfer
 *
 * Unlocked variant of dma_buf_map_attachment().
 */
struct sg_table *
dma_buf_map_attachment_unlocked(struct dma_buf_attachment *attach,
				enum dma_data_direction direction)
{
	struct sg_table *sg_table;

	might_sleep();

	if (WARN_ON(!attach || !attach->dmabuf))
		return ERR_PTR(-EINVAL);

	dma_resv_lock(attach->dmabuf->resv, NULL);
	sg_table = dma_buf_map_attachment(attach, direction);
	dma_resv_unlock(attach->dmabuf->resv);

	return sg_table;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_map_attachment_unlocked, "DMA_BUF");

/**
 * dma_buf_unmap_attachment - unmaps and decreases usecount of the buffer;might
 * deallocate the scatterlist associated. Is a wrapper for unmap_dma_buf() of
 * dma_buf_ops.
 * @attach:	[in]	attachment to unmap buffer from
 * @sg_table:	[in]	scatterlist info of the buffer to unmap
 * @direction:  [in]    direction of DMA transfer
 *
 * This unmaps a DMA mapping for @attached obtained by dma_buf_map_attachment().
 */
void dma_buf_unmap_attachment(struct dma_buf_attachment *attach,
				struct sg_table *sg_table,
				enum dma_data_direction direction)
{
	might_sleep();

	if (WARN_ON(!attach || !attach->dmabuf || !sg_table))
		return;

	dma_resv_assert_held(attach->dmabuf->resv);

	mangle_sg_table(sg_table);
	attach->dmabuf->ops->unmap_dma_buf(attach, sg_table, direction);

	if (dma_buf_pin_on_map(attach))
		attach->dmabuf->ops->unpin(attach);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_unmap_attachment, "DMA_BUF");

/**
 * dma_buf_unmap_attachment_unlocked - unmaps and decreases usecount of the buffer;might
 * deallocate the scatterlist associated. Is a wrapper for unmap_dma_buf() of
 * dma_buf_ops.
 * @attach:	[in]	attachment to unmap buffer from
 * @sg_table:	[in]	scatterlist info of the buffer to unmap
 * @direction:	[in]	direction of DMA transfer
 *
 * Unlocked variant of dma_buf_unmap_attachment().
 */
void dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *attach,
				       struct sg_table *sg_table,
				       enum dma_data_direction direction)
{
	might_sleep();

	if (WARN_ON(!attach || !attach->dmabuf || !sg_table))
		return;

	dma_resv_lock(attach->dmabuf->resv, NULL);
	dma_buf_unmap_attachment(attach, sg_table, direction);
	dma_resv_unlock(attach->dmabuf->resv);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_unmap_attachment_unlocked, "DMA_BUF");

/**
 * dma_buf_move_notify - notify attachments that DMA-buf is moving
 *
 * @dmabuf:	[in]	buffer which is moving
 *
 * Informs all attachments that they need to destroy and recreate all their
 * mappings.
 */
void dma_buf_move_notify(struct dma_buf *dmabuf)
{
	struct dma_buf_attachment *attach;

	dma_resv_assert_held(dmabuf->resv);

	list_for_each_entry(attach, &dmabuf->attachments, node)
		if (attach->importer_ops)
			attach->importer_ops->move_notify(attach);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_move_notify, "DMA_BUF");

/**
 * DOC: cpu access
 *
 * There are multiple reasons for supporting CPU access to a dma buffer object:
 *
 * - Fallback operations in the kernel, for example when a device is connected
 *   over USB and the kernel needs to shuffle the data around first before
 *   sending it away. Cache coherency is handled by bracketing any transactions
 *   with calls to dma_buf_begin_cpu_access() and dma_buf_end_cpu_access()
 *   access.
 *
 *   Since for most kernel internal dma-buf accesses need the entire buffer, a
 *   vmap interface is introduced. Note that on very old 32-bit architectures
 *   vmalloc space might be limited and result in vmap calls failing.
 *
 *   Interfaces:
 *
 *   .. code-block:: c
 *
 *     void *dma_buf_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
 *     void dma_buf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
 *
 *   The vmap call can fail if there is no vmap support in the exporter, or if
 *   it runs out of vmalloc space. Note that the dma-buf layer keeps a reference
 *   count for all vmap access and calls down into the exporter's vmap function
 *   only when no vmapping exists, and only unmaps it once. Protection against
 *   concurrent vmap/vunmap calls is provided by taking the &dma_buf.lock mutex.
 *
 * - For full compatibility on the importer side with existing userspace
 *   interfaces, which might already support mmap'ing buffers. This is needed in
 *   many processing pipelines (e.g. feeding a software rendered image into a
 *   hardware pipeline, thumbnail creation, snapshots, ...). Also, Android's ION
 *   framework already supported this and for DMA buffer file descriptors to
 *   replace ION buffers mmap support was needed.
 *
 *   There is no special interfaces, userspace simply calls mmap on the dma-buf
 *   fd. But like for CPU access there's a need to bracket the actual access,
 *   which is handled by the ioctl (DMA_BUF_IOCTL_SYNC). Note that
 *   DMA_BUF_IOCTL_SYNC can fail with -EAGAIN or -EINTR, in which case it must
 *   be restarted.
 *
 *   Some systems might need some sort of cache coherency management e.g. when
 *   CPU and GPU domains are being accessed through dma-buf at the same time.
 *   To circumvent this problem there are begin/end coherency markers, that
 *   forward directly to existing dma-buf device drivers vfunc hooks. Userspace
 *   can make use of those markers through the DMA_BUF_IOCTL_SYNC ioctl. The
 *   sequence would be used like following:
 *
 *     - mmap dma-buf fd
 *     - for each drawing/upload cycle in CPU 1. SYNC_START ioctl, 2. read/write
 *       to mmap area 3. SYNC_END ioctl. This can be repeated as often as you
 *       want (with the new data being consumed by say the GPU or the scanout
 *       device)
 *     - munmap once you don't need the buffer any more
 *
 *    For correctness and optimal performance, it is always required to use
 *    SYNC_START and SYNC_END before and after, respectively, when accessing the
 *    mapped address. Userspace cannot rely on coherent access, even when there
 *    are systems where it just works without calling these ioctls.
 *
 * - And as a CPU fallback in userspace processing pipelines.
 *
 *   Similar to the motivation for kernel cpu access it is again important that
 *   the userspace code of a given importing subsystem can use the same
 *   interfaces with a imported dma-buf buffer object as with a native buffer
 *   object. This is especially important for drm where the userspace part of
 *   contemporary OpenGL, X, and other drivers is huge, and reworking them to
 *   use a different way to mmap a buffer rather invasive.
 *
 *   The assumption in the current dma-buf interfaces is that redirecting the
 *   initial mmap is all that's needed. A survey of some of the existing
 *   subsystems shows that no driver seems to do any nefarious thing like
 *   syncing up with outstanding asynchronous processing on the device or
 *   allocating special resources at fault time. So hopefully this is good
 *   enough, since adding interfaces to intercept pagefaults and allow pte
 *   shootdowns would increase the complexity quite a bit.
 *
 *   Interface:
 *
 *   .. code-block:: c
 *
 *     int dma_buf_mmap(struct dma_buf *, struct vm_area_struct *, unsigned long);
 *
 *   If the importing subsystem simply provides a special-purpose mmap call to
 *   set up a mapping in userspace, calling do_mmap with &dma_buf.file will
 *   equally achieve that for a dma-buf object.
 */

static int __dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction direction)
{
	bool write = (direction == DMA_BIDIRECTIONAL ||
		      direction == DMA_TO_DEVICE);
	struct dma_resv *resv = dmabuf->resv;
	long ret;

	/* Wait on any implicit rendering fences */
	ret = dma_resv_wait_timeout(resv, dma_resv_usage_rw(write),
				    true, MAX_SCHEDULE_TIMEOUT);
	if (ret < 0)
		return ret;

	return 0;
}

/**
 * dma_buf_begin_cpu_access - Must be called before accessing a dma_buf from the
 * cpu in the kernel context. Calls begin_cpu_access to allow exporter-specific
 * preparations. Coherency is only guaranteed in the specified range for the
 * specified access direction.
 * @dmabuf:	[in]	buffer to prepare cpu access for.
 * @direction:	[in]	direction of access.
 *
 * After the cpu access is complete the caller should call
 * dma_buf_end_cpu_access(). Only when cpu access is bracketed by both calls is
 * it guaranteed to be coherent with other DMA access.
 *
 * This function will also wait for any DMA transactions tracked through
 * implicit synchronization in &dma_buf.resv. For DMA transactions with explicit
 * synchronization this function will only ensure cache coherency, callers must
 * ensure synchronization with such DMA transactions on their own.
 *
 * Can return negative error values, returns 0 on success.
 */
int dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
			     enum dma_data_direction direction)
{
	int ret = 0;

	if (WARN_ON(!dmabuf))
		return -EINVAL;

	might_lock(&dmabuf->resv->lock.base);

	if (dmabuf->ops->begin_cpu_access)
		ret = dmabuf->ops->begin_cpu_access(dmabuf, direction);

	/* Ensure that all fences are waited upon - but we first allow
	 * the native handler the chance to do so more efficiently if it
	 * chooses. A double invocation here will be reasonably cheap no-op.
	 */
	if (ret == 0)
		ret = __dma_buf_begin_cpu_access(dmabuf, direction);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_begin_cpu_access, "DMA_BUF");

/**
 * dma_buf_end_cpu_access - Must be called after accessing a dma_buf from the
 * cpu in the kernel context. Calls end_cpu_access to allow exporter-specific
 * actions. Coherency is only guaranteed in the specified range for the
 * specified access direction.
 * @dmabuf:	[in]	buffer to complete cpu access for.
 * @direction:	[in]	direction of access.
 *
 * This terminates CPU access started with dma_buf_begin_cpu_access().
 *
 * Can return negative error values, returns 0 on success.
 */
int dma_buf_end_cpu_access(struct dma_buf *dmabuf,
			   enum dma_data_direction direction)
{
	int ret = 0;

	WARN_ON(!dmabuf);

	might_lock(&dmabuf->resv->lock.base);

	if (dmabuf->ops->end_cpu_access)
		ret = dmabuf->ops->end_cpu_access(dmabuf, direction);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_end_cpu_access, "DMA_BUF");


/**
 * dma_buf_mmap - Setup up a userspace mmap with the given vma
 * @dmabuf:	[in]	buffer that should back the vma
 * @vma:	[in]	vma for the mmap
 * @pgoff:	[in]	offset in pages where this mmap should start within the
 *			dma-buf buffer.
 *
 * This function adjusts the passed in vma so that it points at the file of the
 * dma_buf operation. It also adjusts the starting pgoff and does bounds
 * checking on the size of the vma. Then it calls the exporters mmap function to
 * set up the mapping.
 *
 * Can return negative error values, returns 0 on success.
 */
int dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma,
		 unsigned long pgoff)
{
	if (WARN_ON(!dmabuf || !vma))
		return -EINVAL;

	/* check if buffer supports mmap */
	if (!dmabuf->ops->mmap)
		return -EINVAL;

	/* check for offset overflow */
	if (pgoff + vma_pages(vma) < pgoff)
		return -EOVERFLOW;

	/* check for overflowing the buffer's size */
	if (pgoff + vma_pages(vma) >
	    dmabuf->size >> PAGE_SHIFT)
		return -EINVAL;

	/* readjust the vma */
	vma_set_file(vma, dmabuf->file);
	vma->vm_pgoff = pgoff;

	return dmabuf->ops->mmap(dmabuf, vma);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_mmap, "DMA_BUF");

/**
 * dma_buf_vmap - Create virtual mapping for the buffer object into kernel
 * address space. Same restrictions as for vmap and friends apply.
 * @dmabuf:	[in]	buffer to vmap
 * @map:	[out]	returns the vmap pointer
 *
 * This call may fail due to lack of virtual mapping address space.
 * These calls are optional in drivers. The intended use for them
 * is for mapping objects linear in kernel space for high use objects.
 *
 * To ensure coherency users must call dma_buf_begin_cpu_access() and
 * dma_buf_end_cpu_access() around any cpu access performed through this
 * mapping.
 *
 * Returns 0 on success, or a negative errno code otherwise.
 */
int dma_buf_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct iosys_map ptr;
	int ret;

	iosys_map_clear(map);

	if (WARN_ON(!dmabuf))
		return -EINVAL;

	dma_resv_assert_held(dmabuf->resv);

	if (!dmabuf->ops->vmap)
		return -EINVAL;

	if (dmabuf->vmapping_counter) {
		dmabuf->vmapping_counter++;
		BUG_ON(iosys_map_is_null(&dmabuf->vmap_ptr));
		*map = dmabuf->vmap_ptr;
		return 0;
	}

	BUG_ON(iosys_map_is_set(&dmabuf->vmap_ptr));

	ret = dmabuf->ops->vmap(dmabuf, &ptr);
	if (WARN_ON_ONCE(ret))
		return ret;

	dmabuf->vmap_ptr = ptr;
	dmabuf->vmapping_counter = 1;

	*map = dmabuf->vmap_ptr;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_vmap, "DMA_BUF");

/**
 * dma_buf_vmap_unlocked - Create virtual mapping for the buffer object into kernel
 * address space. Same restrictions as for vmap and friends apply.
 * @dmabuf:	[in]	buffer to vmap
 * @map:	[out]	returns the vmap pointer
 *
 * Unlocked version of dma_buf_vmap()
 *
 * Returns 0 on success, or a negative errno code otherwise.
 */
int dma_buf_vmap_unlocked(struct dma_buf *dmabuf, struct iosys_map *map)
{
	int ret;

	iosys_map_clear(map);

	if (WARN_ON(!dmabuf))
		return -EINVAL;

	dma_resv_lock(dmabuf->resv, NULL);
	ret = dma_buf_vmap(dmabuf, map);
	dma_resv_unlock(dmabuf->resv);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_vmap_unlocked, "DMA_BUF");

/**
 * dma_buf_vunmap - Unmap a vmap obtained by dma_buf_vmap.
 * @dmabuf:	[in]	buffer to vunmap
 * @map:	[in]	vmap pointer to vunmap
 */
void dma_buf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	if (WARN_ON(!dmabuf))
		return;

	dma_resv_assert_held(dmabuf->resv);

	BUG_ON(iosys_map_is_null(&dmabuf->vmap_ptr));
	BUG_ON(dmabuf->vmapping_counter == 0);
	BUG_ON(!iosys_map_is_equal(&dmabuf->vmap_ptr, map));

	if (--dmabuf->vmapping_counter == 0) {
		if (dmabuf->ops->vunmap)
			dmabuf->ops->vunmap(dmabuf, map);
		iosys_map_clear(&dmabuf->vmap_ptr);
	}
}
EXPORT_SYMBOL_NS_GPL(dma_buf_vunmap, "DMA_BUF");

/**
 * dma_buf_vunmap_unlocked - Unmap a vmap obtained by dma_buf_vmap.
 * @dmabuf:	[in]	buffer to vunmap
 * @map:	[in]	vmap pointer to vunmap
 */
void dma_buf_vunmap_unlocked(struct dma_buf *dmabuf, struct iosys_map *map)
{
	if (WARN_ON(!dmabuf))
		return;

	dma_resv_lock(dmabuf->resv, NULL);
	dma_buf_vunmap(dmabuf, map);
	dma_resv_unlock(dmabuf->resv);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_vunmap_unlocked, "DMA_BUF");

#ifdef CONFIG_DEBUG_FS
static int dma_buf_debug_show(struct seq_file *s, void *unused)
{
	struct dma_buf *buf_obj;
	struct dma_buf_attachment *attach_obj;
	int count = 0, attach_count;
	size_t size = 0;
	int ret;

	ret = mutex_lock_interruptible(&dmabuf_list_mutex);

	if (ret)
		return ret;

	seq_puts(s, "\nDma-buf Objects:\n");
	seq_printf(s, "%-8s\t%-8s\t%-8s\t%-8s\texp_name\t%-8s\tname\n",
		   "size", "flags", "mode", "count", "ino");

	list_for_each_entry(buf_obj, &dmabuf_list, list_node) {

		ret = dma_resv_lock_interruptible(buf_obj->resv, NULL);
		if (ret)
			goto error_unlock;


		spin_lock(&buf_obj->name_lock);
		seq_printf(s, "%08zu\t%08x\t%08x\t%08ld\t%s\t%08lu\t%s\n",
				buf_obj->size,
				buf_obj->file->f_flags, buf_obj->file->f_mode,
				file_count(buf_obj->file),
				buf_obj->exp_name,
				file_inode(buf_obj->file)->i_ino,
				buf_obj->name ?: "<none>");
		spin_unlock(&buf_obj->name_lock);

		dma_resv_describe(buf_obj->resv, s);

		seq_puts(s, "\tAttached Devices:\n");
		attach_count = 0;

		list_for_each_entry(attach_obj, &buf_obj->attachments, node) {
			seq_printf(s, "\t%s\n", dev_name(attach_obj->dev));
			attach_count++;
		}
		dma_resv_unlock(buf_obj->resv);

		seq_printf(s, "Total %d devices attached\n\n",
				attach_count);

		count++;
		size += buf_obj->size;
	}

	seq_printf(s, "\nTotal %d objects, %zu bytes\n", count, size);

	mutex_unlock(&dmabuf_list_mutex);
	return 0;

error_unlock:
	mutex_unlock(&dmabuf_list_mutex);
	return ret;
}

DEFINE_SHOW_ATTRIBUTE(dma_buf_debug);

static struct dentry *dma_buf_debugfs_dir;

static int dma_buf_init_debugfs(void)
{
	struct dentry *d;
	int err = 0;

	d = debugfs_create_dir("dma_buf", NULL);
	if (IS_ERR(d))
		return PTR_ERR(d);

	dma_buf_debugfs_dir = d;

	d = debugfs_create_file("bufinfo", 0444, dma_buf_debugfs_dir,
				NULL, &dma_buf_debug_fops);
	if (IS_ERR(d)) {
		pr_debug("dma_buf: debugfs: failed to create node bufinfo\n");
		debugfs_remove_recursive(dma_buf_debugfs_dir);
		dma_buf_debugfs_dir = NULL;
		err = PTR_ERR(d);
	}

	return err;
}

static void dma_buf_uninit_debugfs(void)
{
	debugfs_remove_recursive(dma_buf_debugfs_dir);
}
#else
static inline int dma_buf_init_debugfs(void)
{
	return 0;
}
static inline void dma_buf_uninit_debugfs(void)
{
}
#endif

static int __init dma_buf_init(void)
{
	int ret;

	ret = dma_buf_init_sysfs_statistics();
	if (ret)
		return ret;

	dma_buf_mnt = kern_mount(&dma_buf_fs_type);
	if (IS_ERR(dma_buf_mnt))
		return PTR_ERR(dma_buf_mnt);

	dma_buf_init_debugfs();
	return 0;
}
subsys_initcall(dma_buf_init);

static void __exit dma_buf_deinit(void)
{
	dma_buf_uninit_debugfs();
	kern_unmount(dma_buf_mnt);
	dma_buf_uninit_sysfs_statistics();
}
__exitcall(dma_buf_deinit);
