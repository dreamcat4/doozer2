#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/queue.h>
#include <syslog.h>
#include <sys/time.h>

#define DOOZER_ERROR_OTHER     -1
#define DOOZER_ERROR_PERMANENT -2  // Will not resolve itself until config is reread
#define DOOZER_ERROR_TRANSIENT -3  // Network errors, etc
#define DOOZER_ERROR_NO_DATA   -4
#define DOOZER_ERROR_INVALID_ARGS -5

#define COLOR_OFF    "\003"
#define COLOR_BLUE   "\0032"
#define COLOR_GREEN  "\0033"
#define COLOR_RED    "\0034"
#define COLOR_BROWN  "\0035"
#define COLOR_PURPLE "\0036"
#define COLOR_ORANGE "\0037"
#define COLOR_YELLOW "\0038"

void decolorize(char *str);
void trace(int level, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
void tracev(int level, const char *fmt, va_list ap);



extern void mutex_unlock_ptr(pthread_mutex_t **p);

#define scoped_lock(x) \
 pthread_mutex_t *scopedmutex__ ## __LINE__ \
 __attribute__((cleanup(mutex_unlock_ptr))) = x; \
 pthread_mutex_lock(x);

static inline int
atomic_add(volatile int *ptr, int incr)
{
  return __sync_fetch_and_add(ptr, incr);
}


#define mystrdupa(n) ({ int my_l = strlen(n); \
  char *my_b = alloca(my_l + 1); \
  memcpy(my_b, n, my_l + 1); })

static inline const char *mystrbegins(const char *s1, const char *s2)
{
  while(*s2)
    if(*s1++ != *s2++)
      return NULL;
  return s1;
}


static inline int64_t
get_ts(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}
