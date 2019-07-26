#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/syscall.h>
#include <signal.h>
#include "sicm_high.h"

profiler prof;
static global_signal = SIGRTMIN;

/* Function used by the profile threads to block/unblock
 * their own signal.
 */
void block_signal(int signal) {
	sigset_t mask;

  /* Block the signal */
  sigemptyset(&mask);
  sigaddset(&mask, signal);
  if(sigprocmask(SIG_SETMASK, &mask, NULL) == -1) {
    fprintf(stderr, "Error blocking signal %d. Aborting.\n", signal);
    exit(1);
  }

  /* Print out what time we were triggered */
  syscall(SYS_gettimeofday, &tv, NULL);
  printf("Profiling thread %d triggered: %ld.%06ld\n", signal, tv.tv_sec, tv.tv_usec);
  fflush(stdout);
}

/* Unblocks a signal. Also notifies the Master thread. */
void unblock_signal(int signal) {
	sigset_t mask;

  /* Signal the master thread that we're done */
  pthread_mutex_lock(&prof.mtx);
  syscall(SYS_gettimeofday, &tv, NULL);
  printf("Profiling thread %d done: %ld.%06ld\n", signal, tv.tv_sec, tv.tv_usec);
  fflush(stdout);
  prof.threads_finished++;
  pthread_cond_signal(&prof.cond);
  pthread_mutex_unlock(&prof.mtx);

  sigemptyset(&mask);
  sigaddset(&mask, signal);
  if(sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1) {
    fprintf(stderr, "Error unblocking signal. Aborting.\n");
    exit(1);
  }
}

/* This is the signal handler for the Master thread, so
 * it does this on every interval.
 */
void profile_master_interval(int s) {
	struct timeval tv;
  size_t i;
  unsigned copy;
  syscall(SYS_gettimeofday, &tv, NULL);

  printf("\n\n====================\n");
  printf("Master triggered at time %ld.%06ld\n", tv.tv_sec, tv.tv_usec);
  fflush(stdout);

  /* Notify the threads */
  for(i = 0; i < prof.num_profile_threads; i++) {
    pthread_kill(prof.profile_threads[i].id, prof.profile_threads[i].signal);
  }

  /* Wait for the threads to do their bit */
  pthread_mutex_lock(&prof.mtx);
  while(1) {
    if(prof.threads_finished) {
      /* At least one thread is finished, check if it's all of them */
      copy = prof.threads_finished;
      pthread_mutex_unlock(&prof.mtx);
      if(prof.threads_finished == 2) {
        /* They're all done. */
        printf("Profiling threads are all done.\n");
        fflush(stdout);
        pthread_mutex_lock(&prof.mtx);
        prof.threads_finished = 0;
        break;
      }
      /* At least one was done, but not all of them. Continue waiting. */
      pthread_mutex_lock(&prof.mtx);
    } else {
      /* Wait for at least one thread to signal us */
      pthread_cond_wait(&prof.cond, &prof.mtx);
    }
  }

  /* Finished handling this interval. Wait for another. */
  prof.cur_interval++;
  pthread_mutex_unlock(&prof.mtx);
  printf("Exited the loop, we're now ready to receive another signal\n");
  printf("====================\n");
  fflush(stdout);
}

void setup_profile_thread(void *(*main)(void *), /* Spinning loop function */
                          void (*interval)(int), /* Per-interval function */
                          unsigned long skip_intervals) {
  struct sigaction sa;
  profile_thread *profthread;

  /* Add a new profile_thread struct for it */
  prof.num_profile_threads++;
  prof.profile_threads = realloc(prof.profile_threads, sizeof(profile_thread) * prof.num_profile_threads);
  profthread = &(prof.profile_threads[prof.num_profile_threads - 1]);

  /* Start the thread */
  pthread_create(&(profthread->id), NULL, &main, NULL);

  /* Set up the signal handler */
  profthread->signal = global_signal;
	sa.sa_flags = 0;
  sa.sa_handler = interval;
  sigemptyset(&sa.sa_mask);
  if(sigaction(profthread->signal, &sa, NULL) == -1) {
    fprintf(stderr, "Error creating slave signal handler. Aborting.\n");
    exit(1);
  }

  profthread->skip_intervals = skip_intervals;

  /* Get ready for the next one */
  global_signal++;
}

