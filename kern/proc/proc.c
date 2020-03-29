/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <file.h>
#include <kern/errno.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

/* global pid tables */
struct pidtable *pidtable;


/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	proc->p_numthreads = 0;
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	/* PFH synch variables */
	proc->pfh_lock = NULL;
	proc->pfh_lock_refcount = NULL;

	/* variables for a2 adv */
	proc->retval = proc->pid = 0;
	proc->userproc = false;
	proc->parent = NULL;
	proc->p_proclock = NULL;
	proc->p_proccv = NULL;

	/* Zero out file table */
	memset(proc->p_fh, 0, sizeof(struct pfh_data*) * OPEN_MAX);
	
	return proc;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	// if we don't have the refcount, it means that either the process
	// was not designed for usermode, or it failed halfway.
	// therefore, we also assume that the pfh_lock doesn't exist.
	if(proc->pfh_lock_refcount) {
		if(!*proc->pfh_lock_refcount) {
			// process was halfway created: lock creation failed.
			kfree(proc->pfh_lock_refcount);
			
		} else {
			// ensure that we don't race with other destructor,
			// especially regarding the reference counter.
			lock_acquire(proc->pfh_lock);
			
			// close all files (aka decrement their refcount)
			for(int i=0; i<OPEN_MAX; ++i) {
				if(proc->p_fh[i])
					a2_sys_close_stub(proc->p_fh[i]);
			}

			if(!--*proc->pfh_lock_refcount) {
				// When the refcount reaches 0, there must be no one else referencing
				// this lock and refcount. If it does, then we have a SERIOUS bug!
				// therefore, we'll destroy all of 'em to avoid leak!
				kfree(proc->pfh_lock_refcount);
				lock_release(proc->pfh_lock);
				lock_destroy(proc->pfh_lock);
			} else {
				lock_release(proc->pfh_lock);
			}
		}
	}

	// at this point, no one should hold this lock or CV.
	if(proc->p_proclock)
		lock_destroy(proc->p_proclock);
	if(proc->p_proccv)
		cv_destroy(proc->p_proccv);

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}

	/* deallocate the pid */
	pid_deallocate(proc->pid);

	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
	
	// todo initialize the PID table
	pidtable_init();
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* FH synch fields */
	newproc->pfh_lock_refcount = kmalloc(sizeof(uint32_t));
	if(!newproc->pfh_lock_refcount) {
		// failed creating refcount for process
		proc_destroy(newproc);
		return NULL;
	}
	// marker for destroyer that the process was created halfway
	*newproc->pfh_lock_refcount = 0;
	
	newproc->pfh_lock = lock_create("proc_pfh");
	if(!newproc->pfh_lock) {
		// failed creating lock
		proc_destroy(newproc);
		return NULL;
	}
	*newproc->pfh_lock_refcount = 1;
	/* end FH synch fields */

	/* other fields related to a2 adv */
	newproc->userproc = true;
	newproc->p_proclock = lock_create("p_proclock");
	if(!newproc->p_proclock)
		goto error_1;
	newproc->p_proccv = cv_create("p_proccv");
	if(!newproc->p_proccv) 
		goto error_1;

	/* assigned a PID to the newproc, return erro code if nonzero */
	if(pid_allocate(newproc,(uint32_t *)&newproc->pid))
		goto error_1;

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	return newproc;

error_1: /* jump here if error after creating file handle lock or failed in pid allocation */
	lock_destroy(newproc->pfh_lock);
	proc_destroy(newproc);
	return NULL;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	if(proc->p_proclock)
		lock_acquire(proc->p_proclock);
	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);
	if(proc->p_proclock)
		lock_release(proc->p_proclock);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	if(proc->p_proclock)
		lock_acquire(proc->p_proclock);
	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_lock);
	if(!proc->p_numthreads)
		// very likely it is waitpid that is doing the waiting!
		cv_broadcast(proc->p_proccv, proc->p_proclock);
	// the moment we release this lock, proc is not safe to access anymore
	if(proc->p_proclock)
		lock_release(proc->p_proclock);

	spl = splhigh();
	t->t_proc = NULL;
	splx(spl);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}

/* PID table operations */
void pidtable_init(){
	// set up the pid table
	pidtable = kmalloc(sizeof(struct pidtable));
	if (pidtable == NULL){
		panic("pid table allocation failed\n");
	}

	// set up lock
	pidtable->pid_lock = lock_create("pidtable_lock");
	if (pidtable->pid_lock == NULL){
		panic("pid lock allocation failed\n");
	}

	// set up cv
	pidtable->pid_cv =  cv_create("pidtable_cv");
	if (pidtable->pid_cv == NULL){
		panic("pid cv allocation failed\n");
	}

	/* set the kernel thread parameters */
	pidtable->pid_available = PID_MAX - PID_MIN + 1;
	pidtable->pid_next = PID_MIN;

	/* 
	 * create space for more pids within the table 
	 * meanwhile increasing the available pids from 2 to 1023
	 * this is only done by once at the booting stage so there
	 * wont be any race condition
	 */
	for (int i = PID_MIN; i <= PID_MAX; i++){
		pidtable_rmproc(i);
	}

}

/* these two functions may have race condition, make sure to make them atomic! */

/* insert the proc to its slot in the pidtable */
void pidtable_addproc(pid_t pid, struct proc *proc){
	/* make sure the proc exists */
	KASSERT(proc != NULL);
	pidtable->pid_procs[pid] = proc;
	pidtable->pid_status[pid] = PS_RUNNING;
	pidtable->pid_available--;
}

/* remove the proc from the pidtable, because it finished task */
void pidtable_rmproc(pid_t pid){
	KASSERT(pid <= PID_MAX && pid >= PID_MIN);
	// KASSERT(pidtable->pid_procs[pid] != NULL);
	pidtable->pid_procs[pid] = NULL;
	pidtable->pid_status[pid] = PS_READY;
	pidtable->pid_available++;
}

/* wrapping function to make removing pid atomic */
void pid_deallocate(pid_t pid){
	KASSERT(pid <= PID_MAX && pid >= PID_MIN);
	/* critical region */
	lock_acquire(pidtable->pid_lock);
	pidtable_rmproc(pid);
	lock_release(pidtable->pid_lock);
}	


/* there is no guarantee that allocation of pid is always success
 * so we need to set the erro code just like other syscalls format
 * if failed we pass the erro code to the caller. If succeed the 
 * allocated pid will be stored in the retval.
 */
int pid_allocate(struct proc *proc, uint32_t *retval){
	KASSERT(proc != NULL);
	int err = 0;

	lock_acquire(pidtable->pid_lock);

	/* it is our duty to make sure running process doesn't above limitation 
	 * if available slot equals to one which means we have already run out of
	 * resources, one space is for kernel-only thread
	 */
	if(!pidtable->pid_available) {
		err = ENPROC;
		goto error_1;
	}

	/* find the next empty slot (guaranteed to be exists, otherwise, bug!) */
	for(int i = 0; i < pidtable->pid_available; ++i) {
		if(pidtable->pid_status[pidtable->pid_next] == PS_READY)
			break;
		// wraparound increment
		if(++pidtable->pid_next > PID_MAX)
			pidtable->pid_next = PID_MIN;
	}

	pidtable_addproc(pidtable->pid_next, proc);
	*retval = pidtable->pid_next;

	/* successfully allocate a pid */
	lock_release(pidtable->pid_lock);
	return 0;

error_1:// jump here if running processes exceed maximum
	lock_release(pidtable->pid_lock);
	return err;
}

// TODO: exit && clean zombie
// TODO: mark parent field in children to NULL if parent exit first