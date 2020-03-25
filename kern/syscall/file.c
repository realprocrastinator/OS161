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

#define RW_BUFF_SZ 8
// table size (maximum capacity)
#define TCAP (curproc->p_fh_cap)

/*helper functions*/
inline size_t max(size_t a, size_t b);

inline size_t min(size_t a, size_t b);

void* a2_realloc(void* oldptr, size_t oldsize, size_t newsize);

int grow_pfh(uint32_t target);

/*syscall handler functions*/

int a2_sys_dup2(uint32_t oldfd, uint32_t newfd, int32_t *outfd) {
    // if given fd is not valid we should return EBADF (same code as rw)
    if(oldfd >= curproc->p_maxfh_ext || curproc->p_fh_ext[oldfd] < 0) 
        return EBADF;

    // if the given fd is beyond the absolute limit, we should return EMFILE
    if (newfd >= OPEN_MAX)
        return EMFILE;

    if (newfd == oldfd) {
        /* if the fds are same do nothing but return the newfd */
        *outfd = newfd;
        return 0;
    }

    if((uint32_t)newfd >= TCAP)
        /* seems our p_fh table is not large enough! Make it bigger */
        grow_pfh(newfd);

    // just close the newfd!
    a2_sys_close(newfd);
    
    // attach the newfd to oldfd
    curproc->p_fh_ext[newfd] = curproc->p_fh_ext[oldfd];
    
    /* we need to attach the vnode of the oldfd to our newfd */
    curproc->p_fh_ext[newfd] = curproc->p_fh_ext[oldfd];
    /* Dont forget to increase the reference counter! */
    ++curproc->p_fh_int[curproc->p_fh_ext[oldfd]].refcount;
    /* Also don't forget to update stats! */
    if(newfd >= curproc->p_maxfh_ext)
        curproc->p_maxfh_ext = newfd + 1;
    
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
    if(fd >= curproc->p_maxfh_ext || curproc->p_fh_ext[fd] < 0)
        return EBADF;
    
    struct pfh_data* intfh = curproc->p_fh_int + curproc->p_fh_ext[fd];

    /* join the old offset. even though the arguments requires unsigned integer, it should not matter. */
    join32to64(offset_hi, offset_lo, (uint64_t*)&offset);

    /* if the vnode is not seekable */
    if(!VOP_ISSEEKABLE(intfh->vnode))
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
			new_offset = intfh->curr_offset + offset;
			break;

		case SEEK_END:
			//if it is SEEK_END, we use VOP_STAT to figure out
			//the size of the file, and set the offset to be that size.
			result = VOP_STAT( intfh->vnode, &file_st );
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
    intfh->curr_offset = new_offset;

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

    // hard limit
    if(curproc->p_maxfh_ext >= OPEN_MAX)
        return EMFILE;
    
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
            goto finish;
        // look for empty FD available
        for(fd = 0; fd < curproc->p_maxfh_ext && curproc->p_fh_ext[fd] >= 0; ++fd) {}
    } else {
        // if the target_fd is set, then it must be coming from another kernel function
        // we will trust that!
        strcpy(kfilename, (char*)filename);
        // ensure that the target_fd is available for use
        if(curproc->p_fh_ext[target_fd] >= 0) {
            result = EBADF;
            goto finish;
        }
        fd = target_fd;
    }

    // reallocate if needed!
    if(fd >= curproc->p_fh_cap) {
        result = grow_pfh(fd);
        if(result)
            goto finish;
    }

    // find empty slot for internal fd
    size_t int_fd;
    for(int_fd = 0; int_fd < curproc->p_maxfh_int && curproc->p_fh_int[int_fd].vnode; ++int_fd) {}
    
    // let VFS does it's job!
    result = vfs_open(kfilename, flags, (flags & O_CREAT ? mode : 0777), &curproc->p_fh_int[int_fd].vnode);
    if(result) {
        curproc->p_fh_int[int_fd].vnode = 0;
        goto finish;
    }
    // start from the beginning!
    curproc->p_fh_int[int_fd].curr_offset = 0;
    // don't forget the refcount
    ++curproc->p_fh_int[int_fd].refcount;
    // don't forget to set the ACCMODE flag
    curproc->p_fh_int[int_fd].flags = accmode;
    
    // update stats
    if(fd >= (curproc->p_maxfh_ext))
        curproc->p_maxfh_ext = fd + 1;
    if(int_fd >= (curproc->p_maxfh_int))
        curproc->p_maxfh_int = int_fd + 1;
    curproc->p_fh_ext[fd] = int_fd;

    *out_fd = fd;

