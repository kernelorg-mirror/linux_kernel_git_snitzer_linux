=======
Locking
=======

The text below describes the locking rules for VFS-related methods.
It is (believed to be) up-to-date. *Please*, if you change anything in
prototypes or locking protocols - update this file. And update the relevant
instances in the tree, don't leave that to maintainers of filesystems/devices/
etc. At the very least, put the list of dubious cases in the end of this file.
Don't turn it into log - maintainers of out-of-the-tree code are supposed to
be able to use diff(1).

Thing currently missing here: socket operations. Alexey?

dentry_operations
=================

prototypes::

	int (*d_revalidate)(struct inode *, const struct qstr *,
			    struct dentry *, unsigned int);
	int (*d_weak_revalidate)(struct dentry *, unsigned int);
	int (*d_hash)(const struct dentry *, struct qstr *);
	int (*d_compare)(const struct dentry *,
			unsigned int, const char *, const struct qstr *);
	int (*d_delete)(struct dentry *);
	int (*d_init)(struct dentry *);
	void (*d_release)(struct dentry *);
	void (*d_iput)(struct dentry *, struct inode *);
	char *(*d_dname)((struct dentry *dentry, char *buffer, int buflen);
	struct vfsmount *(*d_automount)(struct path *path);
	int (*d_manage)(const struct path *, bool);
	struct dentry *(*d_real)(struct dentry *, enum d_real_type type);
	bool (*d_unalias_trylock)(const struct dentry *);
	void (*d_unalias_unlock)(const struct dentry *);

locking rules:

================== ===========	========	==============	========
ops		   rename_lock	->d_lock	may block	rcu-walk
================== ===========	========	==============	========
d_revalidate:	   no		no		yes (ref-walk)	maybe
d_weak_revalidate: no		no		yes	 	no
d_hash		   no		no		no		maybe
d_compare:	   yes		no		no		maybe
d_delete:	   no		yes		no		no
d_init:		   no		no		yes		no
d_release:	   no		no		yes		no
d_prune:           no		yes		no		no
d_iput:		   no		no		yes		no
d_dname:	   no		no		no		no
d_automount:	   no		no		yes		no
d_manage:	   no		no		yes (ref-walk)	maybe
d_real		   no		no		yes 		no
d_unalias_trylock  yes		no		no 		no
d_unalias_unlock   yes		no		no 		no
================== ===========	========	==============	========

inode_operations
================

prototypes::

	int (*create) (struct mnt_idmap *, struct inode *,struct dentry *,umode_t);
	struct dentry * (*lookup) (struct inode *,struct dentry *, unsigned int);
	int (*link) (struct dentry *,struct inode *,struct dentry *);
	int (*unlink) (struct inode *,struct dentry *);
	int (*unlink_shared) (struct inode *,struct dentry *);
	void (*unlink_shared_done) (struct inode *,struct dentry *);
	int (*symlink) (struct mnt_idmap *, struct inode *,struct dentry *,const char *);
	struct dentry *(*mkdir) (struct mnt_idmap *, struct inode *,struct dentry *,umode_t);
	int (*rmdir) (struct inode *,struct dentry *);
	int (*mknod) (struct mnt_idmap *, struct inode *,struct dentry *,umode_t,dev_t);
	int (*rename) (struct mnt_idmap *, struct inode *, struct dentry *,
			struct inode *, struct dentry *, unsigned int);
	int (*readlink) (struct dentry *, char __user *,int);
	const char *(*get_link) (struct dentry *, struct inode *, struct delayed_call *);
	void (*truncate) (struct inode *);
	int (*permission) (struct mnt_idmap *, struct inode *, int, unsigned int);
	struct posix_acl * (*get_inode_acl)(struct inode *, int, bool);
	int (*setattr) (struct mnt_idmap *, struct dentry *, struct iattr *);
	int (*getattr) (struct mnt_idmap *, const struct path *, struct kstat *, u32, unsigned int);
	ssize_t (*listxattr) (struct dentry *, char *, size_t);
	int (*fiemap)(struct inode *, struct fiemap_extent_info *, u64 start, u64 len);
	void (*update_time)(struct inode *inode, enum fs_update_time type,
			    int flags);
	void (*sync_lazytime)(struct inode *inode);
	int (*atomic_open)(struct inode *, struct dentry *,
				struct file *, unsigned open_flag,
				umode_t create_mode);
	int (*tmpfile) (struct mnt_idmap *, struct inode *,
			struct file *, umode_t);
	int (*fileattr_set)(struct mnt_idmap *idmap,
			    struct dentry *dentry, struct file_kattr *fa);
	int (*fileattr_get)(struct dentry *dentry, struct file_kattr *fa);
	struct posix_acl * (*get_acl)(struct mnt_idmap *, struct dentry *, int);
	struct offset_ctx *(*get_offset_ctx)(struct inode *inode);

locking rules:
	all may block

==============	==================================================
ops		i_rwsem(inode)
==============	==================================================
lookup:		shared
create:		exclusive
link:		exclusive (both)
mknod:		exclusive
symlink:	exclusive
mkdir:		exclusive
unlink:		exclusive (both)
unlink_shared:	shared on parent, exclusive on victim (see below)
rmdir:		exclusive (both)(see below)
rename:		exclusive (both parents, some children)	(see below)
readlink:	no
get_link:	no
setattr:	exclusive
permission:	no (may not block if called in rcu-walk mode)
get_inode_acl:	no
get_acl:	no
getattr:	no
listxattr:	no
fiemap:		no
update_time:	no
sync_lazytime:	no
atomic_open:	shared (exclusive if O_CREAT unless the fs opts in)
tmpfile:	no
fileattr_get:	no or exclusive
fileattr_set:	exclusive
get_offset_ctx  no
==============	==================================================


	Additionally, ->rmdir(), ->unlink(), ->unlink_shared() and ->rename()
	have ->i_rwsem exclusive on victim.
	cross-directory ->rename() has (per-superblock) ->s_vfs_rename_sem.
	->unlink(), ->unlink_shared() and ->rename() have ->i_rwsem exclusive
	on all non-directories involved.
	->rename() has ->i_rwsem exclusive on any subdirectory that changes parent.
	->unlink_shared_done() has ->i_rwsem shared on the parent and nothing
	on the victim.  It is not an operation on the directory but the end of
	one, and is described with ->unlink_shared() below rather than listed
	above.

	->atomic_open() with O_CREAT set is called with ->i_rwsem on the
	parent held exclusive, unless the filesystem has set
	IOPF_ATOMIC_OPEN_CREATE_SHARED in the ->iop_flags of that
	directory's ->i_op; then lookup_open() calls it with ->i_rwsem held
	shared, as ->atomic_open() without O_CREAT already is;
	dentry_create(), which nfsd uses, is still called with it held
	exclusive.  The flag is a property of the ->i_op instance, so it
	applies to every directory using it.  It is ignored unless the
	filesystem also provides ->atomic_open(): without that method the
	parent is still taken exclusive, so ->create() is never called with
	->i_rwsem held shared.

	IOPF_ATOMIC_OPEN_CREATE_SHARED is transitional: it chooses how the
	parent is locked for a create, and goes away once the VFS no longer
	takes the parent lock for one.

	Setting IOPF_ATOMIC_OPEN_CREATE_SHARED is a statement that the
	filesystem does not rely on the exclusive parent lock to exclude
	other creates in the same directory, or to serialize its own
	per-directory state.  Creates in one directory may then run
	concurrently with each other, with an ->atomic_open() for the same
	or another name without O_CREAT, and with ->lookup() and
	->iterate_shared().  What excludes two creates of the same name is
	the in-lookup protocol: whenever ->atomic_open() is called with
	O_CREAT set and ->i_rwsem held shared, the VFS guarantees that the
	dentry passed is in-lookup (d_in_lookup() is true), so a concurrent
	lookup or create of that name finds it in the in-lookup hash and
	waits for it.  With ->i_rwsem held exclusive - on a superblock that
	has set SB_I_NAMES_COLLIDE, or through dentry_create() - the dentry
	need not be in-lookup and may be a hashed negative one, as before.
	Nothing else changes: ->create(), ->link(), ->unlink(), ->mkdir(),
	->rmdir(), ->mknod(), ->symlink() and ->rename() still take
	->i_rwsem exclusive on the parent, so a create holding it shared
	still excludes them.  It does not exclude ->unlink_shared(), if the
	filesystem provides that as well; see below.

	The flag is also ignored, and the parent taken exclusive, on a
	superblock that has set SB_I_NAMES_COLLIDE in ->s_iflags.  A
	filesystem sets that bit when two names the dcache compares as
	different - byte for byte, or through ->d_hash() and ->d_compare()
	where it has them - can be one directory entry, as they are on a
	case-insensitive server whose client does not fold case in the
	dcache.  The in-lookup protocol is per name as the dcache compares
	names, so there it would exclude a second create of "foo" and not a
	create of "FOO".  A filesystem whose ->d_compare() already makes such
	names equal, as a casefolded directory's does, has no reason to set
	it.  The bit may change while the filesystem is mounted.  The VFS
	reads it for each open before it takes the parent lock, so an open
	that read it clear may still be running shared when it is set.

	A filesystem that sets the flag must publish its result on the
	dentry passed to ->atomic_open() with d_splice_alias() or d_add(),
	which hash it and end the in-lookup state together, inside the
	parent's ->i_dir_seq window, or with d_add_ci(), which leaves it
	unhashed and publishes a dentry of the case-exact name that way.
	It must not hash it with d_rehash().  d_rehash() takes ->d_lock and
	adds the name to the dentry hash, nothing more: the in-lookup state
	does not end, no ->i_dir_seq window is taken, and the waiters are
	not woken.  A concurrent d_alloc_parallel() for the name then
	neither finds a reason to retry nor has anything to wait for, and
	hashes a second dentry for it.  The result is two hashed dentries
	for one name with independent inode state, lookups getting either,
	and nothing in the VFS detecting the condition.

	The guarantee is for the O_CREAT opener only.  Under the shared lock
	an O_CREAT open can run concurrently with a plain open of the same
	name, and only the O_CREAT one is handed an in-lookup dentry; the
	other may be holding a dentry it found hashed and revalidated.  That
	pairing is not new - two plain opens of one name can already reach
	->atomic_open() that way under the same shared lock - and the VFS
	does not close it.  A filesystem setting this flag is responsible
	for it, as it is today.  One way, which NFS uses, is for
	->d_revalidate() to report a cached negative dentry invalid when
	LOOKUP_OPEN is set without LOOKUP_CREATE: lookup_open() then drops
	it and calls d_alloc_parallel() itself, so the plain open is also
	handed an in-lookup dentry, or waits on the creator's.  Calling
	d_alloc_parallel() from ->atomic_open() instead relies on in-lookup
	dentries being allocated under ->i_rwsem, which is planned to
	change.

	->unlink_shared() is an optional second removal method.  For a
	directory whose ->i_op provides it together with ->unlink() and
	->unlink_shared_done(), filename_unlinkat() - the path of unlink(2),
	and of unlinkat(2) without AT_REMOVEDIR - takes ->i_rwsem on the
	parent shared and calls ->unlink_shared() where it used to call
	->unlink().  ->unlink() itself is still called only with the parent
	held exclusive: by the retry described below, and by every other
	caller of vfs_unlink(), none of which changes.  So each of the two
	methods knows the mode of the parent lock from having been called,
	and neither needs to ask the lock.  On a superblock that has set
	SB_I_NAMES_COLLIDE, ->unlink_shared() is not called, for the reason
	given for O_CREAT above: the exclusion it installs is per name as
	the dcache compares names.  The VFS reads the bit before it takes
	the parent lock, so a removal that read it clear may still call
	->unlink_shared() after it is set; a filesystem that sets the bit
	while mounted must hand such a removal back.

	->unlink_shared_done() is the other half of the method and is
	mandatory with it: a directory whose ->i_op has one without the
	other has not opted in, and its removals go to ->unlink() with the
	parent held exclusive as before.  The VFS calls it for every call of
	->unlink_shared() that did not return a positive value, and for no
	other, with ->i_rwsem on the parent still held shared and nothing
	held on the victim, after any delete event for the removal and
	before the parent lock is dropped.  It is where the exclusion
	->unlink_shared() installed for the name ends; see the order of
	notifications below.

	With the parent held shared the VFS still provides this much.  The
	dentry was resolved under this hold of the lock through
	d_alloc_parallel(); it is positive, is not a directory, and is
	referenced until after the method returns.  ->i_rwsem on the victim
	is held exclusive across the call, as it is for ->unlink(), so two
	removals of one inode are serialized.  ->create(), ->link(),
	->symlink(), ->mkdir(), ->mknod(), ->rmdir() and ->rename() still
	take the parent exclusive and so are still excluded.  And while the
	dentry stays hashed, no second dentry for its name can be hashed:
	d_alloc_parallel() returns a hashed dentry rather than creating
	in-lookup state for the name, so a lookup or an open that resolves
	the name calls ->d_revalidate() on that dentry, and a hashed
	positive dentry is never passed to ->atomic_open().

	What the VFS no longer provides is exclusion of ->lookup(), of any
	->atomic_open() called with the parent held shared (with O_CREAT
	too, if the filesystem has set IOPF_ATOMIC_OPEN_CREATE_SHARED), of
	->iterate_shared() or of a second ->unlink_shared() for the same
	name.  Nor does it keep the dentry hashed: a lookup whose
	->d_revalidate() fails d_invalidate()s it, and d_delete() on behalf
	of another remover of the name unhashes it while it is still
	positive.  Nor is anything except a lookup of the name routed
	through ->d_revalidate(): the filesystem's own users of d_lookup()
	and d_alloc_parallel(), readdir priming among them, reach the
	dentry directly.

	The filesystem therefore orders the removal itself against every
	lookup, open and create of the same name that the parent lock no
	longer excludes, and against its own directory listings: a listing
	that the backing store may have produced before a removal took
	effect - one begun before the removal, or while it was in flight -
	must not instantiate the removed name, and must not refresh the
	inode being removed, once that removal may have completed.
	Whatever it anchors that ordering on is found through the dcache,
	which compares names as the filesystem's dentry operations do, byte
	for byte without them.  If the backing store treats two names as
	one object where those do not compare equal, through case folding
	or Unicode normalization, the removal of one name is not ordered
	against a lookup or open of the other that the parent lock no
	longer excludes.  Such removals must be handed back, and where the
	filesystem cannot tell that they can happen, that is a limitation
	it documents.

	One arrangement that provides the ordering is a per-dentry marker
	that ->d_revalidate() waits on, kept by these rules:

	- the marker is installed under ->d_lock, and only on a dentry that
	  in that same ->d_lock section is still hashed, is still positive
	  and has no reference but the caller's.  A resolver that can fail
	  ->d_revalidate(), and so have the dentry unhashed, takes its
	  reference before it consults ->d_revalidate(), so with a count of
	  one every resolver that consults ->d_revalidate() after the marker
	  is installed finds it and waits.  In that same ->d_lock section,
	  after installing the marker and not before, invalidate ->d_seq
	  (write_seqcount_invalidate(), whose barrier orders only earlier
	  stores): an RCU-walk that consulted ->d_revalidate() before the
	  marker was installed then fails its ->d_seq recheck when it takes
	  its reference, and retries in ref-walk, where it finds the marker
	  and waits.  The one resolver this does not reach is an open by
	  file handle, whose reference comes from d_obtain_alias() without a
	  ->d_revalidate(); the exclusive parent lock never excluded it
	  either;
	- RCU-walk consults ->d_revalidate() without a reference and cannot
	  wait, so for the rule above to hold ->d_revalidate() must answer
	  -ECHILD under LOOKUP_RCU while the marker is set, and must never
	  answer 0 under LOOKUP_RCU.  -ECHILD sends the walk to ref-walk,
	  where the consult is repeated with a reference held;
	- none of the filesystem's own paths but the remover's unhashes the
	  dentry while the marker is set, and the remover does so only once
	  the backing store has answered its removal; none hands the dentry
	  to a caller as the result of a lookup without waiting for the
	  marker.  Erring toward leaving the dentry hashed is safe: a later
	  resolver of the name waits for the removal, after which the
	  remover's own d_delete() normally unhashes it, and an open by path
	  that fails on it with -EOPENSTALE is retried by the VFS, first in
	  ref-walk if it was begun in RCU-walk and, if it fails again, with
	  LOOKUP_REVAL, which tells ->d_revalidate() not to trust its cache;
	- the marker is cleared in ->unlink_shared_done() and not before
	  ->unlink_shared() returns, so that it outlasts the delete event the
	  VFS emits for the removal;
	- anything else - a second reference, a dentry that is no longer
	  hashed, a removal the filesystem would carry out by renaming - is
	  answered with UNLINK_NEED_EXCL.

	UNLINK_NEED_EXCL is a positive value that only ->unlink_shared() may
	return.  It says that this removal cannot be made with the parent
	held shared and that nothing was done.  The VFS then drops the lock,
	takes it exclusive, looks the name up again and calls ->unlink(),
	which cannot ask again.  A directory found by the shared lookup is
	sent the same way without the method being called, unless the name
	ends in a slash, which is answered -EISDIR from its type alone; so
	->unlink_shared() is never passed a directory.  A removal that the
	method hands back runs the permission check, the audit record, both
	security hooks and both delegation checks once in each pass, and its
	name is looked up twice.

	A lookup that holds the parent shared can now wait in
	->d_revalidate() for such a removal, for as long as the removal
	takes; before, it would have waited for the parent lock instead.  If
	the filesystem's wait there is uninterruptible, so is this one.

	The order of notifications for one name is what
	->unlink_shared_done() keeps.  ->unlink_shared() returns with its
	exclusion for the name still installed; the VFS emits the delete
	event for the removal, then calls ->unlink_shared_done() to end it,
	then drops the parent lock.  A creator of the name that the call
	releases emits its create event after that delete event whichever
	mode it holds the parent in: one that takes the parent exclusive is
	waiting for a lock that is held until after the call, and one that
	holds it shared - an O_CREAT ->atomic_open(), once the filesystem
	has set IOPF_ATOMIC_OPEN_CREATE_SHARED - is waiting for the
	exclusion itself.  A filesystem that ends its exclusion inside
	->unlink_shared() instead loses that order against a creator of the
	second kind: the create event can come before the delete event of
	the removal it waited for, and a watcher then concludes that the
	name is gone while it exists.  That is why ->unlink_shared_done() is
	not optional.

	What this does not order is a creator that never met the exclusion.
	When d_delete_notify() unhashes the name before it emits the event -
	when another reference is held at the delete, or the
	fs.dentry-negative sysctl is set - a resolver that arrives after the
	unhash finds no dentry for the name, makes its own, and is ordered
	against the removal by nothing; the window is the span of the event
	itself.  Nor does any of this decide what a creator released by
	->unlink_shared_done() goes on to resolve: the dentry it was waiting
	on may still be positive, and whether ->d_revalidate() then sends it
	to the backing store is the filesystem's answer and not the VFS's.

	->unlink_shared() is meant as a transitional interface.  It lets one
	filesystem at a time stop relying on the exclusive parent lock for
	removal, and ->unlink() and ->unlink_shared() can become one again
	once removal locks the target dentry and not the parent.

See Documentation/filesystems/directory-locking.rst for more detailed discussion
of the locking scheme for directory operations.

xattr_handler operations
========================

prototypes::

	bool (*list)(struct dentry *dentry);
	int (*get)(const struct xattr_handler *handler, struct dentry *dentry,
		   struct inode *inode, const char *name, void *buffer,
		   size_t size);
	int (*set)(const struct xattr_handler *handler,
                   struct mnt_idmap *idmap,
                   struct dentry *dentry, struct inode *inode, const char *name,
                   const void *buffer, size_t size, int flags);

locking rules:
	all may block

=====		==============
ops		i_rwsem(inode)
=====		==============
list:		no
get:		no
set:		exclusive
=====		==============

super_operations
================

prototypes::

	struct inode *(*alloc_inode)(struct super_block *sb);
	void (*free_inode)(struct inode *);
	void (*destroy_inode)(struct inode *);
	void (*dirty_inode) (struct inode *, int flags);
	int (*write_inode) (struct inode *, struct writeback_control *wbc);
	int (*drop_inode) (struct inode *);
	void (*evict_inode) (struct inode *);
	void (*put_super) (struct super_block *);
	int (*sync_fs)(struct super_block *sb, int wait);
	int (*freeze_fs) (struct super_block *);
	int (*unfreeze_fs) (struct super_block *);
	int (*statfs) (struct dentry *, struct kstatfs *);
	void (*umount_begin) (struct super_block *);
	int (*show_options)(struct seq_file *, struct dentry *);
	ssize_t (*quota_read)(struct super_block *, int, char *, size_t, loff_t);
	ssize_t (*quota_write)(struct super_block *, int, const char *, size_t, loff_t);

locking rules:
	All may block [not true, see below]

======================	============	========================
ops			s_umount	note
======================	============	========================
alloc_inode:
free_inode:				called from RCU callback
destroy_inode:
dirty_inode:
write_inode:
drop_inode:				!!!inode->i_lock!!!
evict_inode:
put_super:		write
sync_fs:		read
freeze_fs:		write
unfreeze_fs:		write
statfs:			maybe(read)	(see below)
umount_begin:		no
show_options:		no		(namespace_sem)
quota_read:		no		(see below)
quota_write:		no		(see below)
======================	============	========================

->statfs() has s_umount (shared) when called by ustat(2) (native or
compat), but that's an accident of bad API; s_umount is used to pin
the superblock down when we only have dev_t given us by userland to
identify the superblock.  Everything else (statfs(), fstatfs(), etc.)
doesn't hold it when calling ->statfs() - superblock is pinned down
by resolving the pathname passed to syscall.

->quota_read() and ->quota_write() functions are both guaranteed to
be the only ones operating on the quota file by the quota code (via
dqio_sem) (unless an admin really wants to screw up something and
writes to quota files with quotas on). For other details about locking
see also dquot_operations section.

file_system_type
================

prototypes::

	void (*kill_sb) (struct super_block *);

locking rules:

=======		=========
ops		may block
=======		=========
kill_sb		yes
=======		=========

->kill_sb() takes a write-locked superblock, does all shutdown work on it,
unlocks and drops the reference.

address_space_operations
========================
prototypes::

	int (*read_folio)(struct file *, struct folio *);
	int (*writepages)(struct address_space *, struct writeback_control *);
	bool (*dirty_folio)(struct address_space *, struct folio *folio);
	void (*readahead)(struct readahead_control *);
	int (*write_begin)(const struct kiocb *, struct address_space *mapping,
				loff_t pos, unsigned len,
				struct folio **foliop, void **fsdata);
	int (*write_end)(const struct kiocb *, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned copied,
				struct folio *folio, void *fsdata);
	sector_t (*bmap)(struct address_space *, sector_t);
	void (*invalidate_folio) (struct folio *, size_t start, size_t len);
	bool (*release_folio)(struct folio *, gfp_t);
	void (*free_folio)(struct folio *);
	int (*direct_IO)(struct kiocb *, struct iov_iter *iter);
	int (*migrate_folio)(struct address_space *, struct folio *dst,
			struct folio *src, enum migrate_mode);
	int (*launder_folio)(struct folio *);
	bool (*is_partially_uptodate)(struct folio *, size_t from, size_t count);
	int (*error_remove_folio)(struct address_space *, struct folio *);
	int (*swap_activate)(struct swap_info_struct *sis, struct file *f, sector_t *span)
	int (*swap_deactivate)(struct file *);

locking rules:
	All except dirty_folio and free_folio may block

======================	======================== =========	===============
ops			folio locked		 i_rwsem	invalidate_lock
======================	======================== =========	===============
read_folio:		yes, unlocks				shared
writepages:
dirty_folio:		maybe
readahead:		yes, unlocks				shared
write_begin:		locks the folio		 exclusive
write_end:		yes, unlocks		 exclusive
bmap:
invalidate_folio:	yes					exclusive
release_folio:		yes
free_folio:		yes
direct_IO:
migrate_folio:		yes (both)
launder_folio:		yes
is_partially_uptodate:	yes
error_remove_folio:	yes
swap_activate:		no
swap_deactivate:	no
======================	======================== =========	===============

->write_begin(), ->write_end() and ->read_folio() may be called from
the request handler (/dev/loop).

->read_folio() unlocks the folio, either synchronously or via I/O
completion.

->readahead() unlocks the folios that I/O is attempted on like ->read_folio().

->writepages() is used for periodic writeback and for syscall-initiated
sync operations.  The address_space should start I/O against at least
``*nr_to_write`` pages.  ``*nr_to_write`` must be decremented for each page
which is written.  The address_space implementation may write more (or less)
pages than ``*nr_to_write`` asks for, but it should try to be reasonably close.
If nr_to_write is NULL, all dirty pages must be written.

writepages should _only_ write pages which are present in
mapping->i_pages.

->dirty_folio() is called from various places in the kernel when
the target folio is marked as needing writeback.  The folio cannot be
truncated because either the caller holds the folio lock, or the caller
has found the folio while holding the page table lock which will block
truncation.

->bmap() is currently used by legacy ioctl() (FIBMAP) provided by some
filesystems and by the swapper. The latter will eventually go away.  Please,
keep it that way and don't breed new callers.

->invalidate_folio() is called when the filesystem must attempt to drop
some or all of the buffers from the page when it is being truncated. It
returns zero on success.  The filesystem must exclusively acquire
invalidate_lock before invalidating page cache in truncate / hole punch
path (and thus calling into ->invalidate_folio) to block races between page
cache invalidation and page cache filling functions (fault, read, ...).

->release_folio() is called when the MM wants to make a change to the
folio that would invalidate the filesystem's private data.  For example,
it may be about to be removed from the address_space or split.  The folio
is locked and not under writeback.  It may be dirty.  The gfp parameter
is not usually used for allocation, but rather to indicate what the
filesystem may do to attempt to free the private data.  The filesystem may
return false to indicate that the folio's private data cannot be freed.
If it returns true, it should have already removed the private data from
the folio.  If a filesystem does not provide a ->release_folio method,
the pagecache will assume that private data is buffer_heads and call
try_to_free_buffers().

->free_folio() is called when the kernel has dropped the folio
from the page cache.

->launder_folio() may be called prior to releasing a folio if
it is still found to be dirty. It returns zero if the folio was successfully
cleaned, or an error value if not. Note that in order to prevent the folio
getting mapped back in and redirtied, it needs to be kept locked
across the entire operation.

->swap_activate() will be called to prepare the given file for swap.  It
should perform any validation and preparation necessary to ensure that
writes can be performed with minimal memory allocation.  It should call
add_swap_extent(), or the helper iomap_swapfile_activate(), and return
the number of extents added.  If IO should be submitted through
the file system it should call swap_fs_activate, otherwise IO will be
submitted directly to the block device ``sis->bdev``.

->swap_deactivate() will be called in the sys_swapoff()
path after ->swap_activate() returned success.

file_lock_operations
====================

prototypes::

	void (*fl_copy_lock)(struct file_lock *, struct file_lock *);
	void (*fl_release_private)(struct file_lock *);


locking rules:

===================	=============	=========
ops			inode->i_lock	may block
===================	=============	=========
fl_copy_lock:		yes		no
fl_release_private:	maybe		maybe[1]_
===================	=============	=========

.. [1]:
   ->fl_release_private for flock or POSIX locks is currently allowed
   to block. Leases however can still be freed while the i_lock is held and
   so fl_release_private called on a lease should not block.

lock_manager_operations
=======================

prototypes::

	void (*lm_notify)(struct file_lock *);  /* unblock callback */
	int (*lm_grant)(struct file_lock *, struct file_lock *, int);
	void (*lm_break)(struct file_lock *); /* break_lease callback */
	int (*lm_change)(struct file_lock **, int);
	bool (*lm_breaker_owns_lease)(struct file_lock *);
        bool (*lm_lock_expirable)(struct file_lock *);
        void (*lm_expire_lock)(void);
        bool (*lm_breaker_timedout)(struct file_lease *);

locking rules:

======================	=============	=================	=========
ops			   flc_lock  	blocked_lock_lock	may block
======================	=============	=================	=========
lm_notify:		no      	yes			no
lm_grant:		no		no			no
lm_break:		yes		no			no
lm_change		yes		no			no
lm_breaker_owns_lease:	yes     	no			no
lm_lock_expirable	yes		no			no
lm_expire_lock		no		no			yes
lm_open_conflict	yes		no			no
lm_breaker_timedout     yes             no                      no
======================	=============	=================	=========

block_device_operations
=======================
prototypes::

	int (*open) (struct block_device *, fmode_t);
	int (*release) (struct gendisk *, fmode_t);
	int (*ioctl) (struct block_device *, fmode_t, unsigned, unsigned long);
	int (*compat_ioctl) (struct block_device *, fmode_t, unsigned, unsigned long);
	int (*direct_access) (struct block_device *, sector_t, void **,
				unsigned long *);
	void (*unlock_native_capacity) (struct gendisk *);
	int (*getgeo)(struct gendisk *, struct hd_geometry *);
	void (*swap_slot_free_notify) (struct block_device *, unsigned long);

locking rules:

======================= ===================
ops			open_mutex
======================= ===================
open:			yes
release:		yes
ioctl:			no
compat_ioctl:		no
direct_access:		no
unlock_native_capacity:	no
getgeo:			no
swap_slot_free_notify:	no	(see below)
======================= ===================

swap_slot_free_notify is called with swap_lock and sometimes the page lock
held.


file_operations
===============

prototypes::

	loff_t (*llseek) (struct file *, loff_t, int);
	ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
	ssize_t (*read_iter) (struct kiocb *, struct iov_iter *);
	ssize_t (*write_iter) (struct kiocb *, struct iov_iter *);
	int (*iopoll) (struct kiocb *kiocb, bool spin);
	int (*iterate_shared) (struct file *, struct dir_context *);
	__poll_t (*poll) (struct file *, struct poll_table_struct *);
	long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
	long (*compat_ioctl) (struct file *, unsigned int, unsigned long);
	int (*mmap) (struct file *, struct vm_area_struct *);
	int (*open) (struct inode *, struct file *);
	int (*flush) (struct file *);
	int (*release) (struct inode *, struct file *);
	int (*fsync) (struct file *, loff_t start, loff_t end, int datasync);
	int (*fasync) (int, struct file *, int);
	int (*lock) (struct file *, int, struct file_lock *);
	unsigned long (*get_unmapped_area)(struct file *, unsigned long,
			unsigned long, unsigned long, unsigned long);
	int (*check_flags)(int);
	int (*flock) (struct file *, int, struct file_lock *);
	ssize_t (*splice_write)(struct pipe_inode_info *, struct file *, loff_t *,
			size_t, unsigned int);
	ssize_t (*splice_read)(struct file *, loff_t *, struct pipe_inode_info *,
			size_t, unsigned int);
	int (*setlease)(struct file *, long, struct file_lock **, void **);
	long (*fallocate)(struct file *, int, loff_t, loff_t);
	void (*show_fdinfo)(struct seq_file *m, struct file *f);
	unsigned (*mmap_capabilities)(struct file *);
	ssize_t (*copy_file_range)(struct file *, loff_t, struct file *,
			loff_t, size_t, unsigned int);
	loff_t (*remap_file_range)(struct file *file_in, loff_t pos_in,
			struct file *file_out, loff_t pos_out,
			loff_t len, unsigned int remap_flags);
	int (*fadvise)(struct file *, loff_t, loff_t, int);

locking rules:
	All may block.

->llseek() locking has moved from llseek to the individual llseek
implementations.  If your fs is not using generic_file_llseek, you
need to acquire and release the appropriate locks in your ->llseek().
For many filesystems, it is probably safe to acquire the inode
mutex or just to use i_size_read() instead.
Note: this does not protect the file->f_pos against concurrent modifications
since this is something the userspace has to take care about.

->iterate_shared() is called with i_rwsem held for reading, and with the
file f_pos_lock held exclusively

->fasync() is responsible for maintaining the FASYNC bit in filp->f_flags.
Most instances call fasync_helper(), which does that maintenance, so it's
not normally something one needs to worry about.  Return values > 0 will be
mapped to zero in the VFS layer.

->readdir() and ->ioctl() on directories must be changed. Ideally we would
move ->readdir() to inode_operations and use a separate method for directory
->ioctl() or kill the latter completely. One of the problems is that for
anything that resembles union-mount we won't have a struct file for all
components. And there are other reasons why the current interface is a mess...

->read on directories probably must go away - we should just enforce -EISDIR
in sys_read() and friends.

->setlease operations should call generic_setlease() before or after setting
the lease within the individual filesystem to record the result of the
operation

->fallocate implementation must be really careful to maintain page cache
consistency when punching holes or performing other operations that invalidate
page cache contents. Usually the filesystem needs to call
truncate_inode_pages_range() to invalidate relevant range of the page cache.
However the filesystem usually also needs to update its internal (and on disk)
view of file offset -> disk block mapping. Until this update is finished, the
filesystem needs to block page faults and reads from reloading now-stale page
cache contents from the disk. Since VFS acquires mapping->invalidate_lock in
shared mode when loading pages from disk (filemap_fault(), filemap_read(),
readahead paths), the fallocate implementation must take the invalidate_lock to
prevent reloading.

->copy_file_range and ->remap_file_range implementations need to serialize
against modifications of file data while the operation is running. For
blocking changes through write(2) and similar operations inode->i_rwsem can be
used. To block changes to file contents via a memory mapping during the
operation, the filesystem must take mapping->invalidate_lock to coordinate
with ->page_mkwrite.

dquot_operations
================

prototypes::

	int (*write_dquot) (struct dquot *);
	int (*acquire_dquot) (struct dquot *);
	int (*release_dquot) (struct dquot *);
	int (*mark_dirty) (struct dquot *);
	int (*write_info) (struct super_block *, int);

These operations are intended to be more or less wrapping functions that ensure
a proper locking wrt the filesystem and call the generic quota operations.

What filesystem should expect from the generic quota functions:

==============	============	=========================
ops		FS recursion	Held locks when called
==============	============	=========================
write_dquot:	yes		dqonoff_sem or dqptr_sem
acquire_dquot:	yes		dqonoff_sem or dqptr_sem
release_dquot:	yes		dqonoff_sem or dqptr_sem
mark_dirty:	no		-
write_info:	yes		dqonoff_sem
==============	============	=========================

FS recursion means calling ->quota_read() and ->quota_write() from superblock
operations.

More details about quota locking can be found in fs/quota/dquot.c.

vm_operations_struct
====================

prototypes::

	void (*open)(struct vm_area_struct *);
	void (*close)(struct vm_area_struct *);
	vm_fault_t (*fault)(struct vm_fault *);
	vm_fault_t (*huge_fault)(struct vm_fault *, unsigned int order);
	vm_fault_t (*map_pages)(struct vm_fault *, pgoff_t start, pgoff_t end);
	vm_fault_t (*page_mkwrite)(struct vm_area_struct *, struct vm_fault *);
	vm_fault_t (*pfn_mkwrite)(struct vm_area_struct *, struct vm_fault *);
	int (*access)(struct vm_area_struct *, unsigned long, void*, int, int);

locking rules:

=============	==========	===========================
ops		mmap_lock	PageLocked(page)
=============	==========	===========================
open:		write
close:		read/write
fault:		read		can return with page locked
huge_fault:	maybe-read
map_pages:	maybe-read
page_mkwrite:	read		can return with page locked
pfn_mkwrite:	read
access:		read
=============	==========	===========================

->fault() is called when a previously not present pte is about to be faulted
in. The filesystem must find and return the page associated with the passed in
"pgoff" in the vm_fault structure. If it is possible that the page may be
truncated and/or invalidated, then the filesystem must lock invalidate_lock,
then ensure the page is not already truncated (invalidate_lock will block
subsequent truncate), and then return with VM_FAULT_LOCKED, and the page
locked. The VM will unlock the page.

->huge_fault() is called when there is no PUD or PMD entry present.  This
gives the filesystem the opportunity to install a PUD or PMD sized page.
Filesystems can also use the ->fault method to return a PMD sized page,
so implementing this function may not be necessary.  In particular,
filesystems should not call filemap_fault() from ->huge_fault().
The mmap_lock may not be held when this method is called.

->map_pages() is called when VM asks to map easy accessible pages.
Filesystem should find and map pages associated with offsets from "start_pgoff"
till "end_pgoff". ->map_pages() is called with the RCU lock held and must
not block.  If it's not possible to reach a page without blocking,
filesystem should skip it. Filesystem should use set_pte_range() to setup
page table entry. Pointer to entry associated with the page is passed in
"pte" field in vm_fault structure. Pointers to entries for other offsets
should be calculated relative to "pte".

->page_mkwrite() is called when a previously read-only pte is about to become
writeable. The filesystem again must ensure that there are no
truncate/invalidate races or races with operations such as ->remap_file_range
or ->copy_file_range, and then return with the page locked. Usually
mapping->invalidate_lock is suitable for proper serialization. If the page has
been truncated, the filesystem should not look up a new page like the ->fault()
handler, but simply return with VM_FAULT_NOPAGE, which will cause the VM to
retry the fault.

->pfn_mkwrite() is the same as page_mkwrite but when the pte is
VM_PFNMAP or VM_MIXEDMAP with a page-less entry. Expected return is
VM_FAULT_NOPAGE. Or one of the VM_FAULT_ERROR types. The default behavior
after this call is to make the pte read-write, unless pfn_mkwrite returns
an error.

->access() is called when get_user_pages() fails in
access_process_vm(), typically used to debug a process through
/proc/pid/mem or ptrace.  This function is needed only for
VM_IO | VM_PFNMAP VMAs.

--------------------------------------------------------------------------------

			Dubious stuff

(if you break something or notice that it is broken and do not fix it yourself
- at least put it here)
