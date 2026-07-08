#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{
    struct thread_data* thread_func_args = (struct thread_data *) thread_param;
    int rc;

    if (usleep(thread_func_args->wait_to_obtain_ms * 1000) != 0)
    {
        ERROR_LOG("usleep before obtaining mutex was interrupted");
        return thread_param;
    }

    rc = pthread_mutex_lock(thread_func_args->mutex);
    if (rc != 0)
    {
        ERROR_LOG("pthread_mutex_lock failed with %d", rc);
        return thread_param;
    }

    if (usleep(thread_func_args->wait_to_release_ms * 1000) != 0)
    {
        ERROR_LOG("usleep before releasing mutex was interrupted");
        pthread_mutex_unlock(thread_func_args->mutex);
        return thread_param;
    }

    rc = pthread_mutex_unlock(thread_func_args->mutex);
    if (rc != 0)
    {
        ERROR_LOG("pthread_mutex_unlock failed with %d", rc);
        return thread_param;
    }

    thread_func_args->thread_complete_success = true;

    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    struct thread_data *thread_param;
    int rc;

    if (thread == NULL || mutex == NULL)
    {
        ERROR_LOG("thread and mutex arguments must not be NULL");
        return false;
    }

    thread_param = malloc(sizeof(struct thread_data));
    if (thread_param == NULL)
    {
        ERROR_LOG("could not allocate thread_data");
        return false;
    }

    thread_param->mutex = mutex;
    thread_param->wait_to_obtain_ms = wait_to_obtain_ms;
    thread_param->wait_to_release_ms = wait_to_release_ms;
    thread_param->thread_complete_success = false;

    rc = pthread_create(thread, NULL, threadfunc, thread_param);
    if (rc != 0)
    {
        ERROR_LOG("pthread_create failed with %d", rc);
        free(thread_param);
        return false;
    }

    return true;
}
