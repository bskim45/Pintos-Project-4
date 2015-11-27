#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "lib/kernel/list.h"
#include "filesys/file.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of processes in THREAD_BLOCK state
   made by jh2ekd */
static struct list sleep_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static int fixed_load_avg;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

#define ready_threads ((int)list_size(&ready_list) + (thread_current () != idle_thread? 1 : 0))

#define fixed_point_f 16384 //16384 = 2^14. It is 17.14 fixed-point
#define CALC_PRIORITY_TICK 4

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleep_list);

  fixed_load_avg = conv2fixed (0);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

  if (thread_mlfqs){
    initial_thread->fixed_recent_cpu = conv2fixed(0);
    initial_thread->nice = 0;
  }
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();
  struct thread *t_e;
  struct list_elem *e;
  bool any_unblock = false;
  enum intr_level old_level;

  old_level = intr_disable ();
  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Think about race condition */
  /* calculate load_avg if thread_mlfqs is ture */
  if (thread_mlfqs){
    t->fixed_recent_cpu = add_x_n (t->fixed_recent_cpu,1);

    if (timer_ticks()%TIMER_FREQ==0){
      /* calc load_avg */
      fixed_load_avg = add_x_y (mul_x_y (div_x_n (conv2fixed (59), 60), fixed_load_avg),
	  mul_x_n (div_x_n (conv2fixed (1), 60), ready_threads));
      if (!list_empty(&all_list)){
  	for (e = list_begin (&all_list); e != list_tail(&all_list); e = list_next (e)){
  	  t_e = list_entry(e, struct thread, allelem);
  	  if (t_e != idle_thread)
  	    t_e->fixed_recent_cpu = add_x_n (mul_x_y (div_x_y (mul_x_n (fixed_load_avg, 2),
  		    add_x_n (mul_x_n (fixed_load_avg, 2), 1)), t_e->fixed_recent_cpu), t_e->nice);
  	}
      }
    }
    if ((timer_ticks()%CALC_PRIORITY_TICK==0) && (!list_empty(&all_list))){
      for (e = list_begin (&all_list); e != list_tail(&all_list); e = list_next (e)){
	t_e = list_entry(e, struct thread, allelem);
	if (t_e != idle_thread){
//  	  thread_donate_priority (t_e, PRI_MAX-round2int (div_x_n (t_e->fixed_recent_cpu, 4))-((t_e->nice)*2));
	  t_e->priority = PRI_MAX-round2int (div_x_n (t_e->fixed_recent_cpu, 4))-((t_e->nice)*2);
/*	  if (t_e->status == THREAD_READY){
	    list_remove(&t_e->elem);
	    list_insert_ordered (&ready_list, &t_e->elem, (list_less_func*)thread_priority_more, NULL);
	  }*/
	}
      }
      intr_yield_on_return();
    }
  }

  /* Wake sleeping threads */
  while (list_entry(list_begin (&sleep_list), struct thread, sleepelem) -> waketime <=timer_ticks()
      && list_begin (&sleep_list) != list_end (&sleep_list)){
    thread_unblock(list_entry(list_pop_front(&sleep_list), struct thread, sleepelem));
    any_unblock = true;
  }

  if (any_unblock)
    intr_yield_on_return();
  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
  intr_set_level (old_level);
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  //old_level = intr_disable ();
  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* inherit parent thread's fixed_recent_cpu */
  if (thread_mlfqs){
    t->fixed_recent_cpu = thread_current()->fixed_recent_cpu;
    t->nice = thread_current()->nice;
  }
  
  /* For Project #2 */
  list_push_back (&thread_current()->children_list, &t->childelem);
  /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);

  /* Add to run queue. */
  thread_unblock (t);

  /* start most high priority thread */
  thread_yield();

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  
  list_insert_ordered (&ready_list, &t->elem, (list_less_func*)thread_priority_more, NULL);
  t->status = THREAD_READY;

  intr_set_level (old_level);
}

/* put thread into sleep_list and block it */
void
thread_sleep (int64_t waketime)
{
  void* aux;
  struct thread* cur = thread_current();
  enum intr_level old_level;

  old_level = intr_disable ();
  (cur->waketime) = waketime;
  aux = 0;
  list_insert_ordered (&sleep_list, &(cur->sleepelem),
      (list_less_func*)thread_waketime_less, aux);
  thread_block ();
  intr_set_level (old_level);
}

bool thread_waketime_less (const struct list_elem *a,
    const struct list_elem *b, void *aux UNUSED){
  struct thread* thread_a = (struct thread*)(list_entry(a, struct thread, sleepelem));
  struct thread* thread_b = (struct thread*)(list_entry(b, struct thread, sleepelem));
  if ( thread_a->waketime == thread_b->waketime )
    return thread_a->priority > thread_b->priority?
      true : false;
  else
  return thread_a->waketime < thread_b->waketime?
    true : false;
}

