/*
 * Declarations for advanced part of assignment 2.
 */

#ifndef _A2ADV_H_
#define _A2ADV_H_

#include <limits.h>
#include <cdefs.h>
#include <mips/trapframe.h>
#include <proc.h>

int a2_sys_fork(int32_t* /*pid_t*/ pid, struct trapframe* tf);

int a2_sys_exit(int32_t status);

// userptr_t of userptr_t
int a2_sys_execv(userptr_t progname, userptr_t* args);

// do not call this directly unless you're syscall or runprogram
// note: do not call this function on an already destroyed process!
int a2_waitpid_stub(struct proc* p, int options, pid_t* pid, int* status);

int a2_sys_waitpid(pid_t pid, int* status, int options);

int a2_sys_getpid(int32_t* pid);

#endif /* _A2ADV_H_ */
