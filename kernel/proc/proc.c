#include "kernel.h"
#include "config.h"
#include "globals.h"
#include "errno.h"

#include "util/debug.h"
#include "util/list.h"
#include "util/string.h"
#include "util/printf.h"

#include "proc/kthread.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "proc/proc.h"

#include "mm/slab.h"
#include "mm/page.h"
#include "mm/mmobj.h"
#include "mm/mm.h"
#include "mm/mman.h"

#include "vm/vmmap.h"

#include "fs/vfs.h"
#include "fs/vfs_syscall.h"
#include "fs/vnode.h"
#include "fs/file.h"

proc_t *curproc = NULL; /* global */
static slab_allocator_t *proc_allocator = NULL;

static list_t _proc_list;
static proc_t *proc_initproc = NULL; /* Pointer to the init process (PID 1) */

void
proc_init()
{
        list_init(&_proc_list);
        proc_allocator = slab_allocator_create("proc", sizeof(proc_t));
        KASSERT(proc_allocator != NULL);
}

static pid_t next_pid = 0;

/**
 * Returns the next available PID.
 *
 * Note: Where n is the number of running processes, this algorithm is
 * worst case O(n^2). As long as PIDs never wrap around it is O(n).
 *
 * @return the next available PID
 */
static int
_proc_getid()
{
        proc_t *p;
        pid_t pid = next_pid;
        while (1) {
failed:
                list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
                        if (p->p_pid == pid) {
                                if ((pid = (pid + 1) % PROC_MAX_COUNT) == next_pid) {
                                        return -1;
                                } else {
                                        goto failed;
                                }
                        }
                } list_iterate_end();
                next_pid = (pid + 1) % PROC_MAX_COUNT;
                return pid;
        }
}

/*
 * The new process, although it isn't really running since it has no
 * threads, should be in the PROC_RUNNING state.
 *
 * Don't forget to set proc_initproc when you create the init
 * process. You will need to be able to reference the init process
 * when reparenting processes to the init process.
 */
proc_t *
proc_create(char *name)
{
    /* Allocate and initialize fields for the process */
    proc_t* proc = (proc_t*)slab_obj_alloc(proc_allocator);

    /* Set prodess id bu calling function */
    proc->p_pid = _proc_getid();


    dbg_print("proc_create: PID: %i, Name: %s\n", proc->p_pid, name);

    /* Check to see if this is the first process, if it is set proc_initproc */
    if(proc->p_pid == PID_INIT)
    {
        proc_initproc = proc;
    }

    strcpy(proc->p_comm, name);

    /* proc->p_comm[256] = *name; */

    /* Initialize thread and child process lists */
    list_init(&proc->p_threads);
    list_init(&proc->p_children);
    sched_queue_init(&proc->p_wait);

    proc->p_pproc = curproc;
    proc->p_status = 0;
    proc->p_state = PROC_RUNNING;


    if(proc->p_pid != PID_IDLE)
    {
        dbg_print("proc_create: Parent PID: %i\n", curproc->p_pid);
    }

    /* Set up page table */
    proc->p_pagedir = pt_create_pagedir();
    
    /* Initialize list link and insert into process list */
    list_link_init(&proc->p_list_link);

    list_insert_tail(&_proc_list, &proc->p_list_link);

    /* Initialize child link and insert at end of parent's process list */
    list_link_init(&proc->p_child_link);
    /* Insert into parent process list as long as the process being created isn't the first */
    if(proc->p_pproc != NULL)  
    {
        list_insert_tail(&proc->p_pproc->p_children, &proc->p_child_link);
    }

    /* Other variables that don't seem to be in use */
    proc->p_files[NFILES] = 0;
    proc->p_cwd = 0;
    proc->p_brk = 0;
    proc->p_start_brk = 0;
    proc->p_vmmap = 0;

    return proc;
}

/**
 * Cleans up as much as the process as can be done from within the
 * process. This involves:
 *    - Closing all open files (VFS)
 *    - Cleaning up VM mappings (VM)
 *    - Waking up its parent if it is waiting
 *    - Reparenting any children to the init process
 *    - Setting its status and state appropriately
 *
 * The parent will finish destroying the process within do_waitpid (make
 * sure you understand why it cannot be done here). Until the parent
 * finishes destroying it, the process is informally called a 'zombie'
 * process.
 *
 * This is also where any children of the current process should be
 * reparented to the init process (unless, of course, the current
 * process is the init process. However, the init process should not
 * have any children at the time it exits).
 *
 * Note: You do _NOT_ have to special case the idle process. It should
 * never exit this way.
 *
 * @param status the status to exit the process with
 */
