#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <lib.h>
#include <uio.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <vnode.h>
#include <file.h>
#include <syscall.h>
#include <copyinout.h>
#include <proc.h>
#include <addrspace.h>
#include <stat.h>
#include <endian.h>
#include <limits.h>
#include <file.h>

/*
 * Add your file-related functions here ...
 */

#define curas (curproc->p_addrspace)

#define RW_BUFF_SZ 32

/*helper functions*/
inline size_t max(size_t a, size_t b);

inline size_t min(size_t a, size_t b);

/*syscall handler functions*/

int a2_sys_dup2(uint32_t oldfd, uint32_t newfd, int32_t *outfd) {
    // if given fd is not valid we should return EBADF (same code as rw)
    struct pfh_data* pfh;
    if((oldfd >= OPEN_MAX) || !(pfh = curproc->p_fh[oldfd]))
        return EBADF;

    // if the given fd is beyond the absolute limit, we should return EBADF
    if (newfd >= OPEN_MAX)
        return EBADF;

    if (newfd == oldfd) {
        /* if the fds are same do nothing but return the newfd */
        *outfd = newfd;
        return 0;
    }

    // just close the newfd!
    a2_sys_close(newfd);
    
    // attach the newfd to oldfd
    curproc->p_fh[newfd] = pfh;
    
    /* Dont forget to increase the reference counter! */
    ++pfh->refcount;
    
    /* at this moment dup2 is successful */
    *outfd = newfd;
    return 0;
}

int a2_sys_lseek(uint32_t fd, uint32_t offset_hi, uint32_t offset_lo, userptr_t whence, off_t *retval64) {
    int result = 0;
    /* off_t are 64 bit signed integers */
    off_t offset; /* old offset */
    off_t new_offset;
    struct stat file_st;
    int32_t whence_val;

    // first thing first, check if fd is valid (same code as rw!)
    struct pfh_data* pfh;
    if((fd >= OPEN_MAX) || !(pfh = curproc->p_fh[fd]))
        return EBADF;

    /* join the old offset. even though the arguments requires unsigned integer, it should not matter. */
    join32to64(offset_hi, offset_lo, (uint64_t*)&offset);

    /* if the vnode is not seekable */
    if(!VOP_ISSEEKABLE(pfh->vnode))
        /* operation not permitted or ESPIPE?*/
        return ESPIPE;

    /* get the whence from user's stack */
    result = copyin(whence, &whence_val, sizeof(int32_t));
    if(result)
        // trying to fool me? not this time!
        return result;
    
    switch( whence_val ) {
		case SEEK_SET:
            new_offset = offset;
			break;
		
		case SEEK_CUR:
			new_offset = pfh->curr_offset + offset;
			break;

		case SEEK_END:
			//if it is SEEK_END, we use VOP_STAT to figure out
			//the size of the file, and set the offset to be that size.
            //if we can't determine the end (via STAT), of course then this operation
            //is invalid.
			result = VOP_STAT( pfh->vnode, &file_st );
            if (result)
                return result;
            
			//set the offet to the filesize.
			new_offset = file_st.st_size + offset;
			break;
		default:
			return EINVAL;
	}

    /* make sure the final offset is not negative */
    if(new_offset < 0)
        return EINVAL;
    
    /* update the phf_data structure */
    /* from this point on, should be success, so we return 0 :) */
    pfh->curr_offset = new_offset;

    *retval64 = new_offset;
    return 0;
}

int a2_sys_open(userptr_t filename, int flags, mode_t mode, int32_t* out_fd, int32_t target_fd) {
    int result = 0;
    size_t filename_len;
    size_t fd;

    // check if access mode is valid
    int accmode = flags & O_ACCMODE;
    switch (accmode)
    {
    case O_RDONLY:
    case O_WRONLY:
    case O_RDWR:
        // OK, continue
        break;
    default:
        // WTH?
        return EINVAL;
    }

    // ensure that the buffer passed is correct!
    // we'll store this in heap to avoid overfilling the stack :(
    char* kfilename = kmalloc(PATH_MAX);
    // no idea why does this happen, but end up gracefully!
    if(!kfilename)
        return ENOMEM;
    
    if(target_fd < 0) {
        // must be from user mode
        result = copyinstr(filename, kfilename, PATH_MAX, &filename_len);
        if(result)
            goto error_1;
        // look for empty FD available
        for(fd = 0; fd < OPEN_MAX && curproc->p_fh[fd]; ++fd) {}
    } else {
        // if the target_fd is set, then it must be coming from another kernel function
        // we will trust that!
        strcpy(kfilename, (char*)filename);
        fd = target_fd;
    }

    // ensure target FD is available for use
    if(curproc->p_fh[fd]) {
        result = fd == (OPEN_MAX - 1) ? EMFILE : EBADF;
        goto error_1;
    }

    // allocate internal data structure at specified fd
    struct pfh_data* pfh = kmalloc(sizeof(struct pfh_data));
    if(!pfh) {
        result = ENOMEM;
        goto error_1;
    }
    memset(pfh, 0, sizeof(struct pfh_data));

    // lock create
    pfh->lock = lock_create("pfh_int");
    if(!pfh->lock) {
        result = ENOMEM;
        goto error_2;
    }

    // let VFS does it's job!
    result = vfs_open(kfilename, flags, (flags & O_CREAT ? mode : 0777), &pfh->vnode);
    if(result)
        goto error_3;
    
    // don't forget the refcount
    ++pfh->refcount;
    // don't forget to set the ACCMODE flag
    pfh->flags = accmode;

    *out_fd = fd;

    // final cleanup, and set fd pointer
    curproc->p_fh[fd] = pfh;
    kfree(kfilename);
    return result;

error_3: /* jump here if error after kmalloc-ing pfh and creating lock */
    lock_destroy(pfh->lock);
error_2: /* jump here if error after kmalloc-ing pfh only */
    kfree(pfh);
error_1: /* jump here if error after kmalloc-ing filename */
    kfree(kfilename);
    return result;
}

