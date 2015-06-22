#ifndef _VSFS_H_
#define _VSFS_H_

/* File Types */
#define VSFS_UNUSED 0x0 /* File is unused */
#define VSFS_FILE 0x1 /* Normal File */
#define VSFS_DIR  0x2 /* Directory */
#define VSFS_DEV  0x3 /* Device node */
#define VSFS_SYM  0x4 /* Symbolic link to another file */

#define VSFS_DIRECT 9
#define VSFS_MAX_NAME 124
#define MAX_PATH_DEPTH 16

/**
 * A disk representation of an inode.
 */
typedef struct vsfs_inode {
	ushort perm; /* Owner: RWX Group: RWX Other: RWX*/
	ushort uid; /* Owner ID */
	ushort gid; /* Group ID */
	ushort links_count; /* Hard links */
	uint type; /* type of this file, see above.*/
	uint size; /* How many bytes are in the file */
	uint blocks; /* Blocks allocated to file */
	/* Direct pointers to data blocks */
	uint direct[VSFS_DIRECT]; 
	uint indirect; /* A pointer to a sector of pointers */
	/* A pointer to a sector of pointers to sectors of pointers. */
	uint double_indirect;
} vsfs_inode;

typedef struct directent {
	char name[VSFS_MAX_NAME];
	int inode_num;
} directent;

/**
 * File system information. The superblock is followed by the inode bitmap which
 * is followed by the block bitmap. The 0th inode should ALWAYS be free. The
 * root (topmost directory) is always inode 1. The root cannot be unlinked.
 */
typedef struct vsfs_superblock {
	uint dmap; /* Amount of blocks for the d block bitmap. */
	uint dblocks; /* Total amount of data blocks in the file system */
	uint imap; /* Amount of blocks for inode bitmap. */
	uint inodes; /* How many inodes are there? */
} vsfs_superblock;

struct vsfs_context
{
	struct vsfs_superblock super;
	uint start;
	uint i_off;
	uint imap_off;
	uint b_off;
	uint bmap_off;

	slock_t cache_lock;
	uint cache_count; /* The amount of inodes fit in the cache */
	struct vsfs_cache_inode* cache; /* Pointer to the cache */

	/* File system hardware drivers */
	struct FSHardwareDriver* hdd;
	/* function forwards */
        int (*read)(void* dst, uint sect,
                struct FSHardwareDriver* driver);
        int (*write)(void* src, uint sect,
                struct FSHardwareDriver* driver);
};

/**
 * Initilize a file system driver with the proper function pointers.
 */
int vsfs_driver_init(struct FSDriver* driver);

/**
 * Setup the file system driver with the file system starting at the given
 * sector. The first sector of the disk contains the super block (see above).
 */
int vsfs_init(uint start_sector, uint end_sector, uint block_size, 
        uint cache_sz, uchar* cache, struct vsfs_context* context);

/**
 * Find an inode in a file system. If the inode is found, load it into the dst
 * buffer and return the inode number. If not found, return 0.
 */
int vsfs_lookup(const char* path, vsfs_inode* dst,
	struct vsfs_context* context);

/**
 * Remove the file from the directory in which it lives and decrement the link
 * count of the file. If the file now has 0 links to it, free the file and
 * all of the blocks it is holding onto.
 */
int vsfs_unlink(const char* path, struct vsfs_context* context);

/**
 * Add the inode new_inode to the file system at path. Make sure to add the
 * directory entry in the parent directory. If there are no more inodes
 * available in the file system, or there is any other error return -1.
 * Return the new inode number on success.
 */
int vsfs_link(const char* path, vsfs_inode* new_inode,
	struct vsfs_context* context);

/**
 * Create the directory entry new_file that is a hard link to file. Return 0
 * on success, return 1 otherwise.
 */
int vsfs_hard_link(const char* new_file, const char* link,
	struct vsfs_context* context);

/**
 * Create a soft link called new_file that points to link.
 */
int vsfs_soft_link(const char* new_file, const char* link,
	struct vsfs_context* context);

/**
 * Read sz bytes from inode node at the position start (start is the seek in
 * the file). Copy the bytes into dst. Return the amount of bytes read. If
 * the user has requested a read that is outside of the bounds of the file,
 * don't read any bytes and return 0.
 */
int vsfs_read(vsfs_inode* node, void* dst, uint start, uint sz,
	struct vsfs_context* context);

/**
 * Write sz bytes to the inode node starting at position start. No more than
 * sz bytes can be copied from src. If the file is not big enough to hold
 * all of the information, allocate more blocks to the file.
 * WARNING: A user is allowed to seek to a position in a file that is not
 * allocated and write to that position. There can never be 'holes' in files
 * where there are some blocks allocated in the beginning and some at the end.
 * WARNING: Blocks allocated to files should be zerod if they aren't going to
 * be written to fully.
 */
int vsfs_write(vsfs_inode* node, void* src, uint start, uint sz,
	struct vsfs_context* context);

/**
 * Clear the given inode structure.
 */
void vsfs_clear(vsfs_inode* node);

#endif 
