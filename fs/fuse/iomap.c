// SPDX-License-Identifier: GPL-2.0

#include <linux/dax.h>
#include <linux/iomap.h>
#include <linux/pagemap.h>

#include "fuse_i.h"
#include "fuse_bpf_i.h"
#include "fuse_dev_i.h"
#include "iomap_i.h"

/* forward declarations */
static const struct vm_operations_struct fuse_iomap_vm_ops;

static int fuse_fill_iomap_begin(struct inode *inode, struct iomap *iomap,
				 struct fuse_iomap_io *io)
{
	struct fuse_conn *fc = get_fuse_conn(inode);

	iomap->addr = io->addr;
	iomap->offset = io->offset;
	iomap->length = io->length;
	iomap->type = io->type;
	iomap->flags = io->flags;

	if (IS_DAX(inode)) {
		if (WARN_ON_ONCE(io->id >= fc->iomap_state.dax.ndevs))
			return -EIO;
		iomap->dax_dev = fc->iomap_state.dax.devp[io->id];
	}

	return 0;
}

static int fuse_iomap_begin(struct inode *inode, loff_t pos, loff_t count,
			    unsigned opflags, struct iomap *iomap,
			    struct iomap *srcmap)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_bpf_ops *ops = fc->bpf_ops;
	int err = -ENOSYS;

	if (ops && ops->iomap_begin) {
		struct fuse_iomap_io io = {};

		err = ops->iomap_begin(fi->nodeid, pos, count, opflags, &io);
		if (!err)
			err = fuse_fill_iomap_begin(inode, iomap, &io);
	}

	/*
	 * TODO:
	 * if (err == -ENOSYS)
	 *      issue FUSE_IOMAP_BEGIN request to server
	 */

	/*
	 * for testing this on passthrough_hp server, return -ENOSYS here since
	 * this doesn't actually interact with dax device
	 */
	err = -EFAULT;

	return err;
}

static const struct iomap_ops fuse_iomap_ops = {
	.iomap_begin		= fuse_iomap_begin,
};

static ssize_t fuse_iomap_dax_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	ssize_t ret;

	if (WARN_ON_ONCE(!IS_DAX(inode)))
		return -EINVAL;

	if (fuse_is_bad(inode))
		return -EIO;

	ret = dax_iomap_rw(iocb, to, &fuse_iomap_ops);
	file_accessed(iocb->ki_filp);

	return ret;
}

static ssize_t fuse_iomap_dax_write_iter(struct kiocb *iocb,
					 struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);

	if (WARN_ON_ONCE(!IS_DAX(inode)))
		return -EINVAL;

	if (fuse_is_bad(inode))
		return -EIO;

	return dax_iomap_rw(iocb, from, &fuse_iomap_ops);
}

static int fuse_iomap_dax_mmap(struct file *file, struct vm_area_struct *vma)
{
	file_accessed(file);
	vma->vm_ops = &fuse_iomap_vm_ops;
	vm_flags_set(vma, VM_HUGEPAGE);

	return 0;
}

static const struct file_operations fuse_iomap_dax_fops = {
	.llseek		= fuse_file_llseek,
	.read_iter	= fuse_iomap_dax_read_iter,
	.write_iter	= fuse_iomap_dax_write_iter,
	.mmap		= fuse_iomap_dax_mmap,
	.open		= fuse_open,
	.flush		= fuse_flush,
	.release	= fuse_release,
	.fsync		= fuse_fsync,
	.lock		= fuse_file_lock,
	.get_unmapped_area = thp_get_unmapped_area,
	.flock		= fuse_file_flock,
	.splice_read	= copy_splice_read,
	.splice_write	= iter_file_splice_write,
	.unlocked_ioctl	= fuse_file_ioctl,
	.fallocate	= fuse_file_fallocate,
	.copy_file_range = fuse_copy_file_range,
};

struct fuse_iomap_config_reply;
#ifdef CONFIG_FUSE_DAX
static int
fuse_dax_notify_failure(struct dax_device *dax_devp, u64 offset,
			u64 len, int mf_flags)
{
	pr_warn("dax notify failure: off=%llu, len=%llu, mf_flags=%d\n",
		offset, len, mf_flags);

	/*
	 * TODO: could also add dax_notify_failure structops callback a server
	 * can register a bpf prog for
	 */

	return 0;
}

static const struct dax_holder_operations fuse_dax_holder_ops = {
	.notify_failure		= fuse_dax_notify_failure,
};

static int fuse_init_iomap_dax(struct fuse_conn *fc,
			       struct fuse_iomap_config_reply *config)
{
	struct fuse_iomap_state *state = &fc->iomap_state;
	struct dax_device **devp;
	/*
	 * These values should be taken from config reply from server.
	 * For this proof-of-concept they're hard-coded
	 */
	unsigned int ndax_devs = 4;
	unsigned int devnos[] = {1, 2, 3, 4};
	unsigned int i;
	int err;

	/*
	 * Dax mode should be taken from config reply from server.
	 * For this proof-of-concept assume the server wanted FUSE_DAX_ALWAYS.
	 */
	fc->dax_mode = FUSE_DAX_ALWAYS;

	if (ndax_devs > FUSE_NDAX_DEV_LIMIT)
		return -EINVAL;

