// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016 Trond Myklebust
 *
 * I/O and data path helper functionality.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/rwsem.h>
#include <linux/fs.h>
#include <linux/nfs_fs.h>

#include "internal.h"

/**
 * nfs_start_io_read - declare the file is being used for buffered reads
 * @inode: file inode
 *
 * Declare that a buffered read operation is about to start, and ensure
 * that we block all direct I/O.
 * On exit, the function ensures that the NFS_INO_ODIRECT flag is unset,
 * and holds a shared lock on inode->i_rwsem to ensure that the flag
 * cannot be changed.
 * In practice, this means that buffered read operations are allowed to
 * execute in parallel, thanks to the shared lock, whereas direct I/O
 * operations need to wait to grab an exclusive lock in order to set
 * NFS_INO_ODIRECT.
 * Note that buffered writes and truncates both take a write lock on
 * inode->i_rwsem, meaning that those are serialised w.r.t. the reads.
 */
int
nfs_start_io_read(struct inode *inode)
{
	struct nfs_inode *nfsi = NFS_I(inode);
	int err;

	/* Be an optimist! */
	err = down_read_killable(&inode->i_rwsem);
	if (err)
		return err;
	if (test_bit(NFS_INO_ODIRECT, &nfsi->flags) == 0)
		return 0;
	up_read(&inode->i_rwsem);

	/* Slow path.... */
	err = down_write_killable(&inode->i_rwsem);
	if (err)
		return err;
	nfs_file_block_o_direct(nfsi);
	downgrade_write(&inode->i_rwsem);

	return 0;
}

/**
 * nfs_end_io_read - declare that the buffered read operation is done
 * @inode: file inode
 *
 * Declare that a buffered read operation is done, and release the shared
 * lock on inode->i_rwsem.
 */
void
nfs_end_io_read(struct inode *inode)
{
	up_read(&inode->i_rwsem);
}

/**
 * nfs_start_io_write - declare the file is being used for buffered writes
 * @inode: file inode
 *
 * Declare that a buffered read operation is about to start, and ensure
 * that we block all direct I/O.
 */
int
nfs_start_io_write(struct inode *inode)
{
	int err;

	err = down_write_killable(&inode->i_rwsem);
	if (!err)
		nfs_file_block_o_direct(NFS_I(inode));
	return err;
}
EXPORT_SYMBOL_GPL(nfs_start_io_write);

/**
 * nfs_end_io_write - declare that the buffered write operation is done
 * @inode: file inode
 *
 * Declare that a buffered write operation is done, and release the
 * lock on inode->i_rwsem.
 */
void
nfs_end_io_write(struct inode *inode)
{
	up_write(&inode->i_rwsem);
}
EXPORT_SYMBOL_GPL(nfs_end_io_write);

/*
 * Call with exclusively locked inode->i_rwsem.
 *
 * Setting NFS_INO_ODIRECT admits lockless O_DIRECT (see
 * nfs_start_io_direct_lockless()), which does not wait for this
 * exclusive section to end.  So the buffered data must be flushed
 * *before* the flag is published, and the flag must be set with
 * release ordering after that flush.
 */
static void nfs_block_buffered(struct nfs_inode *nfsi, struct inode *inode)
{
	if (!test_bit(NFS_INO_ODIRECT, &nfsi->flags)) {
		nfs_sync_mapping(inode->i_mapping);
		smp_mb__before_atomic();
		set_bit(NFS_INO_ODIRECT, &nfsi->flags);
	}
}

static int nfs_block_buffered_nowait(struct nfs_inode *nfsi, struct inode *inode)
{
	if (!test_bit(NFS_INO_ODIRECT, &nfsi->flags)) {
		if (inode->i_mapping->nrpages != 0)
			return 1;
		smp_mb__before_atomic();
		set_bit(NFS_INO_ODIRECT, &nfsi->flags);
	}
	return 0;
}

/*
 * Lockless O_DIRECT admission.
 *
 * Instead of taking inode->i_rwsem shared, pin the inode's DIO count and
 * then check that the inode is in O_DIRECT mode.  This pairs with
 * nfs_file_block_o_direct(), which every exclusive i_rwsem holder that
 * needs O_DIRECT excluded calls: it clears NFS_INO_ODIRECT, issues a full
 * barrier and then waits for i_dio_count to drain:
 *
 *   here:                            nfs_file_block_o_direct():
 *     atomic_inc(&i_dio_count)         clear_bit(NFS_INO_ODIRECT)
 *     smp_mb()                         smp_mb()
 *     test_bit(NFS_INO_ODIRECT)        wait for i_dio_count == 0
 *
 * Either we see the flag clear and back off to the i_rwsem slow path, or
 * the exclusive holder sees our count and waits for it.  The flag is only
 * ever set again under the exclusive lock, after buffered data has been
 * flushed (nfs_block_buffered()), so a set flag observed here with
 * acquire ordering means the inode is in O_DIRECT mode and no exclusive
 * holder that needs O_DIRECT excluded has passed its drain point.
 *
 * On success the caller owns one i_dio_count reference, which it must hand
 * to the direct I/O request (nfs_direct_*_schedule_iovec()), which drops it
 * when the request completes.
 */
