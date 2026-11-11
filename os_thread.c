/*
 * os_thread.c - pthread helpers.
 */
#include "os_thread.h"

int os_mutex_init(os_mutex_t *m)
{
    return pthread_mutex_init(m, NULL);
}

void os_mutex_lock(os_mutex_t *m)
{
    pthread_mutex_lock(m);
}

void os_mutex_unlock(os_mutex_t *m)
{
    pthread_mutex_unlock(m);
}

void os_mutex_destroy(os_mutex_t *m)
{
    pthread_mutex_destroy(m);
}

int os_thread_create(os_thread_t *t, void *(*fn)(void *), void *arg)
{
    return pthread_create(t, NULL, fn, arg);
}

void os_thread_join(os_thread_t t)
{
    pthread_join(t, NULL);
}

void os_thread_detach(os_thread_t t)
{
    pthread_detach(t);
}
