#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/select.h>

#include "kern/types.h"
#include "kern/stdlib.h"
#include "stdlock.h"
#include "file.h"
#include "devman.h"
#include "fsman.h"
#include "tty.h"
#include "pipe.h"
#include "syscall.h"
#include "chronos.h"
#include "ktime.h"
#include "proc.h"
#include "panic.h"

// #define DEBUG_SELECT
// #define DEBUG

extern slock_t ptable_lock;
extern struct proc* rproc;

int sys_link(void)
{
	const char* oldName;
	const char* newName;
	if(syscall_get_str_ptr(&oldName, 0)) return -1;
	if(syscall_get_str_ptr(&newName, 0)) return -1;

#ifdef DEBUG
	if(oldName && newName)
		cprintf("%s: created soft link %s -> %s\n",
				rproc->name, oldName, newName);
#endif

	return fs_link(oldName, newName);
}

int sys_stat(void)
{
	const char* path;
	struct stat* st;
	if(syscall_get_str_ptr(&path, 0)) return -1;
	if(syscall_get_buffer_ptr((void**) &st, sizeof(struct stat), 1)) return -1;
#ifdef DEBUG
	if(path) cprintf("%s: statting file: %s\n", rproc->name, path);
#endif
	inode i = fs_open(path, O_RDONLY, 0, 0, 0);
	if(i == NULL) 
	{
#ifdef DEBUG
		cprintf("%s: permission denied.\n", rproc->name);
#endif
		return -1;
	}
	fs_stat(i, st);
	fs_close(i);
	return 0;

}

/* int open(const char* path, int flags, ...); */
int sys_open(void)
{
	const char* path;
	int flags;
	mode_t mode = 0x0;

	if(syscall_get_str_ptr(&path, 0)) return -1; 
	if(syscall_get_int(&flags, 1)) return -1;
	int fd = find_fd();
	if(fd == -1){
		return -1;
	}

#ifdef DEBUG
	cprintf("%s: opening file %s\n", rproc->name, path);
#endif

#ifdef O_PATH
	if(flags & O_PATH)
	{
		strncpy(rproc->file_descriptors[fd].path, path, FILE_MAX_PATH);
		rproc->file_descriptors[fd].type = FD_TYPE_PATH;
		return -1;
	}
#endif

	int created = 0;
	if(flags & O_CREAT)
	{
		if(syscall_get_int((int*)&mode, 2)) return -1;
		created = fs_create(path, flags, mode, rproc->uid, rproc->gid);
	}

	/* Check for O_EXCL */
	if((flags & O_CREAT) && (flags & O_EXCL) && created)
		return -1;

	rproc->file_descriptors[fd].type = FD_TYPE_FILE;
	strncpy(rproc->file_descriptors[fd].path, path, FILE_MAX_PATH);
	rproc->file_descriptors[fd].flags = flags;
	rproc->file_descriptors[fd].i = fs_open((char*) path, flags,
			mode, rproc->uid, rproc->uid);

	if(rproc->file_descriptors[fd].i == NULL)
	{
		memset(rproc->file_descriptors + fd, 0, 
			sizeof(struct file_descriptor));
		return -1;
	}

	struct stat st;
	fs_stat(rproc->file_descriptors[fd].i, &st);

	if(S_ISDEV(st.st_mode))
	{
		/* Get the driver open */
		rproc->file_descriptors[fd].type = FD_TYPE_DEVICE;
		struct devnode node;
		fs_read(rproc->file_descriptors[fd].i, &node, sizeof(struct devnode), 0);
		rproc->file_descriptors[fd].device = dev_lookup(node.dev);
	}

	if(flags & O_DIRECTORY && !S_ISDIR(st.st_mode))
	{
		memset(rproc->file_descriptors + fd, 0, sizeof(struct file_descriptor));
		return -1;
	}

	if(flags & O_TRUNC && S_ISREG(st.st_mode))
		fs_truncate(rproc->file_descriptors[fd].i, 0);

	rproc->file_descriptors[fd].seek = 0;
#ifdef DEBUG
	cprintf("%s: opened file %s with fd %d\n", rproc->name, path, fd);
#endif
	return fd;
}

