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

#define RW_BUFF_SZ 64

inline size_t max(size_t a, size_t b);

inline size_t min(size_t a, size_t b);

size_t max(size_t a, size_t b) {
    return a > b ? a : b;
}

size_t min(size_t a, size_t b) {
    return a < b ? a : b;
}

int sys_rw(int filehandle, int write, void *buf, size_t size, int32_t* written) {
    // in-stack kernel buffer area!
    struct iovec iov, uiov;
	struct uio ku, uu;
    char kbuff[RW_BUFF_SZ];
    int result, to_write;

    // at the moment we only support the stdin,stdout,stderr trio!
    if(filehandle > 2) {
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

        // move from user space to kernel buffer
        result = uiomove(kbuff, to_write, &uu);
        if(result) {
            return result;
        }

        // ask the VFS to do its job!
        if(write)
            result = VOP_WRITE(curproc->p_fh[filehandle], &ku);
        else
            result = VOP_READ(curproc->p_fh[filehandle], &ku);
        if(result) {
            return result;
        }

        // successfully written this segment
        *written += to_write;
    }
    
    return result;
}
