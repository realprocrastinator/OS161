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

/*
 * Add your file-related functions here ...
 */

/* buffer for temporary space copyin/out */
void* kiobuff;

#define curas (curproc->p_addrspace)

#define RW_BUFF_SZ 3

/*helper functions*/
inline size_t max(size_t a, size_t b);

inline size_t min(size_t a, size_t b);

int grow_pfh(void);

/*syscall handler functions*/

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
        while(fd >= 0 && curproc->p_fh[fd])
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
    result = vfs_open(kfilename, flags, 0, curproc->p_fh + fd);
    if(result) {
        curproc->p_fh[fd] = 0;
        goto finish;
    }
    
    // update stats
    if((size_t)fd >= (curproc->p_maxfh))
        curproc->p_maxfh = fd + 1;
    *out_fd = fd;

finish:
    // don't forget not to leak memory!
    kfree(kfilename);
    return result;
}

int a2_sys_rw(int filehandle, int write, void *buf, size_t size, int32_t* written) {
    // in-stack kernel buffer area!
    struct iovec iov, uiov;
	struct uio ku, uu;
    char kbuff[RW_BUFF_SZ];
    int result, to_write;

    // at the moment we only support the stdin,stdout,stderr trio!
    if((size_t)filehandle >= curproc->p_fh_cap || !curproc->p_fh[filehandle]) {
        return EBADF;
    }

    // init kernel IOV data structure
    uio_kinit(&iov, &ku, kbuff, RW_BUFF_SZ, 0, write ? UIO_WRITE : UIO_READ);

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
        ku.uio_offset = 0;

        // ask the VFS to do its job!
        if(write) {
            // copy the data from user ptr to kernel ptr, then write to VOP
            result = uiomove(kbuff, to_write, &uu);
            if(result)
                return result;
            result = VOP_WRITE(curproc->p_fh[filehandle], &ku);
        } else {
            // read from VOP first, then copy the data to user ptr
            result = VOP_READ(curproc->p_fh[filehandle], &ku);
            if(result)
                return result;
            result = uiomove(kbuff, ku.uio_offset, &uu);
        }
        if(result) {
            return result;
        }

        // successfully written this segment
        *written += ku.uio_offset;

        // check whether our requested IO ammount is completed successfully
        // if that's not the case, stop
        if(ku.uio_offset < to_write)
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
    size_t new_fh_cap = (curproc->p_fh_cap + P_FH_INC) * sizeof(struct vnode*);
    struct vnode** p_fh_new = kmalloc(new_fh_cap);
    // why??? OK, bail out gracefully!
    if(!p_fh_new)
        return EMFILE;
    // TODO: copy old table
    // TODO: zero out
    // TODO: free old table
    // TODO: update p_fh_cap
    return 0;
}