/* int close(int fd) */
int sys_close(void)
{
	int fd;
	if(syscall_get_int(&fd, 0)) return -1;

#ifdef DEBUG
	cprintf("%s: closing file %d\n", rproc->name, fd);
#endif

	return close(fd);
}

int close(int fd)
{
	if(fd >= PROC_MAX_FDS || fd < 0){return -1;}
	if(rproc->file_descriptors[fd].type == FD_TYPE_FILE)
	{
#ifdef DEBUG
		cprintf("%s: closing file %s\n",
			rproc->name, rproc->file_descriptors[fd].path);
#endif
		fs_close(rproc->file_descriptors[fd].i);
	}else if(rproc->file_descriptors[fd].type == FD_TYPE_PIPE)
	{
		/* Do we need to free the pipe? */
		if(rproc->file_descriptors[fd].pipe_type == FD_PIPE_MODE_WRITE)
			rproc->file_descriptors[fd].pipe->write_ref--;
		if(rproc->file_descriptors[fd].pipe_type == FD_PIPE_MODE_READ)
			rproc->file_descriptors[fd].pipe->read_ref--;

		if(!rproc->file_descriptors[fd].pipe->write_ref ||
				!rproc->file_descriptors[fd].pipe->read_ref)
			rproc->file_descriptors[fd].pipe->faulted = 1;
	}
	rproc->file_descriptors[fd].type = 0x00;
	return 0;
}

/* int read(int fd, char* dst, uint sz) */
int sys_read(void)
{
	int fd;
	char* dst;
	int sz;
	if(syscall_get_int(&fd, 0)) return -1;
	if(syscall_get_int(&sz, 2)) return -1;
	if(syscall_get_buffer_ptr((void**)&dst, sz, 1)) return -1;

#ifdef DEBUG
	cprintf("%s: doing read for %d bytes.\n",
		rproc->name, sz);
#endif

	switch(rproc->file_descriptors[fd].type)
	{
	case 0x00:
#ifdef DEBUG
		cprintf("%s: invalid fd %d\n", rproc->name, fd);
#endif 
		return -1;
	case FD_TYPE_FILE:
		if((sz = fs_read(rproc->file_descriptors[fd].i, dst, sz,
				rproc->file_descriptors[fd].seek)) < 0) 
		{
#ifdef DEBUG
			cprintf("READ FAILURE!\n");
#endif
			return -1;
		}

#ifdef DEBUG
		cprintf("%s: got %d bytes from read.\n", rproc->name, sz);
#endif
		break;
	case FD_TYPE_DEVICE:
		/* Check for read support */
		if(rproc->file_descriptors[fd].device->io_driver.read)
		{
			/* read is supported */
			sz = rproc->file_descriptors[fd].device->
				io_driver.read(dst,
				rproc->file_descriptors[fd].seek, sz,
				rproc->file_descriptors[fd].device->
				io_driver.context);
		} else return -1;
		break;
	case FD_TYPE_PIPE:
		if(rproc->file_descriptors[fd].pipe_type == FD_PIPE_MODE_READ)
			pipe_read(dst, sz, rproc->file_descriptors[fd].pipe);
		else return -1;
		break;
	default: return -1;
	}

	rproc->file_descriptors[fd].seek += sz;

#ifdef DEBUG
	cprintf("%s: CONTENTS |%s|\n", rproc->name, dst);
	cprintf("%s: new position in file: %d\n", 
		rproc->name, rproc->file_descriptors[fd].seek);
#endif

	return sz;
}