finish:
    // don't forget not to leak memory!
    kfree(kfilename);
    return result;
}

int a2_sys_close(uint32_t fd) {
    // the same code as from a2_sys_rw below!
    if(fd >= curproc->p_maxfh_ext || curproc->p_fh_ext[fd] < 0) 
        return EBADF;

    uint32_t int_fd = curproc->p_fh_ext[fd];
    
    // reduce refcount, close only if it reaches zero
    if(!(--curproc->p_fh_int[int_fd].refcount)) {
        // VFS' job again! This time, no error indicator, so we will assume this
        // operation is always success! besides, it is specified in POSIX anyway.
        vfs_close(curproc->p_fh_int[int_fd].vnode);
    
        // reset our data struct
        memset(curproc->p_fh_int + int_fd, 0, sizeof(struct pfh_data));
    }

    // this fh is now invalid for application's perspective
    curproc->p_fh_ext[fd] = -1;

    return 0;
}

int a2_sys_rw(uint32_t fd, uint32_t write, void *buf, size_t size, int32_t* written) {
    // in-stack kernel buffer area!
    struct iovec iov, uiov;
	struct uio ku, uu;
    char kbuff[RW_BUFF_SZ];
    int result;
    size_t currwritten, to_write;
    
    // hard limit! (because output is signed 32 bit integer)
    if(size >= 0x80000000)
        return ERANGE;

    if(fd >= curproc->p_maxfh_ext || curproc->p_fh_ext[fd] < 0) 
        return EBADF;

    struct pfh_data* intfh = curproc->p_fh_int + curproc->p_fh_ext[fd];

    // check ACCMODE
    switch(intfh->flags & O_ACCMODE) {
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
    uio_kinit(&iov, &ku, kbuff, RW_BUFF_SZ, intfh->curr_offset, write ? UIO_WRITE : UIO_READ);

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
            result = VOP_WRITE(intfh->vnode, &ku);
            if (result){
                return result;
            }
            currwritten = to_write - ku.uio_resid;
        } else {
            // read from VOP first, then copy the data to user ptr
            result = VOP_READ(intfh->vnode, &ku);
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
        intfh->curr_offset += currwritten;

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

int grow_pfh(uint32_t target) {
    size_t new_fh_cap = TCAP;
    int result = 0;
    
    // do nothing as we can still fit the target
    if(target < TCAP)
        return 0;
    
    new_fh_cap += (((target-TCAP) / P_FH_INC) + 1) * P_FH_INC;

    // realloc internal
    struct pfh_data* p_fh_int_new = a2_realloc(curproc->p_fh_int, 
        TCAP * sizeof(struct pfh_data), new_fh_cap * sizeof(struct pfh_data));
    // why??? OK, bail out gracefully!
    if(!p_fh_int_new)
        return EMFILE;
    // zero out remaining
    memset(p_fh_int_new + TCAP, 0, (new_fh_cap - TCAP) * sizeof(struct pfh_data));

    // realloc external
    int32_t* p_fh_ext_new = a2_realloc(curproc->p_fh_ext,
        TCAP * sizeof(int32_t), new_fh_cap * sizeof(int32_t));
    // why??? OK, bail out gracefully!
    if(!p_fh_ext_new) {
        result = EMFILE;
        goto bail_after_int;
    }
    // minus one out remaining
    memset(p_fh_ext_new + TCAP, -1, (new_fh_cap - TCAP) * sizeof(int32_t));

    // update process data struct
    curproc->p_fh_int = p_fh_int_new;
    curproc->p_fh_ext = p_fh_ext_new;
    curproc->p_fh_cap = new_fh_cap;

    return result;

bail_after_int:
    kfree(p_fh_int_new);
    return result;
}

void* a2_realloc(void* oldptr, size_t oldsize, size_t newsize) {
    if(newsize <= oldsize)
        // do nothing!
        return oldptr;
    
    void* newbuff = kmalloc(newsize);
    memcpy(newbuff, oldptr, oldsize);
    kfree(oldptr);

    return newbuff;
}