#ifndef LOGGER_CLIENT_H
#define LOGGER_CLIENT_H

#include "logger/logger_protocol.h"

/*
 * Send one structured log entry to the custom
 * logger daemon.
 *
 * This function is intentionally best-effort.
 * Failure to reach the logger daemon must not
 * terminate or disrupt the chat server.
 *
 * Return:
 *   0  log packet sent
 *  -1  logging failed
 */
int logger_client_log(
    log_level_t level,
    const char *source,
    const char *format,
    ...
);

#endif
