#include "svfs.h"
#include "dirent.h"
#include "slab_allocator.h"
#include "sys/disk/disk.h"

fs_ops_t svfs_fsops = { .fs_init = svfs_init,
                        .read_vnode = svfs_read_vnode};

static vnode_ops_t svfs_dir_vops = {.read = NULL,
                                    .write = NULL,
                                    .mknod = svfs_mknod,
                                    .mkdir = svfs_mkdir,
                                    .lookup = svfs_lookup,
                                    .readdir = svfs_readdir,
                                    .read_pframe = svfs_read_pframe,
                                    .write_pframe = svfs_write_pframe,
                                    .flush_pframe = svfs_write_pframe}; // used for mobj_flush

static vnode_ops_t svfs_file_vops = {.read = svfs_read,
                                     .write = svfs_write,
                                     .mknod = NULL,
                                     .mkdir = NULL,
                                     .lookup = NULL,
                                     .readdir = NULL,
                                     .read_pframe = svfs_read_pframe,
                                     .write_pframe = svfs_write_pframe,
                                     .flush_pframe = svfs_write_pframe}; // used for mobj_flush

// declare variables later used
static svfs_t svfs; 
static slab_allocator_t svfs_node_allocator;
static slab_allocator_t svfs_pframe_allocator;
static mobj_ops_t svfs_mobj_ops = {vnode_read_pframe, vnode_write_pframe, vnode_flush_pframe};

/*
 * Initializes the svfs instance and the fs struct
 */
void svfs_init() {
    fs_t *fs = &vfs_root_fs;
    char buf[SV_BLOCK_SIZE];
    svfs.svfs_fs = fs;
    spinlock_init(&svfs.svfs_mutex);

    // reads block 0 from disk into svfs_super block
    read_disk_block(SV_SUPER_BLOCK, buf);
    
    svfs.svfs_super = *(svfs_super_t *) buf;
    long result = svfs_check_super(&svfs.svfs_super);
    if (result != 0) { 
        return;
    }

    slab_allocator_init(&svfs_node_allocator, sizeof(svfs_node_t));
    slab_allocator_init(&svfs_pframe_allocator, sizeof(pframe_t));

    // fills in fs struct
    fs->fs_ops = &svfs_fsops;
    fs->vnode_allocator = &svfs_node_allocator;
    list_init(&fs->vnode_list);
    spinlock_init(&fs->vnode_list_mutex);
    svfs_node_t *root = slab_obj_alloc(svfs_node_allocator);
    root->inode.svi_number = svfs.svfs_super.svs_root_inode;
    svfs_read_vnode(fs, &root->vnode);
    fs->fs_root = &root->vnode;
}

/*
 * Initialize a svfs_node/vnode by reading a file from disk and setting the fields
 */
void svfs_read_vnode(fs_t *fs, vnode_t *vn) {
    svfs_node_t *node = (svfs_node_t *) vn; 
    long ino = node->inode.svi_number; 
    char buf[SV_BLOCK_SIZE]; 

    read_disk_block(ino + 1, buf); 
    node->inode = *(svfs_inode_t *) buf; // sets node->inode to the inode in buf 
    node->dirtied_inode = 0; 
    vn->vn_fs = fs; 
    vn->parent = node;
    vn->vn_len = node->inode.svi_un.svi_size; 
    vn->vn_mode = REGULAR; // default regular file

    // checks if the vnode is a directory
    if (vn->vn_len >= (long) sizeof(dirent_t) && node->inode.svi_direct_blocks[0] > 0) { 
        read_disk_block(node->inode.svi_direct_blocks[0], buf); 
        dirent_t *first = (dirent_t *) buf;
        if (first->d_ino == ino && first->d_name[0] == '.' && first->d_name[1] == '\0') { // every directory has a . entry first
            vn->vn_mode = DIRECTORY; 
        }
    }
    if (vn->vn_mode == DIRECTORY) { 
        vn->vn_ops = &svfs_dir_vops; 
    } else {
        vn->vn_ops = &svfs_file_vops; 
    }

   
    mobj_create(&vn->vn_mobj, &svfs_mobj_ops); 
    vn->vn_mobj.parent = vn; 

    // adds the vnode to the vnode list of file system
    list_link_init(&vn->vn_link, vn);
    spinlock_lock(&fs->vnode_list_mutex);
    list_insert_back(&fs->vnode_list, &vn->vn_link);
    spinlock_unlock(&fs->vnode_list_mutex);
}


