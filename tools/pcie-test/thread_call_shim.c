#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/thread_call.h>

_Thread_local bool pcie_test_interrupts_disabled;
bool pcie_test_thread_call_free_fail;

struct thread_call {
	pthread_mutex_t lock;
	pthread_cond_t condition;
	pthread_t thread;
	thread_call_func_t function;
	void *context;
	uint64_t deadline;
	bool pending;
	bool running;
	bool stopping;
};

static void *
thread_call_loop(void *argument)
{
	struct thread_call *call = argument;
	pthread_mutex_lock(&call->lock);
	while (!call->stopping) {
		while (!call->pending && !call->stopping) {
			pthread_cond_wait(&call->condition, &call->lock);
		}
		if (call->stopping) {
			break;
		}
		if (call->deadline > pcie_test_clock()) {
			struct timespec deadline = {
				.tv_sec = (time_t)(call->deadline / 1000000000ULL), .tv_nsec = (long)(call->deadline % 1000000000ULL),
			};
			pthread_cond_timedwait(&call->condition, &call->lock, &deadline);
			continue;
		}
		call->pending = false;
		call->running = true;
		pthread_mutex_unlock(&call->lock);
		assert(!pcie_test_interrupts_disabled);
		call->function(call->context, NULL);
		assert(!pcie_test_interrupts_disabled);
		pthread_mutex_lock(&call->lock);
		call->running = false;
		pthread_cond_broadcast(&call->condition);
	}
	pthread_mutex_unlock(&call->lock);
	return NULL;
}

thread_call_t
thread_call_allocate_with_options(thread_call_func_t function, void *context, int priority, unsigned options)
{
	assert(priority == THREAD_CALL_PRIORITY_KERNEL && options == THREAD_CALL_OPTIONS_ONCE);
	struct thread_call *call = pcie_test_allocate(1, sizeof(*call));
	if (call == NULL) {
		return NULL;
	}
	call->function = function;
	call->context = context;
	assert(pthread_mutex_init(&call->lock, NULL) == 0);
	pthread_condattr_t attributes;
	assert(pthread_condattr_init(&attributes) == 0);
	assert(pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0);
	assert(pthread_cond_init(&call->condition, &attributes) == 0);
	pthread_condattr_destroy(&attributes);
	assert(pthread_create(&call->thread, NULL, thread_call_loop, call) == 0);
	return call;
}

boolean_t
thread_call_enter_delayed(thread_call_t call, uint64_t deadline)
{
	pthread_mutex_lock(&call->lock);
	assert(!call->stopping);
	bool pending = call->pending;
	call->deadline = deadline;
	call->pending = true;
	pthread_cond_broadcast(&call->condition);
	pthread_mutex_unlock(&call->lock);
	return pending;
}

boolean_t
thread_call_enter(thread_call_t call)
{
	return thread_call_enter_delayed(call, 0);
}

boolean_t
thread_call_cancel_wait(thread_call_t call)
{
	assert(!pcie_test_interrupts_disabled && !pthread_equal(call->thread, pthread_self()));
	pthread_mutex_lock(&call->lock);
	bool pending = call->pending;
	call->pending = false;
	while (call->running) {
		pthread_cond_wait(&call->condition, &call->lock);
	}
	pthread_mutex_unlock(&call->lock);
	return pending;
}

boolean_t
thread_call_free(thread_call_t call)
{
	if (pcie_test_thread_call_free_fail) {
		pcie_test_thread_call_free_fail = false;
		return false;
	}
	pthread_mutex_lock(&call->lock);
	if (call->pending || call->running) {
		pthread_mutex_unlock(&call->lock);
		return false;
	}
	call->stopping = true;
	pthread_cond_broadcast(&call->condition);
	pthread_mutex_unlock(&call->lock);
	assert(pthread_join(call->thread, NULL) == 0);
	pthread_cond_destroy(&call->condition);
	pthread_mutex_destroy(&call->lock);
	free(call);
	return true;
}
