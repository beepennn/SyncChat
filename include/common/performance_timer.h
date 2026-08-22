#ifndef PERFORMANCE_TIMER_H
#define PERFORMANCE_TIMER_H

#include <stdint.h>
#include <time.h>


typedef struct
{
    struct timespec start_time;
    int running;

} performance_timer_t;


/*
 * Start a monotonic performance timer.
 *
 * CLOCK_MONOTONIC is used because elapsed-time
 * measurements must not be affected by wall-clock
 * adjustments.
 *
 * Return:
 *   0  success
 *  -1  failure
 */
int performance_timer_start(
    performance_timer_t *timer
);


/*
 * Stop a running timer and return elapsed seconds.
 *
 * Return:
 *   0  success
 *  -1  failure
 */
int performance_timer_stop(
    performance_timer_t *timer,
    double *elapsed_seconds
);


/*
 * Convert a byte count and elapsed duration into
 * mebibytes per second.
 *
 * Returns 0.0 when elapsed_seconds is not positive.
 */
double performance_timer_mib_per_second(
    uint64_t bytes,
    double elapsed_seconds
);


#endif
