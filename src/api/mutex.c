/*
 * Userspace mutex implementation
 *
 * Copyright (C) 2009 Bahadir Bilgehan Balban
 */
#include <l4/lib/wait.h>
#include <l4/lib/mutex.h>
#include <l4/lib/printk.h>
#include <l4/generic/scheduler.h>
#include <l4/generic/kmalloc.h>
#include <l4/generic/tcb.h>
#include <l4/api/kip.h>
#include <l4/api/errno.h>
#include <l4/api/mutex.h>
#include INC_API(syscall.h)
#include INC_ARCH(exception.h)
#include INC_GLUE(memory.h)

struct mutex_queue {
	unsigned long physical;
	struct link list;
	struct waitqueue_head wqh_waiters;
	struct waitqueue_head wqh_wakers;
};

struct mutex_queue_head {
	struct link list;

	/*
	 * Single lock for:
	 * (1) Mutex_queue create/deletion
	 * (2) List add/removal.
	 * (3) Inspection of waitqueues:
	 *     - Both waitqueue spinlocks need to be acquired for
	 *       rendezvous inspection to occur atomically. Currently
	 *       it's not done since we rely on this mutex for that.
	 */
	struct mutex mutex_control_mutex;
	int count;
} mutex_queue_head;

void init_mutex_queue_head(void)
{
	memset(&mutex_queue_head, 0, sizeof (mutex_queue_head));
	link_init(&mutex_queue_head.list);
	mutex_init(&mutex_queue_head.mutex_control_mutex);
}
void mutex_queue_head_lock()
{
	mutex_lock(&mutex_queue_head.mutex_control_mutex);
}

void mutex_queue_head_unlock()
{
	mutex_unlock(&mutex_queue_head.mutex_control_mutex);
}


void mutex_queue_init(struct mutex_queue *mq, unsigned long physical)
{
	/* This is the unique key that describes this mutex */
	mq->physical = physical;

	link_init(&mq->list);
	waitqueue_head_init(&mq->wqh_wakers);
	waitqueue_head_init(&mq->wqh_waiters);
}

void mutex_control_add(struct mutex_queue *mq)
{
	BUG_ON(!list_empty(&mq->list));

	list_insert(&mq->list, &mutex_queue_head.list);
	mutex_queue_head.count++;
}

void mutex_control_remove(struct mutex_queue *mq)
{
	list_remove_init(&mq->list);
	mutex_queue_head.count--;
}

/* Note, this has ptr/negative error returns instead of ptr/zero. */
struct mutex_queue *mutex_control_find(unsigned long mutex_physical)
{
	struct mutex_queue *mutex_queue;

	/* Find the mutex queue with this key */
	list_foreach_struct(mutex_queue, &mutex_queue_head.list, list)
		if (mutex_queue->physical == mutex_physical)
			return mutex_queue;

	return 0;
}

struct mutex_queue *mutex_control_create(unsigned long mutex_physical)
{
	struct mutex_queue *mutex_queue;

	/* Allocate the mutex queue structure */
	if (!(mutex_queue = kzalloc(sizeof(struct mutex_queue))))
		return 0;

	/* Init and return */
	mutex_queue_init(mutex_queue, mutex_physical);

	return mutex_queue;
}

void mutex_control_delete(struct mutex_queue *mq)
{
	BUG_ON(!list_empty(&mq->list));

	/* Test internals of waitqueue */
	BUG_ON(mq->wqh_waiters.sleepers);
	BUG_ON(mq->wqh_wakers.sleepers);
	BUG_ON(!list_empty(&mq->wqh_waiters.task_list));
	BUG_ON(!list_empty(&mq->wqh_wakers.task_list));

	kfree(mq);
}

/*
 * A contended thread is expected to show up with the
 * contended mutex address here.
 *
 * (1) The mutex is converted into its physical form and
 *     searched for in the existing mutex list. If it does not
 *     appear there, it gets added.
 * (2) The thread is put to sleep in the mutex wait queue
 *     until a wake up event occurs. If there is already an asleep
 *     waker (i.e. unlocker) that is woken up and we return.
 */
