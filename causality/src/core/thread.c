// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "thread.h"

#ifdef _WIN32
#include <process.h>

typedef struct {
    Ca_ThreadFn fn;
    void       *user_data;
} Ca_ThreadTrampoline;

/*
 * Win32 thread entry point that adapts _beginthreadex to Ca_ThreadFn.
 *
 * Invokes the user function, then frees the heap-allocated trampoline before
 * returning so the handle lifetime is independent of the stack.
 *
 * arg  Heap-allocated Ca_ThreadTrampoline; ownership is transferred here.
 */
static unsigned __stdcall win32_thread_proc(void *arg)
{
    Ca_ThreadTrampoline *tramp = (Ca_ThreadTrampoline *)arg;
    tramp->fn(tramp->user_data);
    CA_FREE(tramp);
    return 0;
}

/*
 * Create and start a new thread running fn(user_data).
 *
 * fn         Function to execute on the new thread.
 * user_data  Opaque argument forwarded to fn.
 * Returns    Allocated Ca_Thread handle, or NULL on failure.
 */
Ca_Thread *ca_thread_create(Ca_ThreadFn fn, void *user_data)
{
    Ca_Thread *t = (Ca_Thread *)CA_MALLOC(sizeof(Ca_Thread));
    if (!t) return NULL;
    Ca_ThreadTrampoline *tramp = (Ca_ThreadTrampoline *)CA_MALLOC(sizeof(Ca_ThreadTrampoline));
    if (!tramp) { CA_FREE(t); return NULL; }
    tramp->fn = fn;
    tramp->user_data = user_data;
    t->handle = (HANDLE)_beginthreadex(NULL, 0, win32_thread_proc, tramp, 0, NULL);
    if (!t->handle) { CA_FREE(tramp); CA_FREE(t); return NULL; }
    return t;
}

/*
 * Block until the thread finishes, then free the Ca_Thread handle.
 *
 * thread  Thread to join; no-op if NULL.
 */
void ca_thread_join(Ca_Thread *thread)
{
    if (!thread) return;
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    CA_FREE(thread);
}

/*
 * Detach the thread so it cleans up automatically on completion.
 *
 * Closes the OS handle and frees the Ca_Thread struct without waiting.
 *
 * thread  Thread to detach; no-op if NULL.
 */
void ca_thread_detach(Ca_Thread *thread)
{
    if (!thread) return;
    CloseHandle(thread->handle);
    CA_FREE(thread);
}

/*
 * Allocate and initialise a new mutex.
 *
 * Returns  Newly allocated Ca_Mutex, or NULL on allocation failure.
 */
Ca_Mutex *ca_mutex_create(void)
{
    Ca_Mutex *m = (Ca_Mutex *)CA_MALLOC(sizeof(Ca_Mutex));
    if (!m) return NULL;
    InitializeCriticalSection(&m->cs);
    return m;
}

/*
 * Destroy and free a mutex.
 *
 * mutex  Mutex to destroy; no-op if NULL.
 */
void ca_mutex_destroy(Ca_Mutex *mutex)
{
    if (!mutex) return;
    DeleteCriticalSection(&mutex->cs);
    CA_FREE(mutex);
}

/*
 * Acquire the mutex, blocking until it becomes available.
 *
 * mutex  Mutex to acquire; no-op if NULL.
 */
void ca_mutex_lock(Ca_Mutex *mutex)
{
    if (!mutex) return;
    EnterCriticalSection(&mutex->cs);
}

/*
 * Release a previously acquired mutex.
 *
 * mutex  Mutex to release; no-op if NULL.
 */
void ca_mutex_unlock(Ca_Mutex *mutex)
{
    if (!mutex) return;
    LeaveCriticalSection(&mutex->cs);
}

/*
 * Try to acquire the mutex without blocking.
 *
 * mutex   Mutex to attempt; returns false if NULL.
 * Returns true if the mutex was acquired; false if already held.
 */
bool ca_mutex_trylock(Ca_Mutex *mutex)
{
    if (!mutex) return false;
    return TryEnterCriticalSection(&mutex->cs) != 0;
}

/*
 * Allocate and initialise a new condition variable.
 *
 * Returns  Newly allocated Ca_CondVar, or NULL on allocation failure.
 */
Ca_CondVar *ca_condvar_create(void)
{
    Ca_CondVar *cv = (Ca_CondVar *)CA_MALLOC(sizeof(Ca_CondVar));
    if (!cv) return NULL;
    InitializeConditionVariable(&cv->cv);
    return cv;
}

/*
 * Free a condition variable.
 *
 * cv  Condition variable to destroy; no-op if NULL.
 */
void ca_condvar_destroy(Ca_CondVar *cv)
{
    if (!cv) return;
    CA_FREE(cv);
}

/*
 * Atomically release the mutex and block until the condition variable is signalled.
 *
 * cv     Condition variable to wait on.
 * mutex  Mutex that must be held by the caller; released during the wait.
 */
void ca_condvar_wait(Ca_CondVar *cv, Ca_Mutex *mutex)
{
    if (!cv || !mutex) return;
    SleepConditionVariableCS(&cv->cv, &mutex->cs, INFINITE);
}

/*
 * Wake one thread waiting on the condition variable.
 *
 * cv  Condition variable to signal; no-op if NULL.
 */
