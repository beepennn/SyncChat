#include <errno.h>
#include <stdint.h>
#include <time.h>

#include "common/performance_timer.h"


int performance_timer_start(
    performance_timer_t *timer
)
{
    if (timer == NULL)
    {
        errno = EINVAL;

        return -1;
    }


    if (clock_gettime(
            CLOCK_MONOTONIC,
            &timer->start_time
        ) != 0)
    {
        timer->running = 0;

        return -1;
    }


    timer->running = 1;


    return 0;
}


int performance_timer_stop(
    performance_timer_t *timer,
    double *elapsed_seconds
)
{
    if (timer == NULL ||
        elapsed_seconds == NULL ||
        !timer->running)
    {
        errno = EINVAL;

        return -1;
    }


    struct timespec end_time;


    if (clock_gettime(
            CLOCK_MONOTONIC,
            &end_time
        ) != 0)
    {
        return -1;
    }


    time_t seconds =
        end_time.tv_sec -
        timer->start_time.tv_sec;

    long nanoseconds =
        end_time.tv_nsec -
        timer->start_time.tv_nsec;


    if (nanoseconds < 0)
    {
        seconds--;

        nanoseconds +=
            1000000000L;
    }


    *elapsed_seconds =
        (double)seconds +
        ((double)nanoseconds /
         1000000000.0);


    timer->running = 0;


    return 0;
}


double performance_timer_mib_per_second(
    uint64_t bytes,
    double elapsed_seconds
)
{
    if (elapsed_seconds <= 0.0)
    {
        return 0.0;
    }


    const double bytes_per_mib =
        1024.0 * 1024.0;


    return
        ((double)bytes / bytes_per_mib) /
        elapsed_seconds;
}
