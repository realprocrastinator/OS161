/*
 * Declarations for file handle and file table management.
 */

#ifndef _FILE_H_
#define _FILE_H_

/*
 * Contains some file-related maximum length constants
 */
#include <limits.h>

/*
 * the offset is 64 bits, when trap into the kernel the offst is stored in $a2,$a3
 */


/*
 * Put your function declarations and data types here ...
 */
int a2_sys_dup2(int oldfd,int newfd,int *outfd);
int a2_sys_lseek(int fd, uint32_t offset_hi, uint32_t offset_lo, userptr_t whence, off_t *retval64);
int a2_sys_open(userptr_t filename, int flags, int* out_fd);
int a2_sys_close(int fd);
int a2_sys_rw(int filehandle, int write, void *buf, size_t size, int32_t* written);


#endif /* _FILE_H_ */
