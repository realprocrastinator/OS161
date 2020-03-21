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
 * Put your function declarations and data types here ...
 */

int a2_sys_open(userptr_t filename, int flags, int* out_fd);
int a2_sys_rw(int filehandle, int write, void *buf, size_t size, int32_t* written);


#endif /* _FILE_H_ */