/* This is the Master thread, it keeps track of intervals
 * and starts/stops the profiling threads. It has a timer
 * which signals it at a certain interval. Each time this
 * happens, it notifies the profiling threads.
 */
void *profile_master(void *a) {
  timer_t timerid;
  struct sigevent sev;
  struct sigaction sa;
  struct itimerspec its;
  long long frequency;
  sigset_t mask;
  pid_t tid;
  int master_signal;

  if(profopts.should_profile_all) {
    setup_profile_thread(&profile_all, &profile_all_interval, 0);
  }
  if(profopts.should_profile_rss) {
    setup_profile_thread(&profile_rss, &profile_rss_interval, 0);
  }
  if(profopts.should_profile_one) {
    setup_profile_thread(&profile_one, &profile_one_interval, 0);
  }
  if(profopts.should_profile_allocs) {
    setup_profile_thread(&profile_allocs, &profile_allocs_interval, 0);
  }
  
  /* Initialize synchronization primitives */
  pthread_mutex_init(&prof.mtx, NULL);
  pthread_cond_init(&prof.cond, NULL);
  prof.cur_interval = 0;

  /* Set up a signal handler for the master */
  sa.sa_flags = 0;
  sa.sa_handler = profile_master_interval;
  sigemptyset(&sa.sa_mask);
  if(sigaction(master_signal, &sa, NULL) == -1) {
    fprintf(stderr, "Error creating signal handler. Aborting.\n");
    exit(1);
  }

  /* Block the signal for a bit */
  sigemptyset(&mask);
  sigaddset(&mask, master_signal);
  if(sigprocmask(SIG_SETMASK, &mask, NULL) == -1) {
    fprintf(stderr, "Error blocking signal. Aborting.\n");
    exit(1);
  }

  /* Create the timer */
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = master_signal;
  sev.sigev_value.sival_ptr = &timerid;
  sev._sigev_un._tid = *tid;
  if(timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
    fprintf(stderr, "Error creating timer. Aborting.\n");
    exit(1);
  }
  
  /* Set the timer */
  its.it_value.tv_sec = profopts.profile_rate_nseconds / 1000000000;
  its.it_value.tv_nsec = profopts.profile_rate_nseconds % 1000000000;
  its.it_interval.tv_sec = its.it_value.tv_sec;
  its.it_interval.tv_nsec = its.it_value.tv_nsec;
  if(timer_settime(timerid, 0, &its, NULL) == -1) {
    fprintf(stderr, "Error setting the timer. Aborting.\n");
    exit(1);
  }

  /* Unblock the signal */
  if(sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1) {
    fprintf(stderr, "Error unblocking signal. Aborting.\n");
    exit(1);
  }

  /* Now we wait for the timer to signal us */
  /* TODO: implement how to stop the master thread */
  while(1) {}
}


void initialize_profiling() {
  size_t i;
  pid_t pid;
  int cpu, group_fd;
  unsigned long flags;

  /* All of this initialization HAS to happen in the main SICM thread.
   * If it's not, the `perf_event_open` system call won't profile
   * the current thread, but instead will only profile the thread that
   * it was run in.
   */

  prof.pagesize = (size_t) sysconf(_SC_PAGESIZE);

  /* Allocate perf structs */
  prof.pes = malloc(sizeof(struct perf_event_attr *) * profopts.num_events);
  prof.fds = malloc(sizeof(int) * profopts.num_events);
  for(i = 0; i < profopts.num_events; i++) {
    prof.pes[i] = malloc(sizeof(struct perf_event_attr));
    prof.fds[i] = 0;
  }

  /* Use libpfm to fill the pe struct */
  if(profopts.should_profile_all || profopts.should_profile_one) {
    sh_get_event();
  }

  /* Open all perf file descriptors, different arguments for each type
   * of profiling.
   */
  if(profopts.should_profile_all) {
    pid = 0;
    cpu = -1;
    group_fd = -1;
    flags = 0;
  } else if(profopts.should_profile_one) {
    pid = -1;
    cpu = 0;
    group_fd = -1;
    flags = 0;
  }
  for(i = 0; i < profopts.num_events; i++) {
    prof.fds[i] = syscall(__NR_perf_event_open, prof.pes[i], pid, cpu, group_fd, flags);
    if(prof.fds[i] == -1) {
      fprintf(stderr, "Error opening perf event %d (0x%llx): %s\n", i, prof.pes[i]->config, strerror(errno));
      exit(1);
    }
  }

  if(profopts.should_profile_rss) {
    prof.pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (prof.pagemap_fd < 0) {
      fprintf(stderr, "Failed to open /proc/self/pagemap. Aborting.\n");
      exit(1);
    }
    prof.pfndata = NULL;
    prof.addrsize = sizeof(uint64_t);
    prof.pagesize = (size_t) sysconf(_SC_PAGESIZE);
  }
}