	devp = kcalloc(ndax_devs, sizeof(*devp), GFP_KERNEL);
	if (!devp)
		return -ENOMEM;

/* for testing this on passthrough_hp server, comment this out */
#if 0
	for (i = 0; i < ndax_devs; i++) {
		devp[i] = dax_dev_get(devnos[i]);
		if (!devp[i]) {
			pr_warn("dax device devno %u not found\n",
				devnos[i]);
			err = -ENODEV;
			goto fail;
		}
		err = fs_dax_get(devp[i], fc, &fuse_dax_holder_ops);
		if (err)
			return err;
	}
#endif

	state->dax.ndevs = ndax_devs;
	state->dax.devp = devp;

	return 0;

fail:
	for (i = 0; i < ndax_devs; i++) {
		fs_put_dax(devp[i], fc);
		put_dax(devp[i]);
	}
	kfree(devp);
	return err;
}

static bool is_fuse_iomap_dax_set(struct fuse_conn *fc)
{
	return fc->dax_mode == FUSE_DAX_ALWAYS ||
		fc->dax_mode == FUSE_DAX_INODE_USER;
}
#else
static int fuse_init_iomap_dax(struct fuse_conn *fc,
			       unsigned int ndax_devs)
{
	pr_warn("to use fuse iomap dax, CONFIG_FUSE_DAX must be set\n");
	return -EOPNOTSUPP;
}

static bool is_fuse_iomap_dax_set(struct fuse_conn *fc)
{
	return false;
}
#endif

/* address_space_operations for dax. dax bypasses the page cache */
static const struct address_space_operations fuse_iomap_dax_aops = {
	.direct_IO	= noop_direct_IO,
	.dirty_folio	= noop_dirty_folio,
};

int fuse_iomap_init_file_inode(struct inode *inode)
{
	struct fuse_conn *fc = get_fuse_conn(inode);

	/*
	 * currently only dax in iomap is supported.
	 * iomap non-dax operations will be supported later
	 */
	if (is_fuse_iomap_dax_set(fc)) {
		inode->i_flags |= S_DAX;
		inode->i_fop = &fuse_iomap_dax_fops;
		inode->i_data.a_ops = &fuse_iomap_dax_aops;
		return 0;
	}

	return -ENOSYS;
}

static vm_fault_t __fuse_iomap_dax_fault(struct vm_fault *vmf,
					 unsigned int order, bool write)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	vm_fault_t ret;
	unsigned long pfn;

	if (!IS_DAX(inode))
		return VM_FAULT_SIGBUS;

	if (write) {
		sb_start_pagefault(inode->i_sb);
		file_update_time(vmf->vma->vm_file);
	}

	ret = dax_iomap_fault(vmf, order, &pfn, NULL, &fuse_iomap_ops);
	if (ret & VM_FAULT_NEEDDSYNC)
		ret = dax_finish_sync_fault(vmf, order, pfn);

	if (write)
		sb_end_pagefault(inode->i_sb);

	return ret;
}

static vm_fault_t fuse_iomap_dax_fault(struct vm_fault *vmf)
{
	return __fuse_iomap_dax_fault(vmf, 0, vmf->flags & FAULT_FLAG_WRITE);
}

static vm_fault_t fuse_iomap_dax_huge_fault(struct vm_fault *vmf,
					    unsigned int order)
{
	return __fuse_iomap_dax_fault(vmf, order,
				      vmf->flags & FAULT_FLAG_WRITE);
}

static vm_fault_t fuse_iomap_dax_mkwrite(struct vm_fault *vmf)
{
	return __fuse_iomap_dax_fault(vmf, 0, true);
}

static const struct vm_operations_struct fuse_iomap_vm_ops = {
	.fault		= fuse_iomap_dax_fault,
	.huge_fault	= fuse_iomap_dax_huge_fault,
	.page_mkwrite	= fuse_iomap_dax_mkwrite,
	.pfn_mkwrite	= fuse_iomap_dax_mkwrite,
};

int fuse_init_iomap(struct fuse_conn *fc)
{
	/*
	 * TODO:
	 * a) Send FUSE_IOMAP_CONFIG request to server to get config info
	 * b) Process the server's reply
	 *
	 * If the reply has 'outarg->flags & FUSE_IOMAP_CONFIG_DAX_ALWAYS' set,
	 * then the dax mode should always be used. If the server sets dax mode,
	 * it will return in the config reply:
	 * - number of dax devs (ndax_devs)
	 * - name of each daxdev
	 * - daxdev id of each daxdev
	 *
	 * For this proof-of-concept, just assume this flag has been set by the
	 * server and use hard-coded values for the dax return values
	 */
	bool use_dax_always = true;
	struct fuse_iomap_config_reply *config;
	int err;

	if (use_dax_always) {
		if (fc->writeback_cache)
			return -EINVAL;
		err = fuse_init_iomap_dax(fc, config);
	}

	return err;
}

void fuse_iomap_teardown(struct fuse_conn *fc)
{
	unsigned ndevs = fc->iomap_state.dax.ndevs;
	struct dax_device **devp = fc->iomap_state.dax.devp;
	unsigned int i;

	if (!ndevs)
		return;

	WARN_ON_ONCE(!devp);

	for (i = 0; i < ndevs; i++) {
		fs_put_dax(devp[i], fc);
		put_dax(devp[i]);
	}

	kfree(devp);
}