/* int write(int fd, char* src, uint sz) */
int sys_write(void)
{
	int fd;
	char* src;
	int sz;
	if(syscall_get_int(&fd, 0)) return -1;
	if(syscall_get_int(&sz, 2)) return -1;
	if(syscall_get_buffer_ptr((void**)&src, sz, 1)) return -1;

#ifdef DEBUG
        cprintf("%s: doing write for %d bytes.\n",
                rproc->name, sz);
#endif


	switch(rproc->file_descriptors[fd].type)
	{
	default:
#ifdef DEBUG
                cprintf("%s: invalid fd %d\n", rproc->name, fd);
#endif 
		return -1;
	case FD_TYPE_FILE:
		if(fs_write(rproc->file_descriptors[fd].i, src, sz,
				rproc->file_descriptors[fd].seek) == -1)
		{
#ifdef DEBUG
			cprintf("%s: write to file %s failed!\n",
				rproc->name, 
				rproc->file_descriptors[fd].path);
#endif
			return -1;
		}
		break;
	case FD_TYPE_PIPE:
		if(rproc->file_descriptors[fd].pipe_type == FD_PIPE_MODE_WRITE)
			pipe_write(src, sz, rproc->file_descriptors[fd].pipe);
		else return -1;
	case FD_TYPE_DEVICE:
		if(rproc->file_descriptors[fd].device->io_driver.write)
			sz = rproc->file_descriptors[fd].device->
				io_driver.write(src,
				rproc->file_descriptors[fd].seek, sz, 
				rproc->file_descriptors[fd].device->
				io_driver.context);
		else return -1;
		break;
	}

	rproc->file_descriptors[fd].seek += sz;

#ifdef DEBUG
	cprintf("New position in file: %d\n", 
		rproc->file_descriptors[fd].seek);
#endif
	return sz;
}

/* int lseek(int fd, int offset, int whence) */
int sys_lseek(void)
{
	int fd;
	off_t offset;
	int whence;
	if(syscall_get_int(&fd, 0)) return -1;
	if(syscall_get_int((int*)&offset, 1)) return -1;
	if(syscall_get_int(&whence, 2)) return -1;
	if(rproc->file_descriptors[fd].type != FD_TYPE_FILE){
		return -1;
	}
#ifdef DEBUG
	cprintf("%s: Seeking in file\n", rproc->name);
#endif

	int seek_pos;
	if(whence == SEEK_CUR){
		seek_pos = rproc->file_descriptors[fd].seek + offset;
	}
	else if(whence == SEEK_SET){
		seek_pos = offset;
	}
	else if(whence == SEEK_END){
		struct stat stat;
		fs_stat(rproc->file_descriptors[fd].i, &stat);
		seek_pos = stat.st_size + offset;
	} else {
		return -1;
	}

	if(seek_pos < 0){ seek_pos = 0;}
	rproc->file_descriptors[fd].seek = seek_pos;

	return seek_pos;
}


/* int chmod(const char* path, uint perm) */
int sys_chmod(void)
{
	const char* path;
	mode_t mode;

#ifdef DEBUG
	cprintf("%s: chmodding file\n", rproc->name);
#endif

	if(syscall_get_str_ptr(&path, 0)) return -1;
	if(syscall_get_int((int*)&mode, 1)) return -1;
	return fs_chmod(path, mode);
}

/* int chown(const char* path, uint uid, uint gid) */
int sys_chown(void)
{
	const char* path;
	uid_t uid;
	gid_t gid;

	if(syscall_get_str_ptr(&path, 0)) return -1;
	if(syscall_get_int((int*)&uid, 1)) return -1;
	if(syscall_get_int((int*)&gid, 2)) return -1;

	return fs_chown(path, uid, gid);
}

/* int create(const char* file, uint permissions) */
int sys_create(void)
{
	const char* file;
	uint permissions;

	if(syscall_get_str_ptr(&file, 0)) return -1;
	if(syscall_get_int((int*)&permissions, 1)) return -1;
	return fs_create(file, 0, permissions, rproc->uid, rproc->uid);
}

/* int mkdir(const char* dir, uint permissions) */
int sys_mkdir(void)
{
	const char* dir;
	uint permissions;
	if(syscall_get_str_ptr(&dir, 0)) return -1;
	if(syscall_get_int((int*)&permissions, 1)) return -1;
	fs_mkdir(dir, 0, permissions, rproc->uid, rproc->uid);
	return 0;
}

/* int unlink(const char* file) */
int sys_unlink(void)
{
	const char* file;
	if(syscall_get_str_ptr(&file, 0)) return -1;

#ifdef DEBUG
	if(file) 
		cprintf("%s: unlinking file %s\n", rproc->name,
			file);
#endif

	return fs_unlink(file);
}

