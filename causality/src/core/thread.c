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

Ca_Mutex *ca_mutex_create(void)
{
    Ca_Mutex *m = (Ca_Mutex *)malloc(sizeof(Ca_Mutex));
    if (!m) return NULL;
    InitializeCriticalSection(&m->cs);
    return m;
}

void ca_mutex_destroy(Ca_Mutex *mutex)
{
    if (!mutex) return;
    DeleteCriticalSection(&mutex->cs);
    free(mutex);
}

void ca_mutex_lock(Ca_Mutex *mutex)
{
    if (!mutex) return;
    EnterCriticalSection(&mutex->cs);
}

void ca_mutex_unlock(Ca_Mutex *mutex)
{
    if (!mutex) return;
    LeaveCriticalSection(&mutex->cs);
}

bool ca_mutex_trylock(Ca_Mutex *mutex)
{
    if (!mutex) return false;
    return TryEnterCriticalSection(&mutex->cs) != 0;
}

Ca_CondVar *ca_condvar_create(void)
{
    Ca_CondVar *cv = (Ca_CondVar *)malloc(sizeof(Ca_CondVar));
    if (!cv) return NULL;
    InitializeConditionVariable(&cv->cv);
    return cv;
}

void ca_condvar_destroy(Ca_CondVar *cv)
{
    if (!cv) return;
    free(cv);
}

void ca_condvar_wait(Ca_CondVar *cv, Ca_Mutex *mutex)
{
    if (!cv || !mutex) return;
    SleepConditionVariableCS(&cv->cv, &mutex->cs, INFINITE);
}

void ca_condvar_signal(Ca_CondVar *cv)
{
    if (!cv) return;
    WakeConditionVariable(&cv->cv);
}

void ca_condvar_broadcast(Ca_CondVar *cv)
{
    if (!cv) return;
    WakeAllConditionVariable(&cv->cv);
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

Ca_Mutex *ca_mutex_create(void)
{
    Ca_Mutex *m = (Ca_Mutex *)malloc(sizeof(Ca_Mutex));
    if (!m) return NULL;
    if (pthread_mutex_init(&m->handle, NULL) != 0) {
        free(m);
        return NULL;
    }
    return m;
}

void ca_mutex_destroy(Ca_Mutex *mutex)
{
    if (!mutex) return;
    pthread_mutex_destroy(&mutex->handle);
    free(mutex);
}

void ca_mutex_lock(Ca_Mutex *mutex)
{
    if (!mutex) return;
    pthread_mutex_lock(&mutex->handle);
}

void ca_mutex_unlock(Ca_Mutex *mutex)
{
    if (!mutex) return;
    pthread_mutex_unlock(&mutex->handle);
}

bool ca_mutex_trylock(Ca_Mutex *mutex)
{
    if (!mutex) return false;
    return pthread_mutex_trylock(&mutex->handle) == 0;
}

Ca_CondVar *ca_condvar_create(void)
{
    Ca_CondVar *cv = (Ca_CondVar *)malloc(sizeof(Ca_CondVar));
    if (!cv) return NULL;
    if (pthread_cond_init(&cv->handle, NULL) != 0) {
        free(cv);
        return NULL;
    }
    return cv;
}

void ca_condvar_destroy(Ca_CondVar *cv)
{
    if (!cv) return;
    pthread_cond_destroy(&cv->handle);
    free(cv);
}

void ca_condvar_wait(Ca_CondVar *cv, Ca_Mutex *mutex)
{
    if (!cv || !mutex) return;
    pthread_cond_wait(&cv->handle, &mutex->handle);
}

void ca_condvar_signal(Ca_CondVar *cv)
{
    if (!cv) return;
    pthread_cond_signal(&cv->handle);
}

void ca_condvar_broadcast(Ca_CondVar *cv)
{
    if (!cv) return;
    pthread_cond_broadcast(&cv->handle);
}

#endif