void
proc_cleanup(int status)
{
        /* KASSERT(curproc->p_pid != PID_IDLE); */
        /* dbg_print("p\n"); */
        proc_t* p_traverse;
        dbg_print("Cleaning up the Process  Pid: %i\n", curproc->p_pid);
        curproc->p_status = status;
        curproc->p_state = PROC_DEAD;
        /* Reparent process' children */
        dbg_print("Reparenting Processes of PID: %i to init Process\n", curproc->p_pid);
        /* dbg_print("q\n"); */
        list_iterate_begin(&curproc->p_children, p_traverse, proc_t, p_child_link)
        {
            p_traverse->p_pproc = proc_initproc;
            list_insert_tail(&proc_initproc->p_children, &p_traverse->p_child_link);
        } list_iterate_end();

        /* dbg_print("r\n"); */
        sched_wakeup_on(&curproc->p_wait);
        sched_wakeup_on(&curproc->p_pproc->p_wait);
        /* dbg_print("s\n"); */
        sched_switch();
}

/*
 * This has nothing to do with signals and kill(1).
 *
 * Calling this on the current process is equivalent to calling
 * do_exit().
 *
 * In Weenix, this is only called from proc_kill_all.
 */
void
proc_kill(proc_t *p, int status)
{
        dbg_print("proc_kill: Killing Proc PID: %i\n", p->p_pid);
        kthread_t* t_traverse;
        if(p == curproc)
        {
            do_exit(status);
        }
        else
        {
            list_iterate_begin(&p->p_threads, t_traverse, kthread_t, kt_plink)
            {
                kthread_cancel(t_traverse, NULL);
            } list_iterate_end();
        }
}

/*
 * Remember, proc_kill on the current process will _NOT_ return.
 * Don't kill direct children of the idle process.
 *
 * In Weenix, this is only called by sys_halt.
 */
void
proc_kill_all()
{
        proc_t* p_traverse;
        kthread_t* t_traverse;

        list_iterate_begin(&_proc_list, p_traverse, proc_t, p_list_link)
        {
            if( (p_traverse->p_pid != PID_IDLE) && (p_traverse->p_pproc->p_pid != PID_IDLE) && (p_traverse != curproc) )
            {
                proc_kill(p_traverse, 0);
            }
        } list_iterate_end();

        if(curproc->p_pid != PID_INIT)
        {
            do_exit(0);
        }
}

proc_t *
proc_lookup(int pid)
{
        proc_t *p;
        list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
                if (p->p_pid == pid) {
                        return p;
                }
        } list_iterate_end();
        return NULL;
}

list_t *
proc_list()
{
        return &_proc_list;
}

/*
 * This function is only called from kthread_exit.
 *
 * Unless you are implementing MTP, this just means that the process
 * needs to be cleaned up and a new thread needs to be scheduled to
 * run. If you are implementing MTP, a single thread exiting does not
 * necessarily mean that the process should be exited.
 */
void
proc_thread_exited(void *retval)
{
        proc_cleanup(0);
}

/* If pid is -1 dispose of one of the exited children of the current
 * process and return its exit status in the status argument, or if
 * all children of this process are still running, then this function
 * blocks on its own p_wait queue until one exits.
 *
 * If pid is greater than 0 and the given pid is a child of the
 * current process then wait for the given pid to exit and dispose
 * of it.
 *
 * If the current process has no children, or the given pid is not
 * a child of the current process return -ECHILD.
 *
 * Pids other than -1 and positive numbers are not supported.
 * Options other than 0 are not supported.
 */
pid_t
do_waitpid(pid_t pid, int options, int *status)
{
        dbg_print("do_waitpid: Wait PID: %i Current PID: %i\n", pid, curproc->p_pid);
        proc_t* p_traverse;
        kthread_t* t_traverse;
        kthread_t* t_waitpid;
    
        /* dbg_print("a\n"); */

        if(pid == -1)
        {
            /* dbg_print("b\n"); */
            if(list_empty(&curproc->p_children))
            {
                dbg_print("do_waitpid: curproc list is empty\n");
                /* dbg_print("c\n"); */
                return -ECHILD;
            }
            else
            {
                /* dbg_print("d\n"); */
                while(1)
                {
                    /* dbg_print("e\n"); */
                    list_iterate_begin(&curproc->p_children, p_traverse, proc_t, p_child_link)
                    {
                        if(p_traverse->p_state == PROC_DEAD)
                        {
                            *status = p_traverse->p_status;
                            dbg_print("do_waitpid: Destroying Threads\n");
                            list_iterate_begin(&p_traverse->p_threads, t_traverse, kthread_t, kt_plink)
                            {
                                kthread_destroy(t_traverse);
                            } list_iterate_end();
    
                            pid_t to_return = p_traverse->p_pid;

                            list_remove(&p_traverse->p_list_link);
                            list_remove(&p_traverse->p_child_link);

                            /* DESTROY PAGEDIR && FREE SLAB */
                            pt_destroy_pagedir(p_traverse->p_pagedir);
                            slab_obj_free(proc_allocator, p_traverse);

                            return to_return;
                        }
                    } list_iterate_end();
                    /* dbg_print("f\n"); */
                    dbg_print("All threads are currently running, blocking on p_wait\n");
                    sched_sleep_on(&curproc->p_wait);
                    /* dbg_print("g\n"); */
                }
                /* dbg_print("h\n"); */
            }
            /* dbg_print("i\n"); */
        }
        else
        {
            /* dbg_print("j\n"); */
            p_traverse = proc_lookup(pid);

            if(p_traverse == NULL)
            {
                return -ECHILD;
            }

            KASSERT(p_traverse->p_pid == pid);

            if((pid > 0) && (p_traverse->p_pproc == curproc))
            {
                sched_sleep_on(&p_traverse->p_wait);
                *status = p_traverse->p_status;
                dbg_print("do_waitpid: Destroying Threads\n");
                /* dbg_print("l\n"); */
                list_iterate_begin(&p_traverse->p_threads, t_traverse, kthread_t, kt_plink)
                {
                    kthread_destroy(t_traverse);
                } list_iterate_end();

                /* dbg_print("m\n"); */

                pid_t to_return = p_traverse->p_pid;

                list_remove(&p_traverse->p_list_link);
                list_remove(&p_traverse->p_child_link);

                /* DESTROY PAGEDIR && FREE SLAB */
                pt_destroy_pagedir(p_traverse->p_pagedir);
                slab_obj_free(proc_allocator, p_traverse);
                /* dbg_print("n\n"); */

                return to_return;
            }
        }
        /* dbg_print("o\n"); */
        return -ECHILD;
    

    /*
    * NOT_YET_IMPLEMENTED("PROCS: do_waitpid");
    * return 0;
    */
}

