#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* List of sleeping threads */
static struct list sleeping_threads;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void) 
{
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
  list_init(&sleeping_threads);
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (loops_per_tick | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

/* Comparator function. 
   Returns true if thread A has less ticks than thread B,
   false otherwise. */
static bool
ticks_compare (const struct list_elem *a_, const struct list_elem *b_,
            void *aux UNUSED) 
{
  const struct thread *a = list_entry (a_, struct thread, sleep_elem);
  const struct thread *b = list_entry (b_, struct thread, sleep_elem);
  
  return a->sleep_ticks < b->sleep_ticks;
}

/* Put the current thread to sleep for TICKS timer ticks. Interrupts
must be turned on. */
void
thread_sleep(int64_t ticks)
{
  struct thread* cur = thread_current ();
  int64_t start = timer_ticks ();
  cur->sleep_ticks = start + ticks;
  enum intr_level old_level;
  // Put into the list
  // printf("yo dog? bun u sleeping man %s for %d sec\n", cur->name, cur->sleep_ticks);
  // Critical section: insert to list
  old_level = intr_disable ();
  list_insert_ordered(&sleeping_threads, &cur->sleep_elem, ticks_compare, NULL);
  // cur->status = THREAD_READY;
  intr_set_level (old_level);
  // check if we like actually put it in?
  struct list_elem *e;
  struct thread *t;
  // for (e = list_begin (&sleeping_threads); e != list_end (&sleeping_threads);
  //      e = list_next (e)) 
  // {
  //   t = list_entry (e, struct thread, sleep_elem);
  //   printf("name: %s\n", t->name);
  // }
  // Put the thread to sleep
  sema_down(&cur->sleep_sem);
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks) 
{
  int64_t start = timer_ticks ();
  ASSERT (intr_get_level () == INTR_ON);
  // Put thread to sleep
  thread_sleep (ticks);
  // while (timer_elapsed (start) < ticks) {
  //   // printf("I am %s and asleep for %d seconds \n", idk->name, ticks - timer_elapsed(start));
  //   // thread_sleep ();
  //   thread_yield ();
  // }
  // TODO: this should be 0 but like its not because you oversleep and miss the bus
  // TODO: also if you uncomment the print its gonna break bc print takes like 2 ticks or something idk
  // printf("wow i am %s and i woke up at %d\n", idk->name, ticks - timer_elapsed(start));
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  // Loop through through the list and wake up all threads that need to be awaken
  struct list_elem *e;
  struct thread *t;
  enum intr_level old_level;
  while(!list_empty (&sleeping_threads))
  {
    e = list_begin (&sleeping_threads);
    // e = list_next(e);
    t = list_entry (e, struct thread, sleep_elem);
    if (ticks >= t->sleep_ticks){
      printf("ticks: %d\n", ticks);
      printf("tickers: %d\n", t->sleep_ticks);
      printf("WAKE UP. WAKE UP.\n");
      printf("prev: %s\n", list_entry(list_prev(e), struct thread, sleep_elem)->name);
      printf("next: %s\n", list_entry(list_next(e), struct thread, sleep_elem)->name);
      sema_up(&t->sleep_sem);
      list_remove(e);
      old_level = intr_disable ();
      intr_set_level (old_level);
      // t->status = THREAD_RUNNING;
      for (e = list_begin (&sleeping_threads); e != list_end (&sleeping_threads);
       e = list_next (e)) 
      {
        t = list_entry (e, struct thread, sleep_elem);
        printf("%s woke up\n", t->name);
      }
    } 
    else
      break;
    
  }
  // for (e = list_begin (&sleeping_threads); e != list_end (&sleeping_threads);
  //      e = list_next (e))
  // {
  //   t = list_entry (e, struct thread, sleep_elem);
  //   if (ticks >= t->sleep_ticks){
  //     sema_up(&t->sleep_sem);
  //     list_pop_front(&sleeping_threads);
  //     // t->status = THREAD_RUNNING;
  //   } 
  //   else
  //     break;
  // }
  thread_tick ();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}
