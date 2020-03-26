/*
 * Declarations for advanced part of assignment 2.
 */

#ifndef _A2ADV_H_
#define _A2ADV_H_

#include <limits.h>
#include <cdefs.h>
#include <mips/trapframe.h>

int a2_sys_fork(int32_t* /*pid_t*/ pid, struct trapframe* tf);

#endif /* _A2ADV_H_ */