void ca_condvar_signal(Ca_CondVar *cv)
{
    if (!cv) return;
    WakeConditionVariable(&cv->cv);
}

/*
 * Wake all threads waiting on the condition variable.
 *
 * cv  Condition variable to broadcast to; no-op if NULL.
 */
void ca_condvar_broadcast(Ca_CondVar *cv)
{
    if (!cv) return;
    WakeAllConditionVariable(&cv->cv);
}

#else /* POSIX */

/*
 * Create and start a new POSIX thread running fn(user_data).
 *
 * fn         Function to execute on the new thread.
 * user_data  Opaque argument forwarded to fn.
 * Returns    Allocated Ca_Thread handle, or NULL on failure.
 */
Ca_Thread *ca_thread_create(Ca_ThreadFn fn, void *user_data)
{
    Ca_Thread *t = (Ca_Thread *)CA_MALLOC(sizeof(Ca_Thread));
    if (!t) return NULL;
    if (pthread_create(&t->handle, NULL, fn, user_data) != 0) {
        CA_FREE(t);
        return NULL;
    }
    return t;
}

/*
 * Block until the POSIX thread finishes, then free the Ca_Thread handle.
 *
 * thread  Thread to join; no-op if NULL.
 */
void ca_thread_join(Ca_Thread *thread)
{
    if (!thread) return;
    pthread_join(thread->handle, NULL);
    CA_FREE(thread);
}

/*
 * Detach the POSIX thread so it cleans up automatically; frees the Ca_Thread handle.
 *
 * thread  Thread to detach; no-op if NULL.
 */
void ca_thread_detach(Ca_Thread *thread)
{
    if (!thread) return;
    pthread_detach(thread->handle);
    CA_FREE(thread);
}

/*
 * Allocate and initialise a new POSIX mutex.
 *
 * Returns  Newly allocated Ca_Mutex, or NULL on failure.
 */
Ca_Mutex *ca_mutex_create(void)
{
    Ca_Mutex *m = (Ca_Mutex *)CA_MALLOC(sizeof(Ca_Mutex));
    if (!m) return NULL;
    if (pthread_mutex_init(&m->handle, NULL) != 0) {
        CA_FREE(m);
        return NULL;
    }
    return m;
}

/*
 * Destroy and free a POSIX mutex.
 *
 * mutex  Mutex to destroy; no-op if NULL.
 */
void ca_mutex_destroy(Ca_Mutex *mutex)
{
    if (!mutex) return;
    pthread_mutex_destroy(&mutex->handle);
    CA_FREE(mutex);
}

/*
 * Acquire the POSIX mutex, blocking until it becomes available.
 *
 * mutex  Mutex to acquire; no-op if NULL.
 */
void ca_mutex_lock(Ca_Mutex *mutex)
{
    if (!mutex) return;
    pthread_mutex_lock(&mutex->handle);
}

/*
 * Release a previously acquired POSIX mutex.
 *
 * mutex  Mutex to release; no-op if NULL.
 */
void ca_mutex_unlock(Ca_Mutex *mutex)
{
    if (!mutex) return;
    pthread_mutex_unlock(&mutex->handle);
}

/*
 * Try to acquire the mutex without blocking.
 *
 * mutex   Mutex to attempt; returns false if NULL.
 * Returns true if the lock was acquired; false if already held.
 */
bool ca_mutex_trylock(Ca_Mutex *mutex)
{
    if (!mutex) return false;
    return pthread_mutex_trylock(&mutex->handle) == 0;
}

/*
 * Allocate and initialise a new POSIX condition variable.
 *
 * Returns  Newly allocated Ca_CondVar, or NULL on failure.
 */
Ca_CondVar *ca_condvar_create(void)
{
    Ca_CondVar *cv = (Ca_CondVar *)CA_MALLOC(sizeof(Ca_CondVar));
    if (!cv) return NULL;
    if (pthread_cond_init(&cv->handle, NULL) != 0) {
        CA_FREE(cv);
        return NULL;
    }
    return cv;
}

/*
 * Destroy and free a POSIX condition variable.
 *
 * cv  Condition variable to destroy; no-op if NULL.
 */
void ca_condvar_destroy(Ca_CondVar *cv)
{
    if (!cv) return;
    pthread_cond_destroy(&cv->handle);
    CA_FREE(cv);
}

/*
 * Atomically release the mutex and block until the condition variable is signalled.
 *
 * cv     Condition variable to wait on.
 * mutex  Mutex that must be held by the caller; released during the wait.
 */
void ca_condvar_wait(Ca_CondVar *cv, Ca_Mutex *mutex)
{
    if (!cv || !mutex) return;
    pthread_cond_wait(&cv->handle, &mutex->handle);
}

/*
 * Wake one thread waiting on the POSIX condition variable.
 *
 * cv  Condition variable to signal; no-op if NULL.
 */
void ca_condvar_signal(Ca_CondVar *cv)
{
    if (!cv) return;
    pthread_cond_signal(&cv->handle);
}

/*
 * Wake all threads waiting on the POSIX condition variable.
 *
 * cv  Condition variable to broadcast to; no-op if NULL.
 */
void ca_condvar_broadcast(Ca_CondVar *cv)
{
    if (!cv) return;
    pthread_cond_broadcast(&cv->handle);
}

#endif
