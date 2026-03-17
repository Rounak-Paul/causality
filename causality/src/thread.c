#include "thread.h"

#include <stdlib.h>

Ca_Thread *ca_thread_create(Ca_ThreadFn fn, void *user_data)
{
    Ca_Thread *t = (Ca_Thread *)malloc(sizeof(Ca_Thread));
    if (!t) return NULL;
    if (pthread_create(&t->handle, NULL, fn, user_data) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void ca_thread_join(Ca_Thread *thread)
{
    if (!thread) return;
    pthread_join(thread->handle, NULL);
    free(thread);
}

void ca_thread_detach(Ca_Thread *thread)
{
    if (!thread) return;
    pthread_detach(thread->handle);
    free(thread);
}