static bool nfs_start_io_direct_lockless(struct inode *inode)
{
	/*
	 * Cheap pre-check: in buffered mode, go straight to the i_rwsem slow
	 * path without an inc/dec of i_dio_count.  Only the re-check below,
	 * after the barrier, is authoritative.
	 */
	if (!test_bit(NFS_INO_ODIRECT, &NFS_I(inode)->flags))
		return false;
	inode_dio_begin(inode);
	smp_mb__after_atomic();
	if (test_bit_acquire(NFS_INO_ODIRECT, &NFS_I(inode)->flags))
		return true;
	inode_dio_end(inode);
	return false;
}

/**
 * nfs_start_io_direct - declare the file is being used for direct i/o
 * @inode: file inode
 *
 * Declare that a direct I/O operation is about to start, and ensure
 * that we block all buffered I/O.
 * On success, the function ensures that the NFS_INO_ODIRECT flag is set,
 * and either holds a shared lock on inode->i_rwsem (returns 0) or an
 * i_dio_count reference (returns NFS_IO_DIRECT_LOCKLESS), either of which
 * ensures that the flag cannot be cleared and waited out under us.
 * In practice, this means that direct I/O operations are allowed to
 * execute in parallel, thanks to the shared lock, whereas buffered I/O
 * operations need to wait to grab an exclusive lock in order to clear
 * NFS_INO_ODIRECT.
 * Note that buffered writes and truncates both take a write lock on
 * inode->i_rwsem, meaning that those are serialised w.r.t. O_DIRECT.
 *
 * Fast path: if the inode is already in O_DIRECT mode, no i_rwsem is taken
 * at all; the function instead returns NFS_IO_DIRECT_LOCKLESS holding an
 * i_dio_count reference that the caller must pass to the direct I/O
 * request, and nfs_end_io_direct() must NOT be called.  Returns 0 when the
 * shared i_rwsem is held, or a negative error.
 */
int
nfs_start_io_direct(struct inode *inode)
{
	struct nfs_inode *nfsi = NFS_I(inode);
	int err;

	if (nfs_start_io_direct_lockless(inode))
		return NFS_IO_DIRECT_LOCKLESS;

	/* Be an optimist! */
	err = down_read_killable(&inode->i_rwsem);
	if (err)
		return err;
	if (test_bit(NFS_INO_ODIRECT, &nfsi->flags) != 0)
		return 0;
	up_read(&inode->i_rwsem);

	/* Slow path.... */
	err = down_write_killable(&inode->i_rwsem);
	if (err)
		return err;
	nfs_block_buffered(nfsi, inode);
	downgrade_write(&inode->i_rwsem);

	return 0;
}

/**
 * nfs_start_io_direct_nowait - non-blocking variant of nfs_start_io_direct()
 * @inode: file inode
 *
 * Try to declare that a direct I/O operation is about to start without
 * blocking.
 * Ensure all buffered I/O is blocked.
 * If this could not be done without blocking then returns -EAGAIN.
 * Otherwise returns NFS_IO_DIRECT_LOCKLESS (an i_dio_count reference is
 * held and must be handed to the request; do not call nfs_end_io_direct())
 * or 0 (shared i_rwsem held), as nfs_start_io_direct() does.
 */
int
nfs_start_io_direct_nowait(struct inode *inode)
{
	struct nfs_inode *nfsi = NFS_I(inode);

	if (nfs_start_io_direct_lockless(inode))
		return NFS_IO_DIRECT_LOCKLESS;

	if (!down_read_trylock(&inode->i_rwsem))
		return -EAGAIN;
	if (test_bit(NFS_INO_ODIRECT, &nfsi->flags))
		return 0;
	up_read(&inode->i_rwsem);

	/* Slow path: try to flip NFS_INO_ODIRECT without blocking. */
	if (!down_write_trylock(&inode->i_rwsem))
		return -EAGAIN;
	if (nfs_block_buffered_nowait(nfsi, inode)) {
		up_write(&inode->i_rwsem);
		return -EAGAIN;
	}
	downgrade_write(&inode->i_rwsem);
	return 0;
}

/**
 * nfs_end_io_direct - declare that the direct i/o operation is done
 * @inode: file inode
 *
 * Declare that a direct I/O operation is done, and release the shared
 * lock on inode->i_rwsem.  Only for nfs_start_io_direct*() returning 0.
 */
void
nfs_end_io_direct(struct inode *inode)
{
	up_read(&inode->i_rwsem);
}
