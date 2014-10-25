/*
 * Author     : Lokesh Lagudu
 * Mentor     : Erez Zadok
 * University : Stony Brook University
 */

#include "wrapfs.h"

/* The dentry cache is just so we have properly sized dentries */
static struct kmem_cache *wrapfs_dentry_cachep;

int __realloc_dentry_private_data(struct dentry *dentry)
{
	struct wrapfs_dentry_info *info = WRAPFS_D(dentry);
	void *p;
	int size;

	BUG_ON(!info);
	size = sizeof(struct path) * 2;
	p = krealloc(info->lower_paths, size, GFP_ATOMIC);
	if (unlikely(!p))
		return -ENOMEM;

	info->lower_paths = p;
	atomic_set(&info->generation,
	atomic_read(&WRAPFS_SB(dentry->d_sb)->generation));

	memset(info->lower_paths, 0, size);
	return 0;
}

int wrapfs_init_dentry_cache(void)
{
	wrapfs_dentry_cachep =
		kmem_cache_create("wrapfs_dentry",
				sizeof(struct wrapfs_dentry_info),
			0, SLAB_RECLAIM_ACCOUNT, NULL);
	return wrapfs_dentry_cachep ? 0 : -ENOMEM;
}

void wrapfs_destroy_dentry_cache(void)
{
	if (wrapfs_dentry_cachep)
		kmem_cache_destroy(wrapfs_dentry_cachep);
}
void free_dentry_private_data(struct dentry *dentry)
{
	if (!dentry || !dentry->d_fsdata)
		return;
	kmem_cache_free(wrapfs_dentry_cachep, dentry->d_fsdata);
	dentry->d_fsdata = NULL;
}

/* allocate new dentry private data */
int new_dentry_private_data(struct dentry *dentry)
{
	struct wrapfs_dentry_info *info = WRAPFS_D(dentry);

	/* use zalloc to init dentry_info.lower_path */
	info = kmem_cache_alloc(wrapfs_dentry_cachep, GFP_ATOMIC);
	if (!info)
		return -ENOMEM;
	/*spin_lock_init(&info->lock);*/

	info->lower_paths = NULL;
	dentry->d_fsdata = info;

	/* Realloc_dentry_private_data*/
	if (!__realloc_dentry_private_data(dentry))
		return 0;

	free_dentry_private_data(dentry);
	return -ENOMEM;
}

static int wrapfs_inode_test(struct inode *inode, void *candidate_lower_inode)
{
	struct inode *current_lower_inode = wrapfs_lower_inode(inode);
	if (current_lower_inode == (struct inode *)candidate_lower_inode)
		return 1; /* found a match */
	else
		return 0; /* no match */
}

static int wrapfs_inode_set(struct inode *inode, void *lower_inode)
{
	/* we do actual inode initialization in wrapfs_iget */
	return 0;
}
struct inode *wrapfs_new_iget(struct super_block *sb, unsigned long ino)
{
	int size;
	struct wrapfs_inode_info *info;
	struct inode *inode;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	info = WRAPFS_I(inode);
	memset(info, 0, offsetof(struct wrapfs_inode_info, vfs_inode));
	atomic_set(&info->generation,
		atomic_read(&WRAPFS_SB(inode->i_sb)->generation));
	spin_lock_init(&info->rdlock);
	info->rdcount = 1;
	info->hashsize = -1;
	INIT_LIST_HEAD(&info->readdircache);

	size = 2 * sizeof(struct inode *);
	info->lower_inodes = kmalloc(size, GFP_KERNEL);
	if (unlikely(!info->lower_inodes)) {
		printk(KERN_CRIT "u2fs: no kernel memory when allocating "
				"lower-pointer array!\n");
		iget_failed(inode);
		return ERR_PTR(-ENOMEM);
	}

	inode->i_version++;
	inode->i_op = &wrapfs_main_iops;
	inode->i_fop = &wrapfs_main_fops;

	inode->i_mapping->a_ops = &wrapfs_aops;

	inode->i_atime.tv_sec = inode->i_atime.tv_nsec = 0;
	inode->i_mtime.tv_sec = inode->i_mtime.tv_nsec = 0;
	inode->i_ctime.tv_sec = inode->i_ctime.tv_nsec = 0;
	unlock_new_inode(inode);

	return inode;
}
struct inode *wrapfs_iget(struct super_block *sb, struct inode *lower_inode)
{
	struct wrapfs_inode_info *info;
	struct inode *inode; /* the new inode to return */
	int err;

	inode = iget5_locked(sb, /* our superblock */
				/*
				* hashval: we use inode number, but we can
				* also use "(unsigned long)lower_inode"
				* instead.
				*/
			lower_inode->i_ino, /* hashval */
			wrapfs_inode_test, /* inode comparison function */
			wrapfs_inode_set, /* inode init function */
			lower_inode); /* data passed to test+set fxns */
	if (!inode) {
		err = -EACCES;
		iput(lower_inode);
		return ERR_PTR(err);
	}
	/* if found a cached inode, then just return it */
	if (!(inode->i_state & I_NEW))
		return inode;

	/* initialize new inode */
	info = WRAPFS_I(inode);

	inode->i_ino = lower_inode->i_ino;
	if (!igrab(lower_inode)) {
		err = -ESTALE;
		return ERR_PTR(err);
	}
	wrapfs_set_lower_inode(inode, lower_inode);

	inode->i_version++;

	/* use different set of inode ops for symlinks & directories */
	if (S_ISDIR(lower_inode->i_mode))
		inode->i_op = &wrapfs_dir_iops;
	else if (S_ISLNK(lower_inode->i_mode))
		inode->i_op = &wrapfs_symlink_iops;
	else
		inode->i_op = &wrapfs_main_iops;

