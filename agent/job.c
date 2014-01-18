#include <sys/mman.h>
#include <sys/param.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>
#include <signal.h>

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

#include "job.h"
#include "git.h"
#include "autobuild.h"
#include "doozerctrl.h"
#include "makefile.h"


/**
 *
 */
static void
job_report_status_va(job_t *j, const char *status0, const char *fmt, va_list ap)
{
  char msg0[512];
  vsnprintf(msg0, sizeof(msg0), fmt, ap);

  char status[64];
  char msg[512];

  url_escape(status, sizeof(status), status0, URL_ESCAPE_PARAM);
  url_escape(msg,    sizeof(msg),    msg0,    URL_ESCAPE_PARAM);

  trace(LOG_INFO, "Project: %s (%s): %s: %s",
        j->project ?: "<Unknown project>",
        j->version ?: "<Unknown version>",
        status0, msg0);

  while(1) {

    char *r = call_buildmaster(j->bm, "report?jobid=%d&jobsecret=%s&status=%s&msg=%s",
                               j->jobid, j->jobsecret, status, msg);

    if(r == NULL) {
      trace(LOG_WARNING, "Unable to report status '%s' -- %s. Retrying", status,
            j->bm->last_rpc_error);
      sleep(3);
      continue;
    }

    free(r);
    return;
  }
}


/**
 *
 */
void
job_report_status(job_t *j, const char *status0, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  job_report_status_va(j, status0, fmt, ap);
  va_end(ap);
}


/**
 *
 */
void
job_report_fail(job_t *j, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  job_report_status_va(j, "failed", fmt, ap);
  va_end(ap);
}


/**
 *
 */
