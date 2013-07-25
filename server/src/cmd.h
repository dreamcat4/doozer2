#pragma once

int cmd_exec(const char *line,
             const char *user,
             void (*fb)(void *opaque, const char *fmt, ...),
             void *opaque);