int a2_sys_close(uint32_t fd) {
    struct pfh_data* pfh;
    if((fd >= OPEN_MAX) || !(pfh = curproc->p_fh[fd]))
        return EBADF;
    
    // reduce refcount, close only if it reaches zero
    if(!(--pfh->refcount)) {
        // VFS' job again! This time, no error indicator, so we will assume this
        // operation is always success! besides, it is specified in POSIX anyway.
        vfs_close(pfh->vnode);
    
        // destroy lock
        lock_destroy(pfh->lock);

        // deallocate
        kfree(pfh);
    }

    // this fh is now invalid for application's perspective
    curproc->p_fh[fd] = 0;

    return 0;
}

int a2_sys_rw(uint32_t fd, uint32_t write, void *buf, size_t size, int32_t* written) {
    // in-stack kernel buffer area!
    struct iovec iov, uiov;
	struct uio ku, uu;
    char kbuff[RW_BUFF_SZ];
    int result;
    size_t currwritten, to_write;
    
    struct pfh_data* pfh;
    if((fd >= OPEN_MAX) || !(pfh = curproc->p_fh[fd]))
        return EBADF;

    // hard limit! (because output is signed 32 bit integer)
    if(size >= 0x80000000)
        return ERANGE;

    // check ACCMODE
    switch(pfh->flags & O_ACCMODE) {
        case O_RDONLY:
            if(write)
                return EBADF;
            break;
        case O_WRONLY:
            if(!write)
                return EBADF;
            break;
        case O_RDWR:
            break;
        default:
            // joining flags for ACCMODE like this is not valid!
            return EBADF;
    }

    // init kernel IOV data structure
    uio_kinit(&iov, &ku, kbuff, RW_BUFF_SZ, pfh->curr_offset, write ? UIO_WRITE : UIO_READ);

    // init passed-in user buffer IOV
    // the last param, the READ/WRITE, is based on user's perspective
    uio_kinit(&uiov, &uu, buf, size, 0, write ? UIO_WRITE : UIO_READ);
    uu.uio_segflg = UIO_USERSPACE;
    uu.uio_space = curas;

    // reset output param
    *written = 0;
    // reset status to success
    result = 0;

    // loop until we run out of residual in user space
    while(uu.uio_resid) {
        // copy from user ptr to kernel
        to_write = min(uu.uio_resid, (size_t)RW_BUFF_SZ);

        // reset kernel IOV
        iov.iov_kbase = kbuff;
        ku.uio_resid = iov.iov_len = to_write;

        // ask the VFS to do its job!
        if(write) {
            // copy the data from user ptr to kernel ptr, then write to VOP
            result = uiomove(kbuff, to_write, &uu);
            if(result)
                return result;
            result = VOP_WRITE(pfh->vnode, &ku);
            if (result){
                return result;
            }
            currwritten = to_write - ku.uio_resid;
        } else {
            // read from VOP first, then copy the data to user ptr
            result = VOP_READ(pfh->vnode, &ku);
            if(result)
                return result;
            currwritten = to_write - ku.uio_resid;
            result = uiomove(kbuff, currwritten, &uu);
        }
        if(result) {
            return result;
        }

        // successfully written this segment
        // how many write minus how many remaining
        *written += currwritten;
        pfh->curr_offset += currwritten;

        // stop if written partially (could be EOF, buffer full, etc.)
        if(currwritten < to_write)
            break;
    }
    
    return result;
}

/*helper functions declarations*/
size_t max(size_t a, size_t b) {
    return a > b ? a : b;
}

size_t min(size_t a, size_t b) {
    return a < b ? a : b;
}
