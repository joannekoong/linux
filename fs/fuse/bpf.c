// SPDX-License-Identifier: GPL-2.0

#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/module.h>

#include "fuse_i.h"
#include "fuse_dev_i.h"
#include "fuse_bpf_i.h"

BTF_ID_LIST_GLOBAL_SINGLE(btf_fuse_bpf_ops_id, struct, fuse_bpf_ops)
BTF_ID_LIST_GLOBAL_SINGLE(btf_fuse_iomap_io_id, struct, fuse_iomap_io)

extern u32 btf_fuse_bpf_ops_id[];
extern u32 btf_fuse_iomap_io_id[];

static bool fuse_bpf_ops_is_valid_access(int off, int size,
					  enum bpf_access_type type,
					  const struct bpf_prog *prog,
					  struct bpf_insn_access_aux *info)
{
	return bpf_tracing_btf_ctx_access(off, size, type, prog, info);
}

static int fuse_bpf_ops_check_member(const struct btf_type *t,
				      const struct btf_member *member,
				      const struct bpf_prog *prog)
{
	u32 moff = __btf_member_bit_offset(t, member) / 8;
	switch (moff) {
	case offsetof(struct fuse_bpf_ops, iomap_begin):
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int fuse_bpf_ops_btf_struct_access(struct bpf_verifier_log *log,
					   const struct bpf_reg_state *reg,
					   int off, int size)
{
	u32 btf_id = reg->btf_id;

	/*
	 * TODO: add more thorough checking for struct access
	 *
	 * For now, give it full access to fuse_iomap_io struct
	 */

	if (btf_id == btf_fuse_iomap_io_id[0]) {
		if (off < 0 || off + size > sizeof(struct fuse_iomap_io))
			return -EACCES;
		return size;
	}

	return -EACCES;
}

static const struct bpf_verifier_ops fuse_bpf_verifier_ops = {
	.get_func_proto = bpf_base_func_proto,
	.is_valid_access = fuse_bpf_ops_is_valid_access,
	.btf_struct_access = fuse_bpf_ops_btf_struct_access,
};

static int fuse_bpf_ops_init_member(const struct btf_type *t,
				    const struct btf_member *member,
				    void *kdata, const void *udata)
{
	const struct fuse_bpf_ops *u_ops = udata;
	struct fuse_bpf_ops *ops = kdata;
	u32 moff;

	moff = __btf_member_bit_offset(t, member) / 8;
	switch (moff) {
	case offsetof(struct fuse_bpf_ops, name):
		if (bpf_obj_name_cpy(ops->name, u_ops->name,
			     sizeof(ops->name)) <= 0)
			return -EINVAL;
		return 1;  /* Handled */
	case offsetof(struct fuse_bpf_ops, dev_fd):
		ops->dev_fd = u_ops->dev_fd;
		return 1;
	}

	/* Not handled, use default */
	return 0;
}

static struct fuse_conn *fuse_conn_from_fd(int fd)
{
	struct file *file;
	struct fuse_conn *fc;
	struct fuse_dev *fud;

	file = fget(fd);
	if (!file)
		return ERR_PTR(-EBADF);
	if (file->f_op != &fuse_dev_operations) {
		fput(file);
		return ERR_PTR(-EINVAL);
	}

	fud = __fuse_get_dev(file);
	if (!fud || !fud->fc) {
		fput(file);
		return ERR_PTR(-EINVAL);
	}

	fc = fud->fc;
	fuse_conn_get(fc);
	fput(file);
	return fc;
}

static int fuse_bpf_reg(void *kdata, struct bpf_link *link)
{
	struct fuse_bpf_ops *ops = kdata;
	struct fuse_conn *fc;

	if (ops->dev_fd < 0)
		return -EINVAL;

	fc = fuse_conn_from_fd(ops->dev_fd);
	if (IS_ERR(fc))
		return PTR_ERR(fc);

	/*
	 * TODO: support dynamic bpf prog replacement in the future
	 * For now, error out if there's already bpf prog registered
	 */
	spin_lock(&fc->lock);
	if (fc->bpf_ops) {
		spin_unlock(&fc->lock);
		fuse_conn_put(fc);
		return -EBUSY;
	}

	fc->bpf_ops = ops;

	spin_unlock(&fc->lock);

	printk("fuse_bpf: registered ops '%s' for /dev/fuse fd=%d\n",
	       ops->name, ops->dev_fd);

	return 0;
}

static void fuse_bpf_unreg(void *kdata, struct bpf_link *link)
{
	struct fuse_bpf_ops *ops = kdata;

	/*
	 * TODO: add cleanup for getting fc and calling
	 *	fc->bpf_ops = NULL;
	 *	fuse_conn_put(fc);
	 */

	printk("fuse_bpf: unregistered ops '%s'\n", ops->name);
}

static int __iomap_begin(u64 nodeid, loff_t pos,
			 loff_t length, unsigned int flags,
			 struct fuse_iomap_io *out_io)
{
	printk("stub __iomap_begin(). should never get called\n");
	return 0;
}

static struct fuse_bpf_ops __fuse_bpf_ops = {
	.iomap_begin = __iomap_begin,
};

static int fuse_bpf_ops_init(struct btf *btf)
{
	return 0;
}

static struct bpf_struct_ops fuse_bpf_struct_ops = {
	.verifier_ops = &fuse_bpf_verifier_ops,
	.init = fuse_bpf_ops_init,
	.check_member = fuse_bpf_ops_check_member,
	.init_member = fuse_bpf_ops_init_member,
	.reg = fuse_bpf_reg,
	.unreg = fuse_bpf_unreg,
	.name = "fuse_bpf_ops",
	.cfi_stubs = &__fuse_bpf_ops,
	.owner = THIS_MODULE,
};

int __init fuse_bpf_init(void)
{
	return register_bpf_struct_ops(&fuse_bpf_struct_ops, fuse_bpf_ops);
}
#ifndef MODULE
late_initcall(fuse_bpf_init);
#endif

MODULE_IMPORT_NS("BPF_INTERNAL");