bool thread_priority_more (const struct list_elem *a,
    const struct list_elem *b, void *aux UNUSED){
  struct thread* thread_a = (struct thread*)(list_entry(a, struct thread, elem));
  struct thread* thread_b = (struct thread*)(list_entry(b, struct thread, elem));
  return thread_a->priority > thread_b->priority?
    true : false;
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it call thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur->priority <= list_entry(list_begin(&ready_list), struct thread, elem)->priority){
    if (cur != idle_thread){ 
      list_insert_ordered (&ready_list, &cur->elem, (list_less_func*)thread_priority_more, NULL);
      cur->status = THREAD_READY;
      schedule ();
    }
  }
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  struct thread* current_t = thread_current ();
  int alt_priority = 0;
  current_t->orig_priority = new_priority;
  current_t->priority = new_priority;
  if (!list_empty (&current_t->lock_list) && !is_thread_mlfqs()){
    alt_priority = lock_priority(list_entry(list_max (&current_t->lock_list,
	    (list_less_func*)lock_priority_less, NULL), struct lock, elem));
    if (current_t->priority < alt_priority)
      current_t->priority = alt_priority;
  }
  if (thread_current ()->priority < new_priority && !is_thread_mlfqs())
    thread_current ()->priority = new_priority;
  thread_yield();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* can change priority when block or ready, by priority donation */
void
thread_donate_priority(struct thread *thread, const int new_priority)
{
  thread->priority = new_priority;
  if (thread->locked_by != NULL
      && thread->locked_by->holder->priority < new_priority){
    thread_donate_priority (thread->locked_by->holder, new_priority);
  }
  /* if it is in ready_list, rearrange ready_list */
  else if (thread->status == THREAD_READY){
    list_remove (&thread->elem);
    list_insert_ordered (&ready_list, &thread->elem,
	(list_less_func*)thread_priority_more, NULL);
    if (!intr_context())
      thread_yield();
  }
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) 
{
  struct thread *t = thread_current();
  t->nice = nice;
  t->priority =  PRI_MAX-div_x_n (t->fixed_recent_cpu, 4)-(nice*2);
  thread_yield();
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return round2int (mul_x_n (fixed_load_avg, 100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return round2int (mul_x_n (thread_current()->fixed_recent_cpu, 100));
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->orig_priority = priority;
  t->priority = priority;
  t->magic = THREAD_MAGIC;
  t->locked_by = NULL;
  t->exit_code = 0;
  t->end = false;
  t->success_to_load = false;
  sema_init (&t->wait_this, 0); /* For Project #2 */
  sema_init (&t->kill_this, 0);
  sema_init (&t->wait_start_process, 0);
  list_init (&t->children_list);
  list_init (&t->lock_list);

  memset (&t->fd_list, 0, (sizeof(struct file*)) * FD_MAX);
  t->fd_num = 2;
  list_push_back (&all_list, &t->allelem);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  struct list_elem *t_e;
  if (list_empty (&ready_list))
    return idle_thread;
  else{
    if (thread_mlfqs){
      t_e = list_min (&ready_list, (list_less_func*)thread_priority_more, NULL);
      list_remove (t_e);
      return list_entry(t_e, struct thread, elem);
    }
    else
      return list_entry (list_pop_front (&ready_list), struct thread, elem);
  }
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev) 
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail() has
   completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev); 
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

void
print_ready_list(void)
{
  struct list* list = &ready_list;
  struct list_elem *e;
  for(e = list_begin (list); e != list_end (list); e = list_next(e))
    printf("-> %s ", list_entry(e, struct thread, elem)->name);
  printf("\n");
}

int conv2fixed (int n){
  return n*fixed_point_f;
}
int floor2int (int x){
  return x/fixed_point_f;
}
int round2int (int x){
  return x > 0?
    (x+fixed_point_f/2)/fixed_point_f
    :(x-fixed_point_f/2)/fixed_point_f;
}
int add_x_y (int x, int y){
  return x+y;
}
int sub_x_y (int x, int y){
  return x-y;
}
int add_x_n (int x, int n){
  return x+n*fixed_point_f;
}
int sub_x_n (int x, int n){
  return x-n*fixed_point_f;
}
int mul_x_y (int x, int y){
  return ((int64_t)x)*y/fixed_point_f;
}
int mul_x_n (int x, int n){
  return x*n;
}
int div_x_y (int x, int y){
  return ((int64_t)x)*fixed_point_f/y;
}
int div_x_n (int x, int n){
  return x/n;
}

int get_test_readys (void){
  return ready_threads;
}

bool is_thread_mlfqs (void){
  return thread_mlfqs;
}


/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

