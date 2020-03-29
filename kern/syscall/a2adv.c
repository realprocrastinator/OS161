#include <types.h>
#include <a2adv.h>
#include <addrspace.h>
#include <thread.h>
#include <proc.h>
#include <synch.h>
#include <current.h>
#include <kern/errno.h>
#include <kern/wait.h>
#include <kern/fcntl.h>
#include <vfs.h>
#include <vnode.h>
#include <copyinout.h>
#include <syscall.h>

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
    *pid = child->pid ; /* hardcoded PID. Please implement this */
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
                cv_wait(p->p_proccv,p->p_proclock);
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

int a2_sys_waitpid(pid_t* pid, int *status, int options){
    if (*pid < 0 || *pid > PID_MAX)
        // return what err?
        return ENOMEM;
    if (!lock_do_i_hold(pidtable->pid_lock))
        lock_acquire(pidtable->pid_lock);
    /* we check if the child has already exited, if yes
     * we just return without calling waitpid stub
     * otherwise we call it
     */
    if (pidtable->pid_procs[*pid] == READY){
        // todo may be should check if is a zombie?
        return 0;
    }
    lock_release(pidtable->pid_lock);
    return a2_waitpid_stub(pidtable->pid_procs[*pid],options,pid,status);
}

int a2_sys_exit(int32_t status) {
    // set the return value to the signal number
	curproc->retval = status;
    // if the exiting thread is the parent which is the
    // lock holder, then dont double acquire the lock!
    if(!lock_do_i_hold(pidtable->pid_lock))
        lock_acquire(pidtable->pid_lock);
    // deallocate the pid, need to be modified
    pidtable_rmproc(curproc->pid);
    lock_release(pidtable->pid_lock);

    // process destruction will be taken care off whoever is waiting for this process,
	// after we exit this thread
    thread_exit();
}

int a2_sys_execv(userptr_t progname, userptr_t* args) {
    int result;
    size_t copylen;
    vaddr_t newentrypoint, newstackptr;

    char* kprogname = kmalloc(PATH_MAX);
    if(!kprogname)
        return ENOMEM;

    // we don't trust any pointer given by user!
    result = copyinstr(progname, kprogname, PATH_MAX, &copylen);
    if(result)
        return result;

    // open the target program file
    struct vnode *v;
    result = vfs_open(kprogname, O_RDONLY, 0, &v);
    if(result)
        goto error_1;

    // temporarily create kernel memory to store the arguments
    char* kargdata = kmalloc(ARG_MAX);
    if(!kargdata) {
        result = ENOMEM;
        goto error_2;
    }
    // count the max size and argcount
    int argcount = 0;
    int argsizetotal = 0; /*will include all terminating NULLs*/
    userptr_t argstri = (userptr_t)0x1;
    for(; argcount < ARG_MAX/4; ++argcount) {
        result = copyin((userptr_t)(args+argcount), &argstri, sizeof(userptr_t));
        if(result)
            goto error_3;
        if(!argstri)
            break;
        result = copyinstr(argstri, kargdata + argsizetotal, ARG_MAX - argsizetotal, &copylen);
        if(result)
            goto error_3;
        // copylen includes terminating NULL already
        argsizetotal += copylen;
    }

    // rescan the offsets
    size_t* argoffsets = kmalloc(argcount * sizeof(size_t));
    if(!argoffsets) {
        result = ENOMEM;
        goto error_3;
    }
    argoffsets[0] = 0; /* first string always points to the beginning! */
    for(int i=0, j=1; j<argcount; ++i) {
        if(!kargdata[i])
            argoffsets[j++] = i+1;
    }
    
    // create new address space
    struct addrspace* newas = as_create();
    if(!newas) {
        result = ENOMEM;
        goto error_4;
    }

    // switch and activate the new address space
    // don't forget to save the old one in case we fail at any point below
    struct addrspace* oldas = proc_setas(newas);
    as_activate();

    // load the new executable. this will also populate the AS.
    result = load_elf(v, &newentrypoint);
    if(result)
        goto error_5;
    // we're done with the VFS that handles to the ELF
    vfs_close(v);
    v = NULL; /* to make the code simpler! */

    // user's stack pointer
    result = as_define_stack(newas, &newstackptr);
    if(result)
        goto error_5;

    // copy the arguments to user's stack
    // first, allocate the stack for the string data
    size_t userargsbase = (newstackptr -= argsizetotal);
    // copy the string
    copyout(kargdata, (userptr_t)newstackptr, argsizetotal);
    // align to word size
    newstackptr -= newstackptr % sizeof(void*);
    // second, allocate stack for the string offsets (again, don't forget the last NULL)
    newstackptr -= (argcount+1) * sizeof(void*);
    // copy the offsets
    size_t* userargsoffsets = (size_t*)newstackptr;
    for(int i=0; i<argcount; ++i)
        userargsoffsets[i] = userargsbase + argoffsets[i];
    // don't forget the terminating NULL as some apps doesn't check from argc
    userargsoffsets[argcount] = 0;
    
    // done copying data to user stack. now begin platform specific ops.
    // align stack to 8 bytes.
    newstackptr -= newstackptr % 8;
    // allocate for first 4 args
    newstackptr -= 16;
    
    // cleanup (do steps in error_4, error_3, and error_1)
    kfree(argoffsets);
    kfree(kargdata);
    kfree(kprogname);

    // go back to the mortal's realm!
    enter_new_process(argcount, (userptr_t)userargsoffsets, NULL, 
        newstackptr, newentrypoint);

    panic("A supposedly successful execv returned!");
    result = EINVAL;

error_5: /*jump here if error after creating and switching addr space*/
    proc_setas(oldas);
    as_activate();
    as_destroy(newas);
error_4: /*jump here if error after allocating argument offset holder*/
    kfree(argoffsets);
error_3: /*jump here if error after allocating argument holder*/
    kfree(kargdata);
error_2: /*jump here if error after opening target executable*/
    if(v)
        vfs_close(v);
error_1: /* jump here if error after allocating kprogname */
    kfree(kprogname);
    return result;
}

// sys_getpid never fails!
int a2_sys_getpid(int32_t* pid){
    lock_acquire(pidtable->pid_lock);
    *pid = curproc->pid;
    lock_release(pidtable->pid_lock);
    return 0;
}