#ifndef PCIE_TEST_LINUX_MUTEX_H
#define PCIE_TEST_LINUX_MUTEX_H
#include <assert.h>
#include <pthread.h>
struct mutex { pthread_mutex_t native; };
static inline void mutex_init(struct mutex *lock) { assert(pthread_mutex_init(&lock->native, NULL) == 0); }
static inline void mutex_lock(struct mutex *lock) { assert(pthread_mutex_lock(&lock->native) == 0); }
static inline void mutex_unlock(struct mutex *lock) { assert(pthread_mutex_unlock(&lock->native) == 0); }
static inline void mutex_destroy(struct mutex *lock) { assert(pthread_mutex_destroy(&lock->native) == 0); }
#endif
