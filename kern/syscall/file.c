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
    struct pfh_data* pfh;

    // if the given fd is beyond the absolute limit, we should return EBADF
    if (newfd >= OPEN_MAX)
        return EBADF;
    
    // lock to prevent accessing FD that is about to be closed
    // also to prevent someone else messing with the target FD
    lock_acquire(curproc->pfh_lock);
    if((oldfd >= OPEN_MAX) || !(pfh = curproc->p_fh[oldfd])) {
        // if given fd is not valid we should return EBADF (same code as rw)
        lock_release(curproc->pfh_lock);
        return EBADF;
    }

    // prevent doing any operation with this file
    // (we're going to increase the refcount)
    lock_acquire(pfh->lock);

    if (newfd == oldfd) {
        /* if the fds are same do nothing but return the newfd */
        *outfd = newfd;
        goto finish;
    }

    // just close the newfd!
    // deadlock? should not happen. this will access process-wide lock and internal lock
    // but different from this one (newfd's, while we're owning oldfd's)
    a2_sys_close_stub(curproc->p_fh[newfd]);
    
    // attach the newfd to oldfd
    curproc->p_fh[newfd] = pfh;
    
    /* Dont forget to increase the reference counter! */
    ++pfh->refcount;
    
    /* at this moment dup2 is successful */
    *outfd = newfd;

finish:
    lock_release(curproc->pfh_lock);
    lock_release(pfh->lock);
    return 0;
}

int a2_sys_lseek(uint32_t fd, uint32_t offset_hi, uint32_t offset_lo, userptr_t whence, off_t *retval64) {
    int result = 0;
    /* off_t are 64 bit signed integers */
    off_t offset; /* old offset */
    off_t new_offset;
    struct stat file_st;
    int32_t whence_val;

    /* join the old offset. even though the arguments requires unsigned integer, it should not matter. */
    join32to64(offset_hi, offset_lo, (uint64_t*)&offset);

    // first thing first, check if fd is valid (same code as rw!)
    struct pfh_data* pfh;
    // lock to prevent accessing FD that is about to be closed
    lock_acquire(curproc->pfh_lock);
    if((fd >= OPEN_MAX) || !(pfh = curproc->p_fh[fd])) {
        lock_release(curproc->pfh_lock);
        return EBADF;
    }

    // see a2_sys_rw for the reason why we're doing this pattern
    lock_acquire(pfh->lock);
    lock_release(curproc->pfh_lock);

    /* if the vnode is not seekable */
    if(!VOP_ISSEEKABLE(pfh->vnode)) {
        result = ESPIPE;
        goto finish;
    }

    /* get the whence from user's stack */
    result = copyin(whence, &whence_val, sizeof(int32_t));
    if(result)
        // trying to fool me? not this time!
        goto finish;
    
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
                goto finish;
            
			//set the offet to the filesize.
			new_offset = file_st.st_size + offset;
			break;
		default:
            result = EINVAL;
            goto finish;
	}

    /* make sure the final offset is not negative */
    if(new_offset < 0) {
        result = EINVAL;
        goto finish;
    }
    
    /* update the phf_data structure */
    /* from this point on, should be success, so we return 0 :) */
    pfh->curr_offset = new_offset;
    *retval64 = new_offset;

finish:
    lock_release(pfh->lock);
    return result;
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

    // this lock is used to prevent multiple user thread (if exists!) from opening the same FD
    lock_acquire(curproc->pfh_lock);
    
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
    lock_release(curproc->pfh_lock);
    kfree(kfilename);
    return result;

error_3: /* jump here if error after kmalloc-ing pfh and creating lock */
    lock_destroy(pfh->lock);
error_2: /* jump here if error after kmalloc-ing pfh only */
    kfree(pfh);
error_1: /* jump here if error after kmalloc-ing filename AND after acquiring pfh_lock */
    lock_release(curproc->pfh_lock);
    kfree(kfilename);
    return result;
}

