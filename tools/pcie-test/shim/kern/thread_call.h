#ifndef PCIE_TEST_THREAD_CALL_H
#define PCIE_TEST_THREAD_CALL_H

#include <stdint.h>
#include <machine/machine_routines.h>

typedef struct thread_call *thread_call_t;
typedef void *thread_call_param_t;
typedef void (*thread_call_func_t)(thread_call_param_t, thread_call_param_t);
#define THREAD_CALL_PRIORITY_KERNEL 1
#define THREAD_CALL_OPTIONS_ONCE 1

extern bool pcie_test_thread_call_free_fail;

thread_call_t thread_call_allocate_with_options(thread_call_func_t function, void *context, int priority, unsigned options);
boolean_t thread_call_enter(thread_call_t call);
boolean_t thread_call_enter_delayed(thread_call_t call, uint64_t deadline);
boolean_t thread_call_cancel_wait(thread_call_t call);
boolean_t thread_call_free(thread_call_t call);

#endif
