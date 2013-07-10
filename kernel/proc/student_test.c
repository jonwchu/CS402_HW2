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
#include "proc/kmutex.h"

#include "mm/slab.h"
#include "mm/page.h"
#include "mm/mmobj.h"
#include "mm/mm.h"
#include "mm/mman.h"

ktqueue_t wake_me_q;
int wake_me_len2 = 0;
int race2=0;
int rwval = 0;
int num_readers = 0;

kmutex_t mutex;
kmutex_t reader_mutex;
kmutex_t writer_mutex;

typedef struct {
    struct proc *p;
    struct kthread *t;
} proc_thread_t;

/*
 * Create a process and a thread with the given name and calling teh given
 * function. Arg1 is passed to the function (arg2 is always NULL).  The thread
 * is immediately placed on the run queue.  A proc_thread_t is returned, giving
 * the caller a pointer to the new process and thread to coordinate tests.  NB,
 * the proc_thread_t is returned by value, so there are no stack problems.
 */
static proc_thread_t start_proc(char *name, kthread_func_t f, int arg1) {
    proc_thread_t pt;

    pt.p = proc_create(name);
    pt.t = kthread_create(pt.p, f, arg1, NULL);
    KASSERT(pt.p && pt.t && "Cannot create thread or process");
    sched_make_runnable(pt.t);
    return pt;
}

/**
 * Call do_waitpid with the process ID of the given process.  Print a debug
 * message with the exiting process's status.
 */
static void wait_for_proc(proc_t *p) {
    int rv;
    pid_t pid;
    char pname[PROC_NAME_LEN];

    strncpy(pname, p->p_comm, PROC_NAME_LEN); 
    pid = do_waitpid(p->p_pid, 0, &rv);
    dbg_print("%s (%d) exited: %d\n", pname, pid, rv);
}

/**
 * Call waitpid with a -1 pid and print a message about any process that exits.
 * Returns the pid found, including -ECHILD when this process has no children.
 */
static pid_t wait_for_any() {
    int rv;
    pid_t pid;

    pid = do_waitpid(-1, 0, &rv);
    if ( pid != -ECHILD) dbg_print("child (%d) exited: %d\n", pid, rv);
    return pid;
}

/*
 * Repeatedly call wait_for_any() until it returns -ECHILD
 */
static void wait_for_all() {
    while (wait_for_any() != -ECHILD) ;
}

/*
 * Keep context switching until *count >= tot.  Used to count the number of
 * nodes waiting for the kernel thread queue.
 */
static void stop_until_queued(int tot, int *count) {
    while ( *count < tot) 
	sched_switch();
}

/* This test checks to make sure that when you sleep
* threads, and broadcast later, they are popped in the 
* right order
*/
void *broadcast_order(int arg1, void *arg2) {
    int local,local2;

    if ( kmutex_lock_cancellable(&mutex) ) 
    {
		dbg_print("Mutex cancelled? %d", curproc->p_pid);
		do_exit(-1);
    }

    local = race2;
    local++;
    race2 = local;
    kmutex_unlock(&mutex);

    wake_me_len2++;
    sched_sleep_on(&wake_me_q);
    wake_me_len2--;

    if ( kmutex_lock_cancellable(&mutex) ) 
    {
		dbg_print("Mutex cancelled? %d", curproc->p_pid);
		do_exit(-1);
    }

    local2 = race2;
    local2++;
    race2 = local2;
    kmutex_unlock(&mutex);
    dbg_print("broadcast_order: Local: %i ", local);
    dbg_print("Local2: %i\n", local2);
    KASSERT(local == local2 && "Error on broadcast_order");
    do_exit(local);

    /*dbg_print("mutex_test: Local: %i\n", local);*/

    return NULL;
}

/* This tests is for the reader writer test, it reads a value, and
* checks to make sure that it is the same value that is expected.
*/
void *reader(int arg1, void *arg2) {
	kmutex_lock(&reader_mutex);
	kmutex_lock(&mutex);
	num_readers++;
	if(num_readers == 1)
	{
		kmutex_lock(&writer_mutex);
	}
	kmutex_unlock(&mutex);
	kmutex_unlock(&reader_mutex);

	sched_switch();

	dbg_print("reader_test: Expected to Read: %i, Did Read: %i\n", arg1, rwval);
	KASSERT(arg1 == rwval);

	kmutex_lock(&mutex);
	num_readers--;
	if(num_readers == 0)
	{
		kmutex_unlock(&writer_mutex);
	}
	kmutex_unlock(&mutex);

	return NULL;
}

/* This test writes to the value and makes sure that there are no race conditions */
void *writer(int arg1, void *arg2) {
	kmutex_lock(&reader_mutex);
	kmutex_lock(&writer_mutex);

	sched_switch();

	rwval++;

	kmutex_unlock(&writer_mutex);
	kmutex_unlock(&reader_mutex);
	return NULL;
}

void *student_tests(int arg1, void *arg2) { 
	kmutex_init(&mutex);
	kmutex_init(&reader_mutex);
	kmutex_init(&writer_mutex);
    
	/* Student Test 1 */
	int rv = 0;
    int i = 0;
    dbg_print("broadcast order test");
    race2 = 0;
    for (i = 0; i < 10; i++ ) 
	start_proc("broadcast order test", broadcast_order, 0);
    
    stop_until_queued(10, &wake_me_len2);
    race2 = 0;
    sched_broadcast_on(&wake_me_q);
    wait_for_all();
    KASSERT(wake_me_len2 == 0 && "Error on wakeme bookkeeping");

    /* Student Test 2 */
    int checkVal = 0;
    dbg_print("reader writer test");
    start_proc("reader writer test", reader, 0);
    start_proc("reader writer test", reader, 0);
    start_proc("reader writer test", writer, 0);
    checkVal++;
    start_proc("reader writer test", reader, 1);
    start_proc("reader writer test", reader, 1);
    start_proc("reader writer test", reader, 1);
    start_proc("reader writer test", writer, 0);
    checkVal++;
    start_proc("reader writer test", writer, 0);
    checkVal++;
    start_proc("reader writer test", reader, 3);
    start_proc("reader writer test", reader, 3);
    wait_for_all();
    KASSERT(checkVal == rwval && "Error in Reader Writer!");

    return NULL;
}

