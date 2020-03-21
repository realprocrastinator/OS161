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

/*
 * Add your file-related functions here ...
 */

/* buffer for temporary space copyin/out */
void* kiobuff;

#define curas (curproc->p_addrspace)

#define RW_BUFF_SZ 8

/*helper functions*/
inline size_t max(size_t a, size_t b);

inline size_t min(size_t a, size_t b);

int grow_pfh(void);

/*syscall handler functions*/

off_t a2_sys_lseek(int fd, off_t offset, int whence, int* retval){
    int result = 0;
    int new_offset;
    struct stat *file_st = NULL;
    struct vnode * vn = curproc->p_fh[fd].vnode;

    /* if FD is invalid or vnode is NULL */
    if((size_t)fd >= curproc->p_fh_cap || !vn) {
        result = EBADF;
        goto badresult;
    }

    /* if the vnode is not seekable */
    if(!VOP_ISSEEKABLE(vn)){
        /* operation not permitted or ESPIPE?*/
        result = ESPIPE;
        goto badresult;
    }

    /* make sure the offset given by user is valid */
    if(offset < 0){
        result = EINVAL;
        goto badresult;
    }

    /* get the current position of the file data and it's actually the end of the file*/
    int currpos = curproc->p_fh[fd].curr_offset;

    /* set cursor at the begining of the file */
    
    switch( whence ) {
		case SEEK_SET:
            new_offset = offset;
			break;
		
		case SEEK_CUR:
			new_offset = currpos + offset;
			break;

		case SEEK_END:
			//if it is SEEK_END, we use VOP_STAT to figure out
			//the size of the file, and set the offset to be that size.
			result = VOP_STAT( vn, file_st );
            if (result){
                return result;
            }
			//set the offet to the filesize.
			new_offset = file_st->st_size + offset;
			break;
		default:
			result = EINVAL;
	}
    
    /* no match flag! */
    /* update the phf_data structure */
    curproc->p_fh[fd].curr_offset = new_offset;
    *retval = new_offset;
    return result;

badresult:
    *retval = -1;
    return result;
}

int a2_sys_open(userptr_t filename, int flags, int* out_fd) {
    int result = 0;
    size_t filename_len;

    // seriously??
    if(curproc->p_maxfh >= 0x8000000)
        return EMFILE;
    
    // ensure that the buffer passed is correct!
    // we'll store this in heap to avoid overfilling the stack :(
    char* kfilename = kmalloc(PATH_MAX);
    // no idea why does this happen, but end up gracefully!
    if(!kfilename)
        return ENOMEM;
    
    result = copyinstr(filename, kfilename, PATH_MAX, &filename_len);
    if(result)
        goto finish;

    // look for empty FD available (we start from the existing allocated FD first)
    // vfs_open (mode is hardcoded!)
    ssize_t fd = curproc->p_maxfh;
    if((size_t)fd >= curproc->p_fh_cap) {
        // in this case, we'll have to reallocate. OTOH, the newly allocated area 
        // is guaranteed to be empty. 
        result = grow_pfh();
        if(result)
            goto finish;
    } else {
        while(fd >= 0 && curproc->p_fh[fd].vnode)
            fd--;
    }
    
    // OK, no free FD found. Let's use the next space.
    if(fd < 0) {
        fd = curproc->p_maxfh;
        // reallocate if needed!
        if((size_t)fd >= curproc->p_fh_cap) {
            result = grow_pfh();
            if(result)
                goto finish;
        }
    }

    // let VFS does it's job!
    result = vfs_open(kfilename, flags, 0, &curproc->p_fh[fd].vnode);
    if(result) {
        curproc->p_fh[fd].vnode = 0;
        goto finish;
    }
    // start from the beginning!
    curproc->p_fh[fd].curr_offset = 0;
    
    // update stats
    if((size_t)fd >= (curproc->p_maxfh))
        curproc->p_maxfh = fd + 1;
    *out_fd = fd;

finish:
    // don't forget not to leak memory!
    kfree(kfilename);
    return result;
}

int a2_sys_close(int fd) {
    // the same code as from a2_sys_rw below!
    if((size_t)fd >= curproc->p_fh_cap || !curproc->p_fh[fd].vnode) {
        return EBADF;
    }

    // VFS' job again! This time, no error indicator, so we will assume this
    // operation is always success! besides, it is specified in POSIX anyway.
    vfs_close(curproc->p_fh[fd].vnode);

    // reset our data struct
    memset(curproc->p_fh + fd, 0, sizeof(struct pfh_data));

    return 0;
}

int a2_sys_rw(int filehandle, int write, void *buf, size_t size, int32_t* written) {
    // in-stack kernel buffer area!
    struct iovec iov, uiov;
	struct uio ku, uu;
    char kbuff[RW_BUFF_SZ];
    int result;
    size_t currwritten, to_write;

    if((size_t)filehandle >= curproc->p_fh_cap || !curproc->p_fh[filehandle].vnode) {
        return EBADF;
    }

    // init kernel IOV data structure
    uio_kinit(&iov, &ku, kbuff, RW_BUFF_SZ, curproc->p_fh[filehandle].curr_offset, write ? UIO_WRITE : UIO_READ);

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
        // ku.uio_offset = curproc->p_fh[filehandle].curr_offset;

        // ask the VFS to do its job!
        if(write) {
            // copy the data from user ptr to kernel ptr, then write to VOP
            result = uiomove(kbuff, to_write, &uu);
            if(result)
                return result;
            result = VOP_WRITE(curproc->p_fh[filehandle].vnode, &ku);
            if (result){
                return result;
            }
            currwritten = to_write - ku.uio_resid;
        } else {
            // read from VOP first, then copy the data to user ptr
            result = VOP_READ(curproc->p_fh[filehandle].vnode, &ku);
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
        curproc->p_fh[filehandle].curr_offset += currwritten;

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

int grow_pfh(void) {
    // realloc!
    size_t old_fh_cap = (curproc->p_fh_cap) * sizeof(struct pfh_data);
    size_t new_fh_cap = old_fh_cap + P_FH_INC * sizeof(struct pfh_data);
    struct pfh_data* p_fh_new = kmalloc(new_fh_cap);
    // why??? OK, bail out gracefully!
    if(!p_fh_new)
        return EMFILE;
    
    // copy old table
    memcpy(p_fh_new, curproc->p_fh, old_fh_cap);

    // zero out the remaining
    memset(p_fh_new + curproc->p_fh_cap, 0, new_fh_cap - old_fh_cap);

    // free old table
    kfree(curproc->p_fh);

    // update p_fh_cap
    curproc->p_fh = p_fh_new;
    curproc->p_fh_cap += P_FH_INC;

    return 0;
}