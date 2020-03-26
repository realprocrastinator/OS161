#include <types.h>
#include <a2adv.h>
#include <addrspace.h>
#include <thread.h>
#include <proc.h>
#include <synch.h>
#include <current.h>
#include <kern/errno.h>

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
    memmove(child->p_fh, curproc->p_fh, sizeof(struct pfh_data*)*OPEN_MAX);

    // increase the pfh_lock_refcount,
    // but first, acquire lock, so that we won't race.
    // when this happens, parent is still held on waiting for this syscall to complete,
    // so it should not be possible for it to eventually call proc_destroy.
    lock_acquire(curproc->pfh_lock);
    ++child->pfh_lock_refcount;
    // TODO: increase ref count for each file
    // TODO: decrement lock_refcount upon failure!
    lock_release(curproc->pfh_lock);

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
error_1: /* jump here if error before cv and lock creation */
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