	/* use different set of file ops for directories */
	if (S_ISDIR(lower_inode->i_mode))
		inode->i_fop = &wrapfs_dir_fops;
	else
		inode->i_fop = &wrapfs_main_fops;

	inode->i_mapping->a_ops = &wrapfs_aops;

	inode->i_atime.tv_sec = 0;
	inode->i_atime.tv_nsec = 0;
	inode->i_mtime.tv_sec = 0;
	inode->i_mtime.tv_nsec = 0;
	inode->i_ctime.tv_sec = 0;
	inode->i_ctime.tv_nsec = 0;

	/* properly initialize special inodes */
	if (S_ISBLK(lower_inode->i_mode) || S_ISCHR(lower_inode->i_mode) ||
		S_ISFIFO(lower_inode->i_mode) || S_ISSOCK(lower_inode->i_mode))
		init_special_inode(inode, lower_inode->i_mode,
			lower_inode->i_rdev);

	/* all well, copy inode attributes */
	fsstack_copy_attr_all(inode, lower_inode);
	fsstack_copy_inode_size(inode, lower_inode);

	unlock_new_inode(inode);
	return inode;
}

/*
 * Main driver function for wrapfs's lookup.
 *
 * Returns: NULL (ok), ERR_PTR if an error occurred.
 * Fills in lower_parent_path with <dentry,mnt> on success.
 */
static struct dentry *__wrapfs_lookup(struct dentry *dentry, int flags,
				struct path *lower_parent_path, int idx)
{
	int err = 0;
	struct vfsmount *lower_dir_mnt = NULL;
	struct dentry *lower_dir_dentry = NULL;
	struct dentry *lower_dentry;
	const char *name;
	struct path lower_path;
	struct qstr this;

	/* must initialize dentry operations */
	d_set_d_op(dentry, &wrapfs_dops);

	if (IS_ROOT(dentry))
		goto out;

	name = dentry->d_name.name;

	/* now start the actual lookup procedure */
	lower_dir_dentry = lower_parent_path->dentry;
	lower_dir_mnt = lower_parent_path->mnt;

	/* Use vfs_path_lookup to check if the dentry exists or not */
	err = vfs_path_lookup(lower_dir_dentry, lower_dir_mnt, name, 0,
				&lower_path);

	/* no error: handle positive dentries */
	if (!err) {
		wrapfs_set_lower_path_idx(dentry, &lower_path, idx);
		err = wrapfs_interpose(dentry, dentry->d_sb, &lower_path);
		if (err) /* path_put underlying path on error */
			wrapfs_put_reset_lower_path_idx(dentry, idx);

		goto out;
	}

	/*
	* We don't consider ENOENT an error, and we want to return a
	* negative dentry.
	*/
	if (err && err != -ENOENT)
		goto out;

	/* instatiate a new negative dentry */
	this.name = name;
	this.len = strlen(name);
	this.hash = full_name_hash(this.name, this.len);
	lower_dentry = d_lookup(lower_dir_dentry, &this);
	if (lower_dentry)
		goto setup_lower;
	lower_dentry = d_alloc(lower_dir_dentry, &this);

	if (!lower_dentry) {
		err = -ENOMEM;
		goto out;
	}
	d_add(lower_dentry, NULL); /* instantiate and hash */
setup_lower:
	lower_path.dentry = lower_dentry;
	lower_path.mnt = mntget(lower_dir_mnt);
	wrapfs_set_lower_path_idx(dentry, &lower_path, idx);
	mntput(lower_dir_mnt);

	/*
	* If the intent is to create a file, then don't return an error, so
	* the VFS will continue the process of making this negative dentry
	* into a positive one.
	*/
	if (flags & (LOOKUP_CREATE|LOOKUP_RENAME_TARGET))
		err = 0;

out:
	return ERR_PTR(err);
}


struct dentry *wrapfs_lookup(struct inode *dir, struct dentry *dentry,
				struct nameidata *nd)
{
	struct dentry *ret = NULL;
	struct dentry *parent = NULL;
	struct path lower_parent_path;
	int err = 0, i = 0;
	BUG_ON(!nd);
	err = new_dentry_private_data(dentry);
	if (err) {
		ret = ERR_PTR(err);
		goto out;
	}
	parent = dget_parent(dentry);
	for (i = 0; i <= 1; i++) {
		wrapfs_get_lower_path_idx(parent, &lower_parent_path, i);

		if (!lower_parent_path.dentry ||
			!lower_parent_path.dentry->d_inode)
			continue;
		if (!S_ISDIR(lower_parent_path.dentry->d_inode->i_mode))
			continue;
		ret = __wrapfs_lookup(dentry, nd->flags,
					&lower_parent_path, i);

		if (IS_ERR(ret))
			continue;
		/*goto out_free;*/

		if (ret)
			dentry = ret;
		if (dentry->d_inode)
			fsstack_copy_attr_times(dentry->d_inode,
			wrapfs_lower_inode_idx(dentry->d_inode, i));
		/* update parent directory's atime */
		fsstack_copy_attr_atime(parent->d_inode,
			wrapfs_lower_inode_idx(parent->d_inode, i));
		goto out;
	}
	/*goto out;*/

	path_put_lowers_all(dentry, false);
	kfree(WRAPFS_D(dentry)->lower_paths);
	WRAPFS_D(dentry)->lower_paths = NULL;
out:
	wrapfs_put_lower_path(parent, &lower_parent_path);
	dput(parent);
	return ret;
}


