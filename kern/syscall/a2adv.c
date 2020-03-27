#include <types.h>
#include <a2adv.h>
#include <addrspace.h>
#include <thread.h>
#include <proc.h>
#include <synch.h>
#include <current.h>
#include <kern/errno.h>
#include <kern/wait.h>

struct fork_child_pass {
    struct lock* lock;
    struct cv* cv;
    bool status;
    struct trapframe* tf;
};

void enter_forked_process(void* passdata, unsigned long unused);

int a2_sys_fork(int32_t* pid, struct trapframe* tf) {
    int result = 0;

    // copy current process address space (this includes stack)
    // the reason that we do this first is that it is straightforward
    // to destroy as should an error happened below (simply call as_destroy)
    struct addrspace* child_as;
    result = as_copy(curproc->p_addrspace, &child_as);
    if(result)
        return result;

    // then, create new process
    struct proc* child = proc_create_runprogram("forked_child");
    if(!child) {
        as_destroy(child_as);
        return ENOMEM; /* must be memory error. */
    }

    // assign address space to child process
    child->p_addrspace = child_as;

    // copy filetables
    child->pfh_lock = curproc->pfh_lock;
    child->pfh_lock_refcount = curproc->pfh_lock_refcount;
    // after copying, we have to increment the refcount for all file handles
    // because if at any point we're failing, we'll call the proc_destroy,
    // which will decrement or close the file.
    memmove(child->p_fh, curproc->p_fh, sizeof(struct pfh_data*)*OPEN_MAX);

    // increase the pfh_lock_refcount,
    // but first, acquire lock, so that we won't race.
    // when this happens, parent is still held on waiting for this syscall to complete,
    // so it should not be possible for it to eventually call proc_destroy.
    lock_acquire(curproc->pfh_lock);
    ++child->pfh_lock_refcount;
    // increase ref count for each file
    for(int i=0; i<OPEN_MAX; ++i) {
        if(child->p_fh[i]) {
            lock_acquire(child->p_fh[i]->lock);
            ++child->p_fh[i]->refcount;
            lock_release(child->p_fh[i]->lock);
        }
    }
    lock_release(curproc->pfh_lock);

    // I will be the parent of this child. I have to protect my child from being
    // waited by anyone else!
    child->parent = curproc;

    // create condition variable to synchronize trap frame copy with the forked child
    struct fork_child_pass passdata;
    passdata.status = false;
    passdata.cv = cv_create("sysforkcv");
    passdata.tf = tf;
    if(!passdata.cv) {
        result = ENOMEM;
        goto error_1;
    }
    passdata.lock = lock_create("sysforklock");
    if(!passdata.lock) {
        cv_destroy(passdata.cv);
        result = ENOMEM;
        goto error_1;
    }

    // fork current thread
    result = thread_fork("child_thread", child, enter_forked_process, &passdata, 0);
    if(result)
        goto error_2;

    // I am now a parent! let's wait for child to finish copying TF
    lock_acquire(passdata.lock);
    while(!passdata.status) {
        cv_wait(passdata.cv, passdata.lock);
    }
    lock_release(passdata.lock);

    // OK, we can safely remove this stack by returning success!
    *pid = 42; /* hardcoded PID. Please implement this */
    return 0;

error_2: /* jump here if error after cv and lock creation */
    cv_destroy(passdata.cv);
    lock_destroy(passdata.lock);
error_1: /* jump here if error before cv and lock creation, but after file refcount */
    // undo all the work!
    as_destroy(child_as);
    proc_destroy(child);
    return result;
}

void enter_forked_process(void* passdata, unsigned long unused) {
    (void)unused;

    struct fork_child_pass* data = passdata;;
    struct trapframe tf;
    memmove(&tf, data->tf, sizeof(struct trapframe));

    // signal parent that we've completed
    lock_acquire(data->lock);
    data->status = true;
    cv_signal(data->cv, data->lock);
    lock_release(data->lock);

    // modify trapframe so that it returns success and PID 0
    tf.tf_a3 = 0;
    tf.tf_v0 = 0;
    tf.tf_epc += 4; // go to next instruction!

    // activate process' address space (this will map the MMU)
    as_activate();

    // okay, finally, let's go back to mortal's realm!
    mips_usermode(&tf);
}

int a2_waitpid_stub(struct proc* p, int options, pid_t* pid, int* status) {
    // we won't check here if we're the child or not. it is the
    // responsibility of syscall's handler

    int result = 0;
    // this (will be) the only supported option!
    bool nohang = options & WNOHANG;
    if(options < 0 || options > 1)
        return EINVAL;
    
    bool destroy_proc = false;
    
    lock_acquire(p->p_proclock);
    // basically we'll check if numthreads is 0.
    if(p->p_numthreads) {
        // if process is running ...
        if(nohang)
            // if target process is running, set pid to 0 then return 0 (success)
            *pid = 0;
        else {
            // else, wait until process stops (no more thread)
            while(p->p_numthreads)
                cv_wait(p->p_proccv, p->p_proclock);
            // destroy this process afterwards
            destroy_proc = true;
        }
    } else 
        // process is already stopped
        destroy_proc = true;

    // give the return value if we're about to destroy this
    if(destroy_proc) {
        *pid = p->pid;
        if(status)
            *status = p->retval;
    }
    lock_release(p->p_proclock);

    // actually destroy this structure
    if(destroy_proc)
        proc_destroy(p);

    return result;
}

int a2_sys_exit(int32_t status) {
    // set the return value to the signal number
	curproc->retval = status;

    // process destruction will be taken care off whoever is waiting for this process,
	// after we exit this thread
    thread_exit();
}