void sh_start_profile_master_thread() {

  /* This initializes the values that the threads will need to do their profiling,
   * including perf events, file descriptors, etc.
   */
  initialize_profiling();

  /* All the main thread should do is start the master thread */
  pthread_create(&master_id, NULL, &master_profile, NULL);
}

void deinitialize_profiling() {
  for(i = 0; i < profopts.num_events; i++) {
    ioctl(prof.fds[i], PERF_EVENT_IOC_DISABLE, 0);
  }

  for(i = 0; i < profopts.num_events; i++) {
    close(prof.fds[i]);
  }
}

void print_profiling() {
  /* PEBS profiling */
  if(profopts.should_profile_all) {
    printf("===== PEBS RESULTS =====\n");
    for(i = 0; i <= tracker.max_index; i++) {
      if(!tracker.arenas[i]) continue;

      /* Print the sites that are in this arena */
      printf("%d sites: ", tracker.arenas[i]->num_alloc_sites);
      for(n = 0; n < tracker.arenas[i]->num_alloc_sites; n++) {
        printf("%d ", tracker.arenas[i]->alloc_sites[n]);
      }
      printf("\n");

      /* Print the RSS of the arena */
      if(profopts.should_profile_rss) {
        printf("  Peak RSS: %zu\n", tracker.arenas[i]->peak_rss);
      }
      printf("    Number of intervals: %zu\n", tracker.arenas[i]->num_intervals);
      printf("    First interval: %zu\n", tracker.arenas[i]->first_interval);

      /* Print information for each event */
      for(n = 0; n < profopts.num_events; n++) {
        printf("  Event: %s\n", profopts.events[n]);
        printf("    Total: %zu\n", tracker.arenas[i]->profiles[n].total);
        for(x = 0; x < tracker.arenas[i]->num_intervals; x++) {
          printf("      %zu\n", tracker.arenas[i]->profiles[n].interval_vals[x]);
        }
      }
    }
    printf("===== END PEBS RESULTS =====\n");

  /* MBI profiling */
  } else if(profopts.should_profile_one) {
    printf("===== MBI RESULTS FOR SITE %u =====\n", profopts.profile_one_site);
    printf("Average bandwidth: %.1f MB/s\n", prof.running_avg);
    printf("Maximum bandwidth: %.1f MB/s\n", prof.max_bandwidth);
    if(profopts.should_profile_rss) {
      printf("Peak RSS: %zu\n", tracker.arenas[profopts.profile_one_site]->peak_rss);
    }
    printf("===== END MBI RESULTS =====\n");

  /* RSS profiling */
  } else if(profopts.should_profile_rss) {
    printf("===== RSS RESULTS =====\n");
    for(i = 0; i <= tracker.max_index; i++) {
      if(!tracker.arenas[i]) continue;
      printf("Sites: ");
      for(n = 0; n < tracker.arenas[i]->num_alloc_sites; n++) {
        printf("%d ", tracker.arenas[i]->alloc_sites[n]);
      }
      printf("\n");
      if(profopts.should_profile_rss) {
        printf("  Peak RSS: %zu\n", tracker.arenas[i]->peak_rss);
      }
    }
    printf("===== END RSS RESULTS =====\n");
  }
}

void sh_stop_profile_master_thread() {
  size_t i, n, x;

  pthread_join(master_id, NULL);

  deinitialize_profiling();
  print_profiling();
}

