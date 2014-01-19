#include <sys/mman.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>

#include "libsvc/htsmsg.h"
#include "libsvc/misc.h"
#include "libsvc/trace.h"
#include "libsvc/htsbuf.h"
#include "libsvc/misc.h"

#include "spawn.h"


/**
 *
 */
int
spawn(int (*exec_cb)(void *opaque),
      int (*line_cb)(void *opaque, const char *line,
                     char *errbuf, size_t errlen),
      void *opaque,
      htsbuf_queue_t *output, int timeout, int flags,
      char *errbuf, size_t errlen)
{
  int pipe_stdout[2];
  int pipe_stderr[2];
  const int print_to_stdout = isatty(1);

  if(pipe(pipe_stdout) || pipe(pipe_stderr)) {
    snprintf(errbuf, errlen, "Unable to create pipe -- %s",
             strerror(errno));
    return SPAWN_TEMPORARY_FAIL;
  }

  pid_t pid = fork();

  if(pid == -1) {
    close(pipe_stdout[0]);
    close(pipe_stdout[1]);
    close(pipe_stderr[0]);
    close(pipe_stderr[1]);
    snprintf(errbuf, errlen, "Unable to fork -- %s",
             strerror(errno));
    return SPAWN_TEMPORARY_FAIL;
  }

  if(pid == 0) {
    // Close read ends of pipe
    close(pipe_stdout[0]);
    close(pipe_stderr[0]);

    // Let stdin read from /dev/null
    int devnull = open("/dev/null", O_RDONLY);
    if(devnull != -1) {
      dup2(devnull, 0);
      close(devnull);
    }

    // Flush output buffered IO
    fflush(stdout);
    fflush(stderr);

    // Switch stdout/stderr to our pipes
    dup2(pipe_stdout[1], 1);
    dup2(pipe_stderr[1], 2);
    close(pipe_stdout[1]);
    close(pipe_stderr[1]);

    int r = exec_cb(opaque);
    exit(r);
  }

  // Close write ends of pipe
  close(pipe_stdout[1]);
  close(pipe_stderr[1]);

  struct pollfd fds[2] = {
    {
      .fd = pipe_stdout[0],
      .events = POLLIN | POLLHUP | POLLERR,
    }, {

      .fd = pipe_stderr[0],
      .events = POLLIN | POLLHUP | POLLERR,
    }
  };

  int got_timeout = 0;

  char buf[10000];

  htsbuf_queue_t stdout_q, stderr_q, *q;

  htsbuf_queue_init(&stdout_q, 0);
  htsbuf_queue_init(&stderr_q, 0);

  int err = 0;

  while(!err) {

    int r = poll(fds, 2, timeout * 1000);
    if(r == 0) {
      got_timeout = 1;
      break;
    }

    if(fds[0].revents & (POLLHUP | POLLERR))
      break;
    if(fds[1].revents & (POLLHUP | POLLERR))
      break;

    if(fds[0].revents & POLLIN) {
      r = read(fds[0].fd, buf, sizeof(buf));
      q = &stdout_q;
    } else if(fds[1].revents & POLLIN) {
      r = read(fds[1].fd, buf, sizeof(buf));
      q = &stderr_q;
    } else {
      printf("POLL WAT\n");
      sleep(1);
      continue;
    }

    if(r == 0 || r == -1)
      break;

    htsbuf_append(q, buf, r);

    while(!err) {
      int len = htsbuf_find(q, 0xa);
      if(len == -1)
        break;

      if(q == &stderr_q)
        htsbuf_append(output, (const uint8_t []){0xef,0xbf,0xb9}, 3);

      char *line;
      if(len < sizeof(buf) - 1) {
        line = buf;
      } else {
        line = malloc(len + 1);
      }

      htsbuf_read(q, line, len);
      htsbuf_drop(q, 1); // Drop \n
      line[len] = 0;

      htsbuf_append(output, line, len);
      htsbuf_append(output, "\n", 1);

      if(print_to_stdout) {
        printf("%s: %s\033[0m\n",
               q == &stderr_q ? "\033[31mstderr" : "\033[33mstdout",
               line);
      }

      if(line_cb != NULL)
        err = line_cb(opaque, line, errbuf, errlen);

      if(line != buf)
        free(line);
    }
  }

  if(got_timeout || err)
    kill(pid, SIGKILL);

  int status;
  if(waitpid(pid, &status, 0) == -1) {
    snprintf(errbuf, errlen, "Unable to wait for child -- %s",
             strerror(errno));
    return SPAWN_TEMPORARY_FAIL;
  }

  if(got_timeout) {
    snprintf(errbuf, errlen, "No output detected for %d seconds",
             timeout);
    return SPAWN_TEMPORARY_FAIL;
  }

  if(err)
    return err;

  if(WIFEXITED(status)) {
    return WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
#ifdef WCOREDUMP
    if(WCOREDUMP(status)) {
      snprintf(errbuf, errlen, "Core dumped");
      return SPAWN_TEMPORARY_FAIL;
    }
#endif
    snprintf(errbuf, errlen,
             "Terminated by signal %d", WTERMSIG(status));
    return SPAWN_TEMPORARY_FAIL;
  }
  snprintf(errbuf, errlen,
           "Exited with status code %d", status);
  return SPAWN_TEMPORARY_FAIL;
}