/*
 * Check the super block for magic number and inodes
 */
long svfs_check_super(svfs_super_t *super) {
    // if magic number is right and there are inodes: return 0
    if (super->svs_magic == SV_MAGIC_NO && super->svs_num_inodes > 0) {
        return 0;
    }
    return -1;
}

/*
 * Attach an inode to a newly created file. 
 */
long svfs_alloc_inode(svfs_t *svfs) {
    char buf[SV_BLOCK_SIZE];

    spinlock_lock(&svfs->svfs_mutex); 

    long ino = svfs->svfs_super.svs_free_inode; 
    if (ino <= 0) {
        spinlock_unlock(&svfs->svfs_mutex); // unlock & do nothing here if no free inode
        return -1;
    }


    read_disk_block(ino + 1, buf);
    svfs->svfs_super.svs_free_inode = ((svfs_inode_t *) buf)->svi_un.svi_next_free; // the pointer for the the next free inode points from this inode to the next free inode

    for (long i = 0; i < SV_BLOCK_SIZE; i++) {
        buf[i] = 0;
    }
    *(svfs_super_t *) buf = svfs->svfs_super; // put the changes back into super block
    write_disk_block(SV_SUPER_BLOCK, buf);

    spinlock_unlock(&svfs->svfs_mutex); 
    return ino;
}

/*
 * Find a dirent from a svfs_node with the input name/namelen and pass it to the ent pointer. 
 */
long svfs_find_dirent(svfs_node_t *dir, const char *name, long namelen,
                      svfs_node_t *ent) {
    long dsize = sizeof(dirent_t);
    dirent_t d;

    if (namelen <= 0 || namelen > NAME_LEN) { // if name is too short or too long return -1
        return -1;
    }

    for (long i = 0; i + dsize <= dir->vnode.vn_len; i += dsize) { // go through each dirent in the directory
        long read = svfs_read(&dir->vnode, i, &d, dsize);
        if (read != dsize) { 
            return -1;
        }

        long match = (d.d_name[namelen] == '\0'); 
        for (long j = 0; j < namelen && match; j++) { // compare each character in the current dirent's name and name we are given
            if (d.d_name[j] != name[j]) {
                match = 0; 
            }
        }

        if (match) { 
            ent->inode.svi_number = d.d_ino;
            return 0;
        }
    }
    return -1;
}

/*
 * Reads a file from the mobj and passes the data to the buf
 */
long svfs_read(vnode_t *vn, long pos, void *buf, long len) {
    if (pos < 0 || len < 0) { 
        return -1;
    }
    if (pos >= vn->vn_len) { // pos ahead of len return 0 -- past the end of the file
        return 0; }
    if (pos + len > vn->vn_len) { // imake len the length of the file
        len = vn->vn_len - pos;
    }

    for (long page = pos / SV_BLOCK_SIZE; page <= (pos + len - 1) / SV_BLOCK_SIZE; page++) { // pf is loaded into mobj
        pframe_t *pf; 
        mobj_get_pframe(&vn->vn_mobj, page, &pf);
        if (pf == NULL) {
            return -1; 
        }
    }

    return mobj_read(&vn->vn_mobj, pos, len, buf); 
}

/*
 * Writes a file to mobj 
 */