/* int sys_rmdir(const char* dir) */
int sys_rmdir(void)
{
	const char* dir;
	if(syscall_get_str_ptr(&dir, 0)) return -1;
	return fs_rmdir(dir);
}

/* int mv(const char* orig, const char* dst) */
int sys_mv(void)
{
	/* Implement */
	panic("sys_mv not implemented!\n");
	return 0;
}

/* int fstat(int fd, struct file_stat* dst) */
int sys_fstat(void)
{
	int fd;
	struct stat* dst;

	if(syscall_get_buffer_ptr((void**)&dst, sizeof(struct stat), 1)) 
		return -1;
	if(syscall_get_int(&fd, 0)) return -1;
	if(fd < 0 || fd >= PROC_MAX_FDS) return -1;

#ifdef DEBUG
	cprintf("%s: fstat on file %d\n", rproc->name, fd);
#endif

	int close_on_exit = 0;
	inode i = NULL;
	/* Check file descriptor type */
	switch(rproc->file_descriptors[fd].type)
	{
	case FD_TYPE_FILE:
		i = rproc->file_descriptors[fd].i;
		break;
	case FD_TYPE_DEVICE:
	case FD_TYPE_PIPE:
		i = fs_open(rproc->file_descriptors[fd].device->node, 
			O_RDONLY, 0x0, 0x0, 0x0);
		if(!i) return -1;
		close_on_exit = 1;
		break;
	default:
#ifdef DEBUG
		cprintf("Fstat called on: %d\n", rproc->file_descriptors[fd].type); 
#endif
		return -1;
	}

	int result = fs_stat(i, dst);

	if(close_on_exit)fs_close(i);

	return result;
}

/* int readdir(int fd, struct old_linux_dirent* dirp, uint count) */
int sys_readdir(void)
{
	/**
	 * A note on the return values:
	 *  +  1 = success
	 *  + -1 = failure
	 *  +  0 = failure (end of directory)
	 */

	int fd;
	struct old_linux_dirent* dirp;

	if(syscall_get_int(&fd, 0)) return -1;
	if(syscall_get_buffer_ptr((void**)&dirp, 
			sizeof(struct old_linux_dirent), 1))
		return -1;
	if(fd_ok(fd)) return -1;
	if(rproc->file_descriptors[fd].type != FD_TYPE_FILE)
		return -1;

	struct dirent dir;
	int result = fs_readdir(rproc->file_descriptors[fd].i, 
		rproc->file_descriptors[fd].seek, &dir);
	if(result < 0) return -1;
	if(result == 1) return 0;

	/* Convert to old structure */
	dirp->d_ino = dir.d_ino;
	dirp->d_off = rproc->file_descriptors[fd].seek;
	dirp->d_reclen = FILE_MAX_NAME;
	strncpy(dirp->d_name, dir.d_name, FILE_MAX_NAME);

	rproc->file_descriptors[fd].seek++; /* Increment seek */
	return 1;
}

/* int getdents(int fd, struct chronos_dirent* dirp, uint count) */
int sys_getdents(void)
{
        int fd;
	struct dirent* dirp;
	uint count;

        if(syscall_get_int(&fd, 0)) return -1;
        if(syscall_get_int((int*)&count, 2)) return -1;
	if(count < sizeof(struct dirent)) return -1;
        if(syscall_get_buffer_ptr((void**)&dirp, count, 1))
                return -1;
        if(fd_ok(fd)) return -1;
        if(rproc->file_descriptors[fd].type != FD_TYPE_FILE)
                return -1;

        int result = fs_readdir(rproc->file_descriptors[fd].i,
                rproc->file_descriptors[fd].seek, dirp);
        if(result < 0) return -1;
        if(result == 1) return 0;

        /* Convert to old structure */
        rproc->file_descriptors[fd].seek++; /* Increment seek */
        return sizeof(struct dirent);
}