int a2_sys_close(uint32_t fd) {
    struct pfh_data* pfh;

    // prevent multiple user thread from closing this FD
    lock_acquire(curproc->pfh_lock);

    if((fd >= OPEN_MAX) || !(pfh = curproc->p_fh[fd])) {
        lock_release(curproc->pfh_lock);
        return EBADF;
    }

    // this fh is now invalid for application's perspective
    curproc->p_fh[fd] = 0;

    // OK, now other thread is free to access the table!
    lock_release(curproc->pfh_lock);

    a2_sys_close_stub(pfh);

    return 0;
}

int a2_sys_rw(uint32_t fd, uint32_t write, void *buf, size_t size, int32_t* written) {
    // in-stack kernel buffer area!
    struct iovec iov, uiov;
	struct uio ku, uu;
    char kbuff[RW_BUFF_SZ];
    int result = 0;
    size_t currwritten, to_write;

    // hard limit! (because output is signed 32 bit integer)
    if(size >= 0x80000000)
        return ERANGE;
    
    struct pfh_data* pfh;
    // lock to prevent accessing FD that is about to be closed
    lock_acquire(curproc->pfh_lock);
    if((fd >= OPEN_MAX) || !(pfh = curproc->p_fh[fd])) {
        lock_release(curproc->pfh_lock);
        return EBADF;
    }
    // acq the internal file lock first, to prevent race w/ close syscall
    // at this point, close is still waiting for pfh_lock. If it already
    // proceeded to closing the file, we'll get invalid file handle already.
    lock_acquire(pfh->lock);
    // done w/ the process-wide lock. Now if close can proceed, and if it
    // wants to close this file, then it has to wait for the lock that
    // we've acquired above!
    lock_release(curproc->pfh_lock);

    // check ACCMODE
    switch(pfh->flags & O_ACCMODE) {
        case O_RDONLY:
            if(write) {
                result = EBADF;
                goto finish;
            }
            break;
        case O_WRONLY:
            if(!write) {
                result = EBADF;
                goto finish;
            }
            break;
        case O_RDWR:
            break;
        default:
            // joining flags for ACCMODE like this is not valid!
            result = EBADF;
            goto finish;
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
                goto finish;
            result = VOP_WRITE(pfh->vnode, &ku);
            if (result)
                goto finish;
            currwritten = to_write - ku.uio_resid;
        } else {
            // read from VOP first, then copy the data to user ptr
            result = VOP_READ(pfh->vnode, &ku);
            if(result)
                goto finish;
            currwritten = to_write - ku.uio_resid;
            result = uiomove(kbuff, currwritten, &uu);
        }
        if(result)
            goto finish;

        // successfully written this segment
        // how many write minus how many remaining
        *written += currwritten;
        pfh->curr_offset += currwritten;

        // stop if written partially (could be EOF, buffer full, etc.)
        if(currwritten < to_write)
            break;
    }
    
finish:
    lock_release(pfh->lock);
    return result;
}

void a2_sys_close_stub(struct pfh_data* pfh) {
    if(!pfh)
        return;
    // we'll wait until other operation on this handle releases the lock
    lock_acquire(pfh->lock);
    // reduce refcount, close only if it reaches zero
    if(!(--pfh->refcount)) {
        // VFS' job again! This time, no error indicator, so we will assume this
        // operation is always success! besides, it is specified in POSIX anyway.
        vfs_close(pfh->vnode);
    
        // release and destroy lock
        lock_release(pfh->lock);
        lock_destroy(pfh->lock);

        // deallocate
        kfree(pfh);
    } else {
        // don't release the lock twice! (hence we need this "else" part)
        lock_release(pfh->lock);
    }
}

/*helper functions declarations*/
size_t max(size_t a, size_t b) {
    return a > b ? a : b;
}

size_t min(size_t a, size_t b) {
    return a < b ? a : b;
}