int
job_run_command(job_t *j,
                const char *path,
                const char *argv[],
                htsbuf_queue_t *output,
                const char *cmd)
{
  int pipe_stdout[2];
  int pipe_stderr[2];
  const int timeout = 600;
  const int print_to_stdout = isatty(1);

  if(pipe(pipe_stdout) || pipe(pipe_stderr)) {
    job_report_temp_fail(j, "%s: Unable to create pipe -- %s",
                         cmd, strerror(errno));
    return -1;
  }

  pid_t pid = fork();

  if(pid == -1) {
    job_report_temp_fail(j, "%s: Unable to fork -- %s",
                         cmd, strerror(errno));
    return -1;
  }

  if(pid == 0) {
    // Close read ends of pipe
    close(pipe_stdout[0]);
    close(pipe_stderr[0]);

    dup2(pipe_stdout[1], 1);
    dup2(pipe_stderr[1], 2);

    close(pipe_stdout[1]);
    close(pipe_stderr[1]);

    if(chdir(j->repodir)) {
      fprintf(stderr, "Unable to chdir to %s -- %s\n",
              j->repodir, strerror(errno));
      exit(1);
    }

    execv(path, (void *)argv);
    fprintf(stderr, "Unable to execute %s -- %s\n",
            path, strerror(errno));
    exit(127);
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

  while(1) {

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

    while(1) {
      int len = htsbuf_find(q, 0xa);
      if(len == -1)
        break;

      if(q == &stderr_q)
        htsbuf_append(output, (const uint8_t []){0xff, 0xf9}, 2);

      if(print_to_stdout) {
        printf("%s: ", q == &stderr_q ? "\033[31mstderr" : "\033[33mstdout");
        fflush(stdout);
      }

      while(len) {
        int chunk = MIN(len, sizeof(buf));
        htsbuf_read(q, buf, chunk);

        if(print_to_stdout)
          write(1, buf, chunk);

        htsbuf_append(output, buf, chunk);
        len -= chunk;
      }

      if(print_to_stdout)
        printf("\033[0m\n");

      htsbuf_append(output, "\n", 1);

      htsbuf_drop(q, 1); // Drop \n
    }
  }

  if(got_timeout)
    kill(pid, SIGKILL);

  int status;
  if(waitpid(pid, &status, 0) == -1) {
    job_report_temp_fail(j, "%s: Unable to wait for child -- %s",
                         cmd, strerror(errno));
    return -1;
  }

  if(got_timeout)
    return -2;

  if(WIFEXITED(status)) {
    const int exit_status = WEXITSTATUS(status);
    if(exit_status == 127) {
      job_report_fail(j, "%s: Unable to execute command", cmd);
      return -1;
    }
    return exit_status;
  } else if (WIFSIGNALED(status)) {
#ifdef WCOREDUMP
    if(WCOREDUMP(status)) {
      job_report_fail(j, "%s: Core dumped", cmd);
      return -1;
    }
#endif
    job_report_fail(j, "%s: Terminated by signal %d", cmd, WTERMSIG(status));
    return -1;
  }
  job_report_fail(j, "%s: Exited with status code %d", cmd, status);
  return -1;
}


/**
 *
 */
void
job_report_temp_fail(job_t *j, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  job_report_status_va(j, j->can_temp_fail ? "tempfailed" : "failed", fmt, ap);
  va_end(ap);
}




/**
 *
 */
void
job_process(buildmaster_t *bm, htsmsg_t *msg)
{
  job_t j;
  j.bm = bm;
  const char *type = htsmsg_get_str(msg, "type");
  if(type == NULL)
    return;

  if(strcmp(type, "build"))
    return;

  j.jobid = htsmsg_get_u32_or_default(msg, "id", 0);
  if(j.jobid == 0) {
    trace(LOG_ERR, "Job has no jobid");
    return;
  }

  j.jobsecret = htsmsg_get_str(msg, "jobsecret");
  if(j.jobsecret == NULL) {
    trace(LOG_ERR, "Job has no jobsecret");
    return;
  }

  /*
   * From here on we should always report a job status if something fails
   */
  j.can_temp_fail = htsmsg_get_u32_or_default(msg, "can_temp_fail", 0);

  if((j.project = htsmsg_get_str(msg, "project")) == NULL) {
    job_report_temp_fail(&j, "No 'project' field in work");
    return;
  }

  if((j.version = htsmsg_get_str(msg, "version")) == NULL) {
    job_report_temp_fail(&j, "No 'version' field in work");
    return;
  }

  if((j.revision = htsmsg_get_str(msg, "revision")) == NULL) {
    job_report_temp_fail(&j, "No 'revision' field in work");
    return;
  }

  if((j.target = htsmsg_get_str(msg, "target")) == NULL) {
    job_report_temp_fail(&j, "No 'target' field in work");
    return;
  }

  if((j.repourl = htsmsg_get_str(msg, "repo")) == NULL) {
    job_report_temp_fail(&j, "No 'repo' field in work");
    return;
  }

  char repodir[PATH_MAX];
  snprintf(repodir, sizeof(repodir), "%s/repos/%s", bm->workdir, j.project);
  if(mkdir(repodir, 0777) && errno != EEXIST) {
    trace(LOG_ERR, "Unable to create %s -- %s", repodir, strerror(errno));
    job_report_temp_fail(&j, "Unable to create repo directory %s -- %s",
                         repodir, strerror(errno));
    return;
  }

  j.repodir = repodir;
  if(git_checkout_repo(&j))
    return;


  LIST_INIT(&j.artifacts);
  pthread_cond_init(&j.artifact_cond, NULL);

  // Check if we should use Autobuild.sh

  char autobuild[PATH_MAX];
  snprintf(autobuild, sizeof(autobuild), "%s/Autobuild.sh", j.repodir);

  if(!access(autobuild, X_OK)) {
    j.autobuild = autobuild;
    job_report_status(&j, "building", "Building using Autobuild.sh");

    autobuild_process(&j);
    goto done;
  }


  char doozerctrl[PATH_MAX];
  snprintf(doozerctrl, sizeof(doozerctrl), "%s/.doozer.json", j.repodir);
  if(!access(doozerctrl, R_OK)) {
    j.doozerctrl = doozerctrl;
    job_report_status(&j, "building", "Building using .doozer.json");

    doozerctrl_process(&j);
    goto done;
  }

  char makefile[PATH_MAX];
  snprintf(makefile, sizeof(makefile), "%s/Makefile", j.repodir);
  if(!access(makefile, R_OK)) {
    j.makefile = makefile;
    job_report_status(&j, "building", "Building using Makefile");

    makefile_process(&j);
    goto done;
  }

  job_report_fail(&j, "No clue how to build from this repo");
 done:
  pthread_cond_destroy(&j.artifact_cond);
  assert(LIST_FIRST(&j.artifacts) == NULL);

}