/* int pipe(int fd[2]) */
int sys_pipe(void)
{
	int* pipefd;
	if(syscall_get_buffer_ptr((void**)&pipefd, sizeof(int) * 2, 0))
		return -1;

	/* Try to get a pipe */
	pipe_t t = pipe_alloc();
	if(!t) return -1;

	/* Get a read fd */
	int read = find_fd();
	if(read >= 0)
	{
		rproc->file_descriptors[read].type = FD_TYPE_PIPE;
		rproc->file_descriptors[read].pipe_type = FD_PIPE_MODE_READ;
		rproc->file_descriptors[read].pipe = t;
	}

	/* Get a write fd */
	int write = find_fd();
	if(write >= 0)
	{
		rproc->file_descriptors[write].type = FD_TYPE_PIPE;
		rproc->file_descriptors[write].pipe_type = FD_PIPE_MODE_WRITE;
		rproc->file_descriptors[write].pipe = t;
	}

	if(read < 0 || write < 0)
	{
		if(read >= 0)
			rproc->file_descriptors[read].type = 0x0;
		if(write >= 0)
			rproc->file_descriptors[write].type = 0x0;
		pipe_free(t);
		return -1;
	}
	t->write_ref = 1;
	t->read_ref = 1;
	pipefd[0] = read;
	pipefd[1] = write;

	return 0;
}

/* int dup(int fd) */
int sys_dup(void)
{
	int fd;
	if(syscall_get_int(&fd, 0)) return -1;
	int new_fd = find_fd();
	return dup2(new_fd, fd);
}

/* int dup2(int new_fd, int old_fd) */
int sys_dup2(void)
{
	int new_fd;
	int old_fd;
	if(syscall_get_int(&new_fd, 0)) return -1;
	if(syscall_get_int(&old_fd, 1)) return -1;
	return dup2(new_fd, old_fd);
}

int dup2(int new_fd, int old_fd)
{
	if(new_fd < 0 || new_fd >= PROC_MAX_FDS) return -1;
	if(old_fd < 0 || old_fd >= PROC_MAX_FDS) return -1;
	/* Make sure new_fd is closed */
	close(new_fd);
	memmove(rproc->file_descriptors + new_fd,
			rproc->file_descriptors + old_fd,
			sizeof(struct file_descriptor));
	switch(rproc->file_descriptors[old_fd].type)
	{
	default: return -1;
	case FD_TYPE_FILE:
		rproc->file_descriptors[old_fd].i->references++;
		break;
	case FD_TYPE_DEVICE:break;
	case FD_TYPE_PIPE:
		slock_acquire(&rproc->
				file_descriptors[old_fd].pipe->guard);
		if(rproc->file_descriptors[old_fd].pipe_type ==
				FD_PIPE_MODE_WRITE)
			rproc->file_descriptors[old_fd].
			pipe->write_ref++;
		if(rproc->file_descriptors[old_fd].pipe_type
				== FD_PIPE_MODE_READ)
			rproc->file_descriptors[old_fd].
			pipe->read_ref++;
		slock_release(&rproc->
				file_descriptors[old_fd].pipe->guard);
		break;
	}

	return 0;
}

int sys_fchdir(int fd)
{
	if(fd < 0) return -1;
	if(fd >= PROC_MAX_FDS) return -1;
	switch(fd)
	{
	case FD_TYPE_FILE:
	case FD_TYPE_DEVICE:
	case FD_TYPE_PATH:
		break;
	default:
		return -1;
	}
	strncpy(rproc->cwd, rproc->file_descriptors[fd].path, FILE_MAX_PATH);
	return 0;
}

int sys_fchmod(void)
{
	int fd;
	mode_t mode;

	if(syscall_get_int(&fd, 0)) return -1;
	if(syscall_get_int((int*)&mode, 1)) return -1;

	if(fd < 0) return -1;
	if(fd >= PROC_MAX_FDS) return -1;
	switch(fd)
	{
	case FD_TYPE_FILE:
	case FD_TYPE_DEVICE:
	case FD_TYPE_PATH:
		break;
	default:
		return -1;
	}
	return fs_chmod(rproc->file_descriptors[fd].path, mode);
}

int sys_fchown(void)
{
	int fd;
	uid_t owner;
	gid_t group;

	if(syscall_get_int(&fd, 0)) return -1;
	if(syscall_get_short((short*)&owner, 1)) return -1;
	if(syscall_get_short((short*)&group, 2)) return -1;

	if(fd < 0) return -1;
	if(fd >= PROC_MAX_FDS) return -1;
	switch(fd)
	{
	case FD_TYPE_FILE:
	case FD_TYPE_DEVICE:
	case FD_TYPE_PATH:
		break;
	default:
		return -1;
	}
	return fs_chown(rproc->file_descriptors[fd].path, owner, group);
}

