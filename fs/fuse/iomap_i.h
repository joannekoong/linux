// SPDX-License-Identifier: GPL-2.0

#ifndef _FS_FUSE_IOMAP_I_H
#define _FS_FUSE_IOMAP_I_H

int fuse_init_iomap(struct fuse_conn *fc);
void fuse_iomap_teardown(struct fuse_conn *fc);
int fuse_iomap_init_file_inode(struct inode *inode);

#endif /* _FS_FUSE_IOMAP_I_H */