long svfs_write(vnode_t *vn, long pos, const void *buf, long len) {
    svfs_node_t *node = (svfs_node_t *) vn; 
    svfs_super_t *super = &svfs.svfs_super; 
    char blk[SV_BLOCK_SIZE]; 


    if (pos < 0 || len < 0 || pos + len > SV_NDIRECT_BLKS * SV_BLOCK_SIZE) {
        return -1;
    }
    if (len == 0) { 
        return 0;
    }

    for (long page = pos / SV_BLOCK_SIZE; page <= (pos + len - 1) / SV_BLOCK_SIZE; page++) { 
        long is_new = 0; // if the page is new -- right now false by default

        if (node->inode.svi_direct_blocks[page] <= 0) { // if the block number is less than 0 which means that there is no blcok assigned
            spinlock_lock(&svfs.svfs_mutex); 

            long block = -1; 
            for (long i = 0; i < SV_NFBLKS_PER_NODE - 1; i++) { 
                if (super->svs_free_blocks[i] > 0) {
                    block = super->svs_free_blocks[i]; // set current block to thsi free blcok
                    super->svs_free_blocks[i] = -1;
                    break; 
                }
            }

            if (block == -1) { 
                block = super->svs_free_blocks[SV_NFBLKS_PER_NODE - 1]; // use last block in free list
                if (block <= 0) { // no more free blocks after last block
                    spinlock_unlock(&svfs.svfs_mutex);
                    return -1; 
                }
                read_disk_block(block, blk);
                for (long i = 0; i < SV_NFBLKS_PER_NODE; i++) {
                    super->svs_free_blocks[i] = ((long *) blk)[i]; // refill free list with next batch of blocks
                }
            }
            super->svs_nfree--; 


            for (long i = 0; i < SV_BLOCK_SIZE; i++) { // reset bffer
                blk[i] = 0;
            }
            *(svfs_super_t *) blk = *super; // write changes into super block 
            write_disk_block(SV_SUPER_BLOCK, blk);

            spinlock_unlock(&svfs.svfs_mutex); 

            node->inode.svi_direct_blocks[page] = block; 
            is_new = 1; // true now
        }

        pframe_t *pf; 
        mobj_get_pframe(&vn->vn_mobj, page, &pf); // get page frame from mobj
        if (pf == NULL) { 
            return -1;
        }
        if (is_new) { 
            for (long i = 0; i < SV_BLOCK_SIZE; i++) { // zero out page frame because it is new and has no data
                pf->bytes[i] = 0;
            }
        }
    }

    long written = mobj_write(&vn->vn_mobj, pos, len, buf); 

    if (pos + written > vn->vn_len) { // if the write is past the end of the file
        vn->vn_len = pos + written; 
        node->inode.svi_un.svi_size = vn->vn_len;
    }

    for (long i = 0; i < SV_BLOCK_SIZE; i++) {
        blk[i] = 0;
    }
    *(svfs_inode_t *) blk = node->inode; // write changes into inode 
    write_disk_block(node->inode.svi_number + 1, blk); 

    return written; 
}

/*
 * Creates a new file
 */
long svfs_mknod(vnode_t *dir, const char *name, long namelen, vnode_t **out) {
    svfs_node_t tmp; 
    char blk[SV_BLOCK_SIZE];
    long dsize = sizeof(dirent_t); 

    if (namelen <= 0 || namelen > NAME_LEN) { // if name is too short or long return -1
        return -1;
    }
    long en = svfs_find_dirent((svfs_node_t *) dir, name, namelen, &tmp);
    if (en == 0) { // if the dirent is found -- not new so return -1
        return -1;
    }


    long ino = svfs_alloc_inode(&svfs); // allocate a new inode
    if (ino < 0) {
        return -1;
    }

    for (long i = 0; i < SV_BLOCK_SIZE; i++) { // reset bffer
        blk[i] = 0;
    }

    // create a new inode in disk
    svfs_inode_t *inode = (svfs_inode_t *) blk;
    inode->svi_un.svi_size = 0; 
    inode->svi_number = ino; 
    inode->svi_count = 1;
    write_disk_block(ino + 1, blk);

    svfs_node_t *node = slab_obj_alloc(*dir->vn_fs->vnode_allocator); 
    node->inode.svi_number = ino;
    svfs_read_vnode(dir->vn_fs, &node->vnode); 

    dirent_t d = {0}; 
    d.d_ino = ino;
    d.d_off = dir->vn_len + dsize; 
    for (long i = 0; i < namelen; i++) { 
        d.d_name[i] = name[i];
    }
    long written = svfs_write(dir, dir->vn_len, &d, dsize); // write dirent to directory
    if (written != dsize) {
        return -1;
    }

    *out = &node->vnode;
    return 0;
}

/*
 * Creates a new directory
 */
