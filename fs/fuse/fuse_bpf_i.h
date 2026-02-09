#ifndef _FS_FUSE_BPF_H
#define _FS_FUSE_BPF_H

#include "fuse_i.h"

#include <linux/bpf.h>
#include <linux/iomap.h>

/* copied from darrick's iomap patchset */
struct fuse_iomap_io {
        uint64_t offset;        /* file offset of mapping, bytes */
        uint64_t length;        /* length of mapping, bytes */
        uint64_t addr;          /* disk offset of mapping, bytes */
        uint16_t type;          /* FUSE_IOMAP_TYPE_* */
        uint16_t flags;         /* FUSE_IOMAP_F_* */
        uint32_t dev;           /* device cookie */
        uint64_t id;            /* eg for dax devices */
};

struct fuse_bpf_ops {
    /* Required for bpf struct_ops */
    char name[BPF_OBJ_NAME_LEN];

    int (*iomap_begin)(u64 nodeid, loff_t pos, loff_t length,
		       unsigned int flags,
		       struct fuse_iomap_io *out_io__nullable);

    /* /dev/fuse fd */
    int dev_fd;
};

#ifdef CONFIG_DEBUG_INFO_BTF
int __init fuse_bpf_init(void);

#else /* !CONFIG_DEBUG_INFO_BTF */
static inline int __init fuse_bpf_init(void)
{
	return 0;
}
#endif /* CONFIG_DEBUG_INFO_BTF */

#endif /* _FS_FUSE_BPF_H */
