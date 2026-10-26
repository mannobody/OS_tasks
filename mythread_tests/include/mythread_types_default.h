#ifndef MYTHREAD_TYPES_DEFAULT_H
#define MYTHREAD_TYPES_DEFAULT_H

/*
 * Framework fallback for implementations that do not (yet) provide their
 * own mythread_types.h. The Makefile wires it up only when no header under
 * impl/custom/ defines mythread_t.
 *
 * mythread_attr_t is left incomplete: the tests only ever pass NULL, and an
 * implementation with real attributes must ship its own types header.
 */

typedef unsigned long mythread_t;
typedef struct mythread_attr mythread_attr_t;

#endif