int lchown(const char *pathname, uid_t owner, gid_t group)
{
	cprintf("Implementation needed for lchown.\n");
	return -1;
}

int sys_ioctl(void)
{
	int fd;
	ulong request;
	void* arg;

	if(syscall_get_int(&fd, 0)) return -1;
	if(syscall_get_long((long*)&request, 1)) return -1;
	if(sizeof(long) == sizeof(int))
	{
		if(syscall_get_int((int*)&arg, 2)) return -1;
	}else if(syscall_get_int((int*)&arg, 3)) return -1;

	if(fd_ok(fd)) return -1;

	/* Is this a device? */
	if(rproc->file_descriptors[fd].type != FD_TYPE_DEVICE)
		return -1;

	/* Does this device support ioctl? */
	if(!rproc->file_descriptors[fd].device->io_driver.ioctl)
		return -1;

	return rproc->file_descriptors[fd].device->io_driver.ioctl(request, 
		arg, rproc->file_descriptors[fd].device->io_driver.context);
}

int sys_access(void)
{
	const char* pathname;
	mode_t mode;

	if(syscall_get_str_ptr(&pathname, 0)) return -1;
	if(syscall_get_int((int*)&mode, 1)) return -1;

	inode i = fs_open(pathname, O_RDONLY, 0x0, 
		rproc->ruid, rproc->rgid);
	
	if(!i) return -1; /* If the file dne, we can't open it! */

	struct stat st;
	fs_stat(i, &st);
	fs_close(i);

	return 0;
}

int sys_ttyname(void)
{
	int fd;
	char* buf;
	size_t sz;

	if(syscall_get_int(&fd, 0)) return -1;
	if(syscall_get_int((int*)&sz, 2)) return -1;
	if(syscall_get_buffer_ptr((void**)&buf, sz, 1)) return -1;

	/* see if the fd is okay */
	if(fd_ok(fd)) return -1;

	/* Check to make sure the file descriptor is actually a device */
	if(rproc->file_descriptors[fd].type != FD_TYPE_DEVICE) return -1;

	/* See if the device is a tty. */
	if(!tty_check(rproc->file_descriptors[fd].device)) return -1;

	/* Move the mount point into buf */
	memmove(buf, rproc->file_descriptors[fd].device->node, sz);
	return 0;
}

int sys_fpathconf(void)
{
	/* TODO: improve the limit checking here */
	int fd;
	int name;

	if(syscall_get_int(&fd, 0)) return -1;
	if(syscall_get_int(&name, 1)) return -1;

	if(fd_ok(fd)) return -1;

	switch(name)
	{
		case _PC_LINK_MAX:
			return LINK_MAX;
		case _PC_NAME_MAX:
		case _PC_PATH_MAX:
			if(!rproc->file_descriptors[fd].type)
				return -1;
			return FILE_MAX_NAME - 1 -
				strlen(rproc->file_descriptors[fd].path);
		case _PC_PIPE_BUF:
			return PIPE_DATA;
		case _PC_CHOWN_RESTRICTED:
			if(!rproc->file_descriptors[fd].type)
                                return -1;
			return FILE_MAX_NAME - 1 -
                                strlen(rproc->file_descriptors[fd].path);
		case _PC_NO_TRUNC:
			return 1;
		case _PC_VDISABLE:
			if(!tty_check(rproc->file_descriptors[fd].device))
				return -1;
			return 1;
		default:
			break;
	}	

	return -1; /* no limit */
}