/*
 * Cancel all threads, join with them, and exit from the current
 * thread.
 *
 * @param status the exit status of the process
 */
void
do_exit(int status)
{
        /* Since each process contains only one thread, only need to deal with one */
        kthread_cancel(curthr, 0);
}

size_t
proc_info(const void *arg, char *buf, size_t osize)
{
        const proc_t *p = (proc_t *) arg;
        size_t size = osize;
        proc_t *child;

        KASSERT(NULL != p);
        KASSERT(NULL != buf);

        iprintf(&buf, &size, "pid:          %i\n", p->p_pid);
        iprintf(&buf, &size, "name:         %s\n", p->p_comm);
        if (NULL != p->p_pproc) {
                iprintf(&buf, &size, "parent:       %i (%s)\n",
                        p->p_pproc->p_pid, p->p_pproc->p_comm);
        } else {
                iprintf(&buf, &size, "parent:       -\n");
        }

#ifdef __MTP__
        int count = 0;
        kthread_t *kthr;
        list_iterate_begin(&p->p_threads, kthr, kthread_t, kt_plink) {
                ++count;
        } list_iterate_end();
        iprintf(&buf, &size, "thread count: %i\n", count);
#endif

        if (list_empty(&p->p_children)) {
                iprintf(&buf, &size, "children:     -\n");
        } else {
                iprintf(&buf, &size, "children:\n");
        }
        list_iterate_begin(&p->p_children, child, proc_t, p_child_link) {
                iprintf(&buf, &size, "     %i (%s)\n", child->p_pid, child->p_comm);
        } list_iterate_end();

        iprintf(&buf, &size, "status:       %i\n", p->p_status);
        iprintf(&buf, &size, "state:        %i\n", p->p_state);

#ifdef __VFS__
#ifdef __GETCWD__
        if (NULL != p->p_cwd) {
                char cwd[256];
                lookup_dirpath(p->p_cwd, cwd, sizeof(cwd));
                iprintf(&buf, &size, "cwd:          %-s\n", cwd);
        } else {
                iprintf(&buf, &size, "cwd:          -\n");
        }
#endif /* __GETCWD__ */
#endif

#ifdef __VM__
        iprintf(&buf, &size, "start brk:    0x%p\n", p->p_start_brk);
        iprintf(&buf, &size, "brk:          0x%p\n", p->p_brk);
#endif

        return size;
}

size_t
proc_list_info(const void *arg, char *buf, size_t osize)
{
        size_t size = osize;
        proc_t *p;

        KASSERT(NULL == arg);
        KASSERT(NULL != buf);

#if defined(__VFS__) && defined(__GETCWD__)
        iprintf(&buf, &size, "%5s %-13s %-18s %-s\n", "PID", "NAME", "PARENT", "CWD");
#else
        iprintf(&buf, &size, "%5s %-13s %-s\n", "PID", "NAME", "PARENT");
#endif

        list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
                char parent[64];
                if (NULL != p->p_pproc) {
                        snprintf(parent, sizeof(parent),
                                 "%3i (%s)", p->p_pproc->p_pid, p->p_pproc->p_comm);
                } else {
                        snprintf(parent, sizeof(parent), "  -");
                }

#if defined(__VFS__) && defined(__GETCWD__)
                if (NULL != p->p_cwd) {
                        char cwd[256];
                        lookup_dirpath(p->p_cwd, cwd, sizeof(cwd));
                        iprintf(&buf, &size, " %3i  %-13s %-18s %-s\n",
                                p->p_pid, p->p_comm, parent, cwd);
                } else {
                        iprintf(&buf, &size, " %3i  %-13s %-18s -\n",
                                p->p_pid, p->p_comm, parent);
                }
#else
                iprintf(&buf, &size, " %3i  %-13s %-s\n",
                        p->p_pid, p->p_comm, parent);
#endif
        } list_iterate_end();
        return size;
}
