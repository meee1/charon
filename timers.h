/* This file was automatically generated.  Do not edit! */
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
typedef struct timer_obj {
  struct timespec start;
} timer_obj;
long long timer_elapsed_since(timer_obj *o, long long now_usec);
long long timer_now_usec(void);
long long timer_elapsed_usec(timer_obj *o);
void timer_reset(timer_obj *o);
timer_obj *create_timer(void);