int sys_pathconf(void)
{
	/* TODO: improve the limit checking here */
        char* path;
        int name;

	if(syscall_get_str_ptr((const char**)&path, 0)) return -1;
	if(syscall_get_int(&name, 1)) return -1;

	switch(name)
	{
		case _PC_LINK_MAX:
			return LINK_MAX;
		case _PC_NAME_MAX:
		case _PC_PATH_MAX:
			//if(!rproc->file_descriptors[fd].type)
			//	return -1;
			return FILE_MAX_NAME - 1 - strlen(path);
		case _PC_PIPE_BUF:
			return PIPE_DATA;
		case _PC_CHOWN_RESTRICTED:
			//if(!rproc->file_descriptors[fd].type)
			//	return -1;
			return FILE_MAX_NAME - 1 - strlen(path);
		case _PC_NO_TRUNC:
			return 1;
		case _PC_VDISABLE:
			return 1;
		default:
			break;
	}

	return -1;
}

int sys_lstat(void)
{
        const char* path;
        struct stat* st;
        if(syscall_get_str_ptr(&path, 0)) return -1;
        if(syscall_get_buffer_ptr((void**) &st, sizeof(struct stat), 1)) return -1;
        inode i = fs_open(path, O_RDONLY, 0, 0, 0);
        if(i == NULL) return -1;
        /* TODO: Handle symbolic links here! */

	fs_stat(i, st);
        fs_close(i);
        return 0;

}

int sys_utime(void)
{
	cprintf("kernel: utime system call not implemented.\n");
	return -1;
}

int sys_utimes(void)
{
	cprintf("kernel: utimes system call not implemented.\n");
	return -1;
}

/* int fcntl(int fd, int action, ...) */
int sys_fcntl(void)
{
	int fd;
	int action;
	if(syscall_get_int(&fd, 0)) return -1;
	if(syscall_get_int(&action, 1)) return -1;

	int i_arg;
	if(syscall_get_int(&i_arg, 2)) return -1;

#ifdef DEBUG
	cprintf("%s: fcntl on fd %d, action %d, iarg: %d\n", 
			rproc->name, fd, action, i_arg);
#endif

	slock_acquire(&ptable_lock);

	int result = 0;
	switch(action)
	{
		case F_DUPFD:
			i_arg = find_fd_gt(i_arg);
			if(i_arg <= 0) return -1;
			result = dup2(i_arg, fd);		
			break;
		case F_DUPFD_CLOEXEC:
                        i_arg = find_fd_gt(i_arg);
                        if(i_arg <= 0) return -1;
                        result = dup2(i_arg, fd);
			/* Also set the close on exec flag */
			rproc->file_descriptors[i_arg].flags |= O_CLOEXEC;
                        break;
		case F_GETFD:
			result = rproc->file_descriptors[fd].flags;
			break;
		case F_SETFD:
			rproc->file_descriptors[fd].flags = i_arg;
			break;
		case F_GETFL:
			result = rproc->file_descriptors[fd].flags;
			break;
		default:
			cprintf("UNIMPLEMENTED FCNTL: %d\n", action);
			result = -1;
			break;;
	}
	
	slock_release(&ptable_lock);

	return result;
}

int sys_sysconf(void)
{
	int name;
	if(syscall_get_int(&name, 0)) return -1;

	long result = -1;
	switch(name)
	{
		default:
#ifdef DEBUG
			cprintf("kernel: no such limit: %d\n", name);
#endif
			break;
	}

	return result;
}

int sys_ftruncate(void)
{
	panic("kernel: ftruncate not implemented yet.\n");
	return -1;
}

int sys_sync(void)
{
	fs_sync();
	return 0;
}

int sys_select_next_fd(int curr_fd, fd_set* set, int max_fd)
{
	for(;curr_fd < PROC_MAX_FDS && curr_fd < max_fd;curr_fd++)
	{
		if(FD_ISSET(curr_fd, set))
			return curr_fd;
	}

	return -1;
}

int sys_select_fdset_move(fd_set* dst, fd_set* src, int max)
{
	FD_ZERO(dst);

	int set_count = 0;

	int fd;
	for(fd = 0;fd < max;fd++)
		if(FD_ISSET(fd, src))
		{
			FD_SET(fd, dst);
			set_count++;
		}

	return set_count;
}