long svfs_mkdir(vnode_t *dir, const char *name, long namelen, vnode_t **out) {
    long dsize = sizeof(dirent_t); 
    long result = svfs_mknod(dir, name, namelen, out); // create a new file

    if (result != 0) { 
        return -1;
    }

    // set the mode and ops of the new file to directory
    vnode_t *vn = *out; 
    vn->vn_mode = DIRECTORY;
    vn->vn_ops = &svfs_dir_vops;


    // create . and .. dirents -- have to be the first two dirents in a directory
    dirent_t dot = {0};
    dot.d_ino = ((svfs_node_t *) vn)->inode.svi_number;
    dot.d_off = dsize;
    dot.d_name[0] = '.';    
    dirent_t dotdot = {0};
    dotdot.d_ino = ((svfs_node_t *) dir)->inode.svi_number;
    dotdot.d_off = 2 * dsize;
    dotdot.d_name[0] = '.';
    dotdot.d_name[1] = '.';

    // write . and .. dirents to the new directory
    long written = svfs_write(vn, 0, &dot, dsize);
    long written2 = svfs_write(vn, dsize, &dotdot, dsize);
    if (written != dsize || written2 != dsize) {
        return -1;
    }
    return 0;
}

/*
 * Search for an entry in a directory. 
 */
long svfs_lookup(vnode_t *dir, const char *name, long namelen, vnode_t **out) {
    svfs_node_t found; // temporary node for the entry
    fs_t *fs = dir->vn_fs; 

    long entry = svfs_find_dirent((svfs_node_t *) dir, name, namelen, &found);

    if (entry != 0) { 
        return -1;
    }
    long ino = found.inode.svi_number;


    spinlock_lock(&fs->vnode_list_mutex);
    for (list_link_t *link = fs->vnode_list.head; link != NULL; link = link->next) { // traverse list of vnodes
        svfs_node_t *node = link->parent;
        if (node->inode.svi_number == ino) { 
            *out = &node->vnode; 
            spinlock_unlock(&fs->vnode_list_mutex); 
            return 0; // found the entry so return 0
        }
    }
    spinlock_unlock(&fs->vnode_list_mutex);

    // couldn't find entry so create a new node
    svfs_node_t *node = slab_obj_alloc(*fs->vnode_allocator);
    node->inode.svi_number = ino;
    svfs_read_vnode(fs, &node->vnode);

    *out = &node->vnode; // set the output to the new vnode
    return 0;
}

/*
 * Get a directory entry
 */
long svfs_readdir(vnode_t *vnode, long pos, dirent_t **d) {
    static dirent_t ent; // temporary dirent
    long dsize = sizeof(dirent_t); 

    if (pos < 0 || pos % dsize != 0 || pos + dsize > vnode->vn_len) { // if pos is negative, not a multiple of sizeof(dirent), or past the end return -1 -- invalid position
        return -1;
    }
    long read = svfs_read(vnode, pos, &ent, dsize); 
    if (read != dsize) { 
        return -1;
    }

    *d = &ent; 
    return 0;
}

/*
 * Read the data associated from a file on the disk
 */
long svfs_read_pframe(vnode_t *vnode, long pagenum, pframe_t **pfp) {
    svfs_node_t *node = (svfs_node_t *) vnode; 

    if (pagenum < 0 || pagenum >= SV_NDIRECT_BLKS) { //  invalid page number
        return -1;
    }

    long block = node->inode.svi_direct_blocks[pagenum]; 
    if (block <= 0) {
        return -1; //  no block assigned
    }

    // create a new pframe
    pframe_t *pf = slab_obj_alloc(svfs_pframe_allocator);
    pf->pf_pagenum = pagenum;
    pf->pf_addr = pf->bytes;
    read_disk_block(block, pf->bytes);
    list_link_init(&pf->pf_link, pf);
    list_insert_back(&vnode->vn_mobj.mo_pframes, &pf->pf_link); // do we need a lock here? -- no

    *pfp = pf; // set the output to the pframe
    return 0;
}


/*
 * Write pframe to disk
 */
long svfs_write_pframe(vnode_t *vnode, pframe_t *pf) {
    svfs_node_t *node = (svfs_node_t *) vnode; // get the node from the vnode

    if (pf->pf_pagenum < 0 || pf->pf_pagenum >= SV_NDIRECT_BLKS) { // invalid page number
        return -1;
    }
    long block = node->inode.svi_direct_blocks[pf->pf_pagenum]; 
    if (block <= 0) { // no block assigned
        return -1;
    }


    write_disk_block(block, pf->bytes); 
    return 0;
}
