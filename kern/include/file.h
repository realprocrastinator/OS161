/*
 * Declarations for file handle and file table management.
 */

#ifndef _FILE_H_
#define _FILE_H_

/*
 * Contains some file-related maximum length constants
 */
#include <limits.h>
#include <proc.h>

/*
 * Put your function declarations and data types here ...
 */
int a2_sys_dup2(uint32_t oldfd, uint32_t newfd, int32_t *outfd);
int a2_sys_lseek(uint32_t fd, uint32_t offset_hi, uint32_t offset_lo, userptr_t whence, off_t *retval64);
int a2_sys_open(userptr_t filename, int flags, mode_t mode, int32_t* out_fd, int32_t target_fd);
int a2_sys_close(uint32_t fd);
void a2_sys_close_stub(struct pfh_data* pfh);
int a2_sys_rw(uint32_t fd, uint32_t write, void *buf, size_t size, int32_t* written);


#endif /* _FILE_H_ */
