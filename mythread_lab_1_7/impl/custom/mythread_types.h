#ifndef MYTHREAD_1_7_TYPES_H
#define MYTHREAD_1_7_TYPES_H

#include <stdint.h>

/* A stable, opaque handle.  The implementation owns the descriptor. */
typedef uint64_t mythread_t;
typedef struct mythread_attr mythread_attr_t;

#endif
