#ifndef PCIE_TEST_THREAD_H
#define PCIE_TEST_THREAD_H

#include <pthread.h>

typedef pthread_t thread_t;
#define THREAD_NULL ((thread_t)0)
#define current_thread() pthread_self()

#endif
