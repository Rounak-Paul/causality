#include "thread.h"

#include <stdlib.h>

#ifdef _WIN32
#include <process.h>

typedef struct {
    Ca_ThreadFn fn;
    void       *user_data;
} Ca_ThreadTrampoline;

static unsigned __stdcall win32_thread_proc(void *arg)
{
    Ca_ThreadTrampoline *tramp = (Ca_ThreadTrampoline *)arg;
    tramp->fn(tramp->user_data);
    free(tramp);
    return 0;
}

Ca_Thread *ca_thread_create(Ca_ThreadFn fn, void *user_data)
{
    Ca_Thread *t = (Ca_Thread *)malloc(sizeof(Ca_Thread));
    if (!t) return NULL;
    Ca_ThreadTrampoline *tramp = (Ca_ThreadTrampoline *)malloc(sizeof(Ca_ThreadTrampoline));
    if (!tramp) { free(t); return NULL; }
    tramp->fn = fn;
    tramp->user_data = user_data;
    t->handle = (HANDLE)_beginthreadex(NULL, 0, win32_thread_proc, tramp, 0, NULL);
    if (!t->handle) { free(tramp); free(t); return NULL; }
    return t;
}

void ca_thread_join(Ca_Thread *thread)
{
    if (!thread) return;
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    free(thread);
}

void ca_thread_detach(Ca_Thread *thread)
{
    if (!thread) return;
    CloseHandle(thread->handle);
    free(thread);
}

#else /* POSIX */

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

#endif
