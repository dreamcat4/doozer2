#pragma once

struct htsbuf_queue;

#define SPAWN_PERMANENT_FAIL -1
#define SPAWN_TEMPORARY_FAIL -2

int spawn(int (*exec_cb)(void *opaque),
          int (*line_cb)(void *opaque, const char *line,
                         char *errbuf, size_t errlen),
          void *opaque,
          struct htsbuf_queue *output, int timeout,
          int flags, char *errbuf, size_t errlen);