int mutex_control_lock(unsigned long mutex_address)
{
	struct mutex_queue *mutex_queue;

	mutex_queue_head_lock();

	/* Search for the mutex queue */
	if (!(mutex_queue = mutex_control_find(mutex_address))) {
		/* Create a new one */
		if (!(mutex_queue = mutex_control_create(mutex_address))) {
			mutex_queue_head_unlock();
			return -ENOMEM;
		}
		/* Add the queue to mutex queue list */
		mutex_control_add(mutex_queue);
	} else {
		/* See if there is an unlocker */
		if (mutex_queue->wqh_wakers.sleepers) {
			/*
			 * If yes, wake it up async and we can *hope*
		 	 * to acquire the lock before the waker
		 	 */
			wake_up(&mutex_queue->wqh_wakers, WAKEUP_ASYNC);

			/* Since noone is left, delete the mutex queue */
			mutex_control_remove(mutex_queue);
			mutex_control_delete(mutex_queue);

			/* Release lock and return */
			mutex_queue_head_unlock();

			return 0;
		}
	}

	/* Prepare to wait on the waiters queue */
	CREATE_WAITQUEUE_ON_STACK(wq, current);
	wait_on_prepare(&mutex_queue->wqh_waiters, &wq);

	/* Release lock */
	mutex_queue_head_unlock();

	/* Initiate prepared wait */
	return wait_on_prepared_wait();

}

/*
 * A thread that has detected a contention on a mutex that
 * it had locked but has just released is expected to show up with
 * that mutex here.
 *
 * (1) The mutex is converted into its physical form and
 *     searched for in the existing mutex list. If not found,
 *     a new one is created and the thread sleeps there as a waker.
 * (2) All the threads waiting on this mutex are woken up. This may
 *     cause a thundering herd, but user threads cannot be trusted
 *     to acquire the mutex, waking up all of them increases the
 *     chances that some thread may acquire it.
 */
int mutex_control_unlock(unsigned long mutex_address)
{
	struct mutex_queue *mutex_queue;

	mutex_queue_head_lock();

	/* Search for the mutex queue */
	if (!(mutex_queue = mutex_control_find(mutex_address))) {

		/* No such mutex, create one and sleep on it */
		if (!(mutex_queue = mutex_control_create(mutex_address))) {
			mutex_queue_head_unlock();
			return -ENOMEM;
		}

		/* Add the queue to mutex queue list */
		mutex_control_add(mutex_queue);

		/* Prepare to wait on the wakers queue */
		CREATE_WAITQUEUE_ON_STACK(wq, current);
		wait_on_prepare(&mutex_queue->wqh_wakers, &wq);

		/* Release lock */
		mutex_queue_head_unlock();

		/* Initiate prepared wait */
		return wait_on_prepared_wait();
	}

	/*
	 * Found it, if it exists, there are waiters,
	 * now wake all of them up in FIFO order.
	 * FIXME: Make sure this is FIFO order. It doesn't seem so.
	 */
	wake_up(&mutex_queue->wqh_waiters, WAKEUP_ASYNC);

	/* Since noone is left, delete the mutex queue */
	mutex_control_remove(mutex_queue);
	mutex_control_delete(mutex_queue);

	/* Release lock and return */
	mutex_queue_head_unlock();
	return 0;
}

int sys_mutex_control(syscall_context_t *regs)
{
	unsigned long mutex_address = (unsigned long)regs->r0;
	int mutex_op = (int)regs->r1;
	unsigned long mutex_physical;
	int ret = 0;

	/* Check valid operation */
	if (mutex_op != MUTEX_CONTROL_LOCK &&
	    mutex_op != MUTEX_CONTROL_UNLOCK) {
		printk("Invalid args to %s.\n", __FUNCTION__);
		return -EINVAL;
	}

	/* Check valid user virtual address */
	if (!USER_ADDR(mutex_address)) {
		printk("Invalid args to %s.\n", __FUNCTION__);
		return -EINVAL;
	}

	/* Find and check physical address for virtual mutex address */
	if (!(mutex_physical =
		virt_to_phys_by_pgd(mutex_address,
				   TASK_PGD(current))))
		return -EINVAL;

	switch (mutex_op) {
	case MUTEX_CONTROL_LOCK:
		ret = mutex_control_lock(mutex_physical);
		break;
	case MUTEX_CONTROL_UNLOCK:
		ret = mutex_control_unlock(mutex_physical);
		break;
	default:
		printk("%s: Invalid operands\n", __FUNCTION__);
		ret = -EINVAL;
	}

	return ret;
}

