/*
 * os_thread.h - pthread thread/mutex wrappers (Linux).
 */
#ifndef OS_THREAD_H
#define OS_THREAD_H

#include <pthread.h>

typedef pthread_t       os_thread_t;
typedef pthread_mutex_t os_mutex_t;

int  os_mutex_init(os_mutex_t *m);
void os_mutex_lock(os_mutex_t *m);
void os_mutex_unlock(os_mutex_t *m);
void os_mutex_destroy(os_mutex_t *m);

/* Returns 0 on success. */
int  os_thread_create(os_thread_t *t, void *(*fn)(void *), void *arg);
void os_thread_join(os_thread_t t);
void os_thread_detach(os_thread_t t);

#endif /* OS_THREAD_H */
