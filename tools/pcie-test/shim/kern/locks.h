#ifndef PCIE_TEST_LOCKS_H
#define PCIE_TEST_LOCKS_H

#include <assert.h>
#include <pthread.h>

typedef struct { const char *name; } lck_grp_t;
typedef struct { pthread_mutex_t mutex; lck_grp_t *group; } lck_mtx_t;
typedef lck_mtx_t lck_spin_t;

#define LCK_ATTR_NULL NULL
#define LCK_GRP_DECLARE(name, label) lck_grp_t name = { label }
#define LCK_MTX_DECLARE(name, group) lck_mtx_t name = { PTHREAD_MUTEX_INITIALIZER, group }
#define LCK_SPIN_DECLARE(name, group) LCK_MTX_DECLARE(name, group)
#define lck_spin_init lck_mtx_init
#define lck_spin_destroy lck_mtx_destroy
#define lck_spin_lock lck_mtx_lock
#define lck_spin_unlock lck_mtx_unlock

static inline void
lck_mtx_init(lck_mtx_t *lock, lck_grp_t *group, void *attributes)
{
	assert(attributes == NULL);
	assert(pthread_mutex_init(&lock->mutex, NULL) == 0);
	lock->group = group;
}

static inline void
lck_mtx_destroy(lck_mtx_t *lock, lck_grp_t *group)
{
	assert(lock->group == group);
	assert(pthread_mutex_destroy(&lock->mutex) == 0);
}

static inline void
lck_mtx_lock(lck_mtx_t *lock)
{
	assert(pthread_mutex_lock(&lock->mutex) == 0);
}

static inline void
lck_mtx_unlock(lck_mtx_t *lock)
{
	assert(pthread_mutex_unlock(&lock->mutex) == 0);
}

#endif
