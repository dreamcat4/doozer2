#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/queue.h>
#include <syslog.h>

#define DOOZER_ERROR_OTHER     -1
#define DOOZER_ERROR_PERMANENT -2  // Will not resolve itself until config is reread
#define DOOZER_ERROR_TRANSIENT -3  // Network errors, etc
#define DOOZER_ERROR_NO_DATA   -4
#define DOOZER_ERROR_INVALID_ARGS -5

