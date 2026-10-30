#include "native.h"

#include <stdlib.h>
#include <string.h>

#if BSPP_ENABLE_PARALLEL
#include <pthread.h>
#include <unistd.h>

enum { MAX_WORKERS = 8, MAX_WINDOW = 8 };
struct WorkerPool {
    pthread_mutex_t mutex;
    pthread_cond_t work, done;
    pthread_t threads[MAX_WORKERS];
    size_t count, outstanding;
    Work *head, *tail;
    bool stopping;
};

static void thread_check(int error, const char *operation) {
    if (error)
        fail("worker pool %s: %s", operation, strerror(error));
}

static void *worker_main(void *context) {
    WorkerPool *pool = context;
    thread_check(pthread_mutex_lock(&pool->mutex), "lock");
    for (;;) {
        while (!pool->head && !pool->stopping)
            thread_check(pthread_cond_wait(&pool->work, &pool->mutex), "wait for work");
        if (pool->stopping)
            break;
        Work *work = pool->head;
        pool->head = work->next;
        if (!pool->head)
            pool->tail = NULL;
        thread_check(pthread_mutex_unlock(&pool->mutex), "unlock");
        work->run(work->context);
        thread_check(pthread_mutex_lock(&pool->mutex), "lock");
        work->done = true;
        thread_check(pthread_cond_broadcast(&pool->done), "signal completion");
    }
    thread_check(pthread_mutex_unlock(&pool->mutex), "unlock");
    return NULL;
}
#endif

WorkerPool *worker_pool_create(bool parallel) {
#if BSPP_ENABLE_PARALLEL
    if (parallel) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpus < 1)
            fail("cannot determine available CPU count");
        WorkerPool *pool = allocate(1, sizeof *pool);
        pool->count = cpus > MAX_WORKERS ? MAX_WORKERS : (size_t)cpus;
        thread_check(pthread_mutex_init(&pool->mutex, NULL), "initialize mutex");
        thread_check(pthread_cond_init(&pool->work, NULL), "initialize work condition");
        thread_check(pthread_cond_init(&pool->done, NULL), "initialize completion condition");
        for (size_t i = 0; i < pool->count; ++i)
            thread_check(pthread_create(&pool->threads[i], NULL, worker_main, pool), "start");
        return pool;
    }
#else
    (void)parallel;
#endif
    return NULL;
}

size_t worker_pool_window(const WorkerPool *pool) {
#if BSPP_ENABLE_PARALLEL
    if (pool) {
        size_t window = pool->count * 2;
        return window < MAX_WINDOW ? window : MAX_WINDOW;
    }
#else
    (void)pool;
#endif
    return 1;
}

void worker_submit(WorkerPool *pool, Work *work, void (*run)(void *), void *context) {
#if BSPP_ENABLE_PARALLEL
    if (pool) {
        thread_check(pthread_mutex_lock(&pool->mutex), "lock");
        if (work->submitted || pool->stopping)
            fail("invalid worker task submission");
        *work = (Work){.run = run, .context = context, .submitted = true};
        if (pool->tail)
            pool->tail->next = work;
        else
            pool->head = work;
        pool->tail = work;
        ++pool->outstanding;
        thread_check(pthread_cond_signal(&pool->work), "submit");
        thread_check(pthread_mutex_unlock(&pool->mutex), "unlock");
        return;
    }
#else
    (void)pool;
    (void)work;
#endif
    run(context);
}

void worker_wait(WorkerPool *pool, Work *work) {
#if BSPP_ENABLE_PARALLEL
    if (pool) {
        thread_check(pthread_mutex_lock(&pool->mutex), "lock");
        if (!work->submitted)
            fail("cannot wait for an unsubmitted worker task");
        while (!work->done)
            thread_check(pthread_cond_wait(&pool->done, &pool->mutex), "wait for completion");
        work->submitted = false;
        --pool->outstanding;
        thread_check(pthread_mutex_unlock(&pool->mutex), "unlock");
    }
#else
    (void)pool;
    (void)work;
#endif
}

void worker_pool_destroy(WorkerPool *pool) {
#if BSPP_ENABLE_PARALLEL
    if (!pool)
        return;
    thread_check(pthread_mutex_lock(&pool->mutex), "lock");
    if (pool->outstanding)
        fail("cannot destroy worker pool with outstanding tasks");
    pool->stopping = true;
    thread_check(pthread_cond_broadcast(&pool->work), "stop");
    thread_check(pthread_mutex_unlock(&pool->mutex), "unlock");
    for (size_t i = 0; i < pool->count; ++i)
        thread_check(pthread_join(pool->threads[i], NULL), "join");
    thread_check(pthread_cond_destroy(&pool->work), "destroy work condition");
    thread_check(pthread_cond_destroy(&pool->done), "destroy completion condition");
    thread_check(pthread_mutex_destroy(&pool->mutex), "destroy mutex");
    free(pool);
#else
    (void)pool;
#endif
}