int sys_select(void)
{
#ifdef DEBUG_SELECT
	cprintf("kernel: call to select.\n");
#endif
	int nfds;
	fd_set* readfds = NULL;
	fd_set* writefds = NULL;
	fd_set* exceptfds = NULL;
	struct timeval* timeout = NULL;

	fd_set ret_readfds;
	FD_ZERO(&ret_readfds);
	fd_set ret_writefds;
	FD_ZERO(&ret_writefds);
	fd_set ret_exceptfds;
	FD_ZERO(&ret_exceptfds);

	if(syscall_get_int(&nfds, 0))
		return -1;
	syscall_get_optional_ptr((void**)&readfds, 1);
	syscall_get_optional_ptr((void**)&writefds, 2);
	syscall_get_optional_ptr((void**)&exceptfds, 3);
	syscall_get_optional_ptr((void**)&timeout, 4);

#ifdef DEBUG_SELECT
	cprintf("nfds: %d  readfds? %d  writefds? %d exceptfds? %d timeout? %d\n", nfds, 
			readfds ? 1 : 0, 
			writefds ? 1 : 0, 
			exceptfds ? 1 : 0, 
			timeout ? 1 : 0);
	if(timeout)
		cprintf("\ttimeout: tv_sec: %d  tv_usec %d\n", timeout->tv_sec, timeout->tv_usec);
#endif

	int dev_found = 0;
	unsigned int endtime = 0;
	if(timeout)
		endtime = ktime_seconds() + timeout->tv_sec;
	while(!dev_found)
	{
		int fd;
		/* Check read fds if available */
		if(readfds)
		{
			fd = 0;
			while((fd = sys_select_next_fd(fd, readfds, nfds)) != -1)
			{
				switch(rproc->file_descriptors[fd].type)
				{
					case FD_TYPE_FILE:
						/** 
						 * Files are always 
						 * ready for reading
						 */
						FD_SET(fd, &ret_readfds);
						dev_found = 1;
						break;
					case FD_TYPE_DEVICE: /** Going over the 80 limit here ---- */

						/* Does this device support read 'peek'? */
						if(rproc->file_descriptors[fd].device->io_driver.ready_read)
						{
							/* Does the device have something for us? */
							if(rproc->file_descriptors[fd].device->io_driver.ready_read(
										rproc->file_descriptors[fd].device->io_driver.context))
							{
								FD_SET(fd, &ret_readfds);
								dev_found = 1;
							}
						}

						break;
					case FD_TYPE_PATH:
					case FD_TYPE_NULL:
						break;
				}

				fd++;
			}
		}

		if(writefds)
		{
			fd = 0;
			while((fd = sys_select_next_fd(fd, writefds, nfds)) != -1)
			{
				switch(rproc->file_descriptors[fd].type)
				{
					case FD_TYPE_FILE:
						/** 
						 * Files are always 
						 * ready for writing
						 */
						dev_found = 1;
						FD_SET(fd, &ret_writefds);
						break;
					case FD_TYPE_DEVICE: /** Going over the 80 limit here ---- */

						/* Does this device support write 'peek'? */
						if(rproc->file_descriptors[fd].device->io_driver.ready_write)
						{
							/* Is this output device ready? */
							if(rproc->file_descriptors[fd].device->io_driver.ready_write(
										rproc->file_descriptors[fd].device->io_driver.context))
							{
								FD_SET(fd, &ret_writefds);
								dev_found = 1;
							}
						}

						break;
					case FD_TYPE_PATH:
					case FD_TYPE_NULL:
						break;
				}

				fd++;
			}
		}


		if(!dev_found)
		{
			/* yield for a cycle */
			yield();
	
			/* Is our timeout up? */
			if(timeout && ktime_seconds() >= endtime)
			{
#ifdef DEBUG_SELECT
				cprintf("select: timeout expired.\n");
#endif
				return 0;
			}
		}

	}	

	/* copy the return sets */
	int fd_count = 0;
	if(readfds) fd_count += sys_select_fdset_move(readfds, &ret_readfds, nfds);
	if(writefds) fd_count += sys_select_fdset_move(writefds, &ret_writefds, nfds);
	if(exceptfds) fd_count += sys_select_fdset_move(exceptfds, &ret_exceptfds, nfds);

#ifdef DEBUG_SELECT
	cprintf("select: value returned: %d\n", fd_count);
#endif

	return fd_count;
}
