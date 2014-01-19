#include <sys/mman.h>
#include <sys/param.h>
#include <sys/wait.h>
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
#include "libsvc/misc.h"

#include "job.h"
#include "heap.h"
#include "git.h"
#include "artifact.h"
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
static int
intercept_doozer_artifact(job_t *j, const char *a, int gzipped,
                          char *errbuf, size_t errlen)
{
  char *line = mystrdupa(a);
  char *argv[4];
  if(str_tokenize(line, argv, 4, ':') != 4) {
    snprintf(errbuf, errlen, "Invalid doozer-artifact line");
    return JOB_RUN_COMMAND_PERMANENT_FAIL;
  }

  const char *localpath   = argv[0];
  const char *filetype    = argv[1];
  const char *contenttype = argv[2];
  const char *filename    = argv[3];

  if(localpath[0] == '/') {

  } else {
    char newpath[PATH_MAX];
    snprintf(newpath, sizeof(newpath), "%s/%s",
             j->repodir, localpath);
    localpath = newpath;
  }

  char newfilename[PATH_MAX];

  char *file_ending = strrchr(filename, '.');
  if(file_ending != NULL)
    *file_ending++ = 0;

  snprintf(newfilename, sizeof(newfilename),
           "%s-%s%s%s",
           filename, j->version,
           file_ending ? "." : "",
           file_ending ?: "");

  if(artifact_add_file(j, filetype, newfilename, contenttype,
                       localpath, gzipped, errbuf, errlen))
    return JOB_RUN_COMMAND_PERMANENT_FAIL;
  return 0;
}


/**
 *
 */
int
job_run_command(job_t *j,
                const char *path,
                const char *argv[],
                htsbuf_queue_t *output,
                const char *cmd,
                char *errbuf, size_t errlen)
{
  int pipe_stdout[2];
  int pipe_stderr[2];
  const int timeout = 600;
  const int print_to_stdout = isatty(1);

  if(pipe(pipe_stdout) || pipe(pipe_stderr)) {
    snprintf(errbuf, errlen, "Unable to create pipe -- %s",
             strerror(errno));
    return JOB_RUN_COMMAND_TEMPORARY_FAIL;
  }

  pid_t pid = fork();

  if(pid == -1) {
    close(pipe_stdout[0]);
    close(pipe_stdout[1]);
    close(pipe_stderr[0]);
    close(pipe_stderr[1]);
    snprintf(errbuf, errlen, "Unable to fork -- %s",
             strerror(errno));
    return JOB_RUN_COMMAND_TEMPORARY_FAIL;
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

    // First, go back to root

    if(setuid(0)) {
      fprintf(stderr, "Unable to setuid(0) -- %s\n",
              strerror(errno));
      exit(1);
    }

    if(setuid(build_uid)) {
      fprintf(stderr, "Unable to setuid(%d) -- %s\n",
              build_uid, strerror(errno));
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

      const char *a;
      if((a = mystrbegins(line, "doozer-artifact:")) != NULL)
        err = intercept_doozer_artifact(j, a, 0, errbuf, errlen);
      else if((a = mystrbegins(line, "doozer-artifact-gzip:")) != NULL)
        err = intercept_doozer_artifact(j, a, 1, errbuf, errlen);

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
    return JOB_RUN_COMMAND_TEMPORARY_FAIL;
  }

  if(got_timeout) {
    snprintf(errbuf, errlen, "No output detected for %d seconds",
             timeout);
    return JOB_RUN_COMMAND_TEMPORARY_FAIL;
  }

  if(err)
    return err;

  if(WIFEXITED(status)) {
    const int exit_status = WEXITSTATUS(status);
    if(exit_status == 127) {
      snprintf(errbuf, errlen, "Unable to execute command: %s", cmd);
      return JOB_RUN_COMMAND_PERMANENT_FAIL;
    }
    return exit_status;
  } else if (WIFSIGNALED(status)) {
#ifdef WCOREDUMP
    if(WCOREDUMP(status)) {
      snprintf(errbuf, errlen, "Unable to execute command: %s", cmd);
      return JOB_RUN_COMMAND_TEMPORARY_FAIL;
    }
#endif
    snprintf(errbuf, errlen,
             "Terminated by signal %d", WTERMSIG(status));
    return JOB_RUN_COMMAND_TEMPORARY_FAIL;
  }
  snprintf(errbuf, errlen,
           "Exited with status code %d", status);
  return JOB_RUN_COMMAND_TEMPORARY_FAIL;
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
static int
job_mkdir(job_t *j, char path[PATH_MAX], const char *fmt, ...)
{
  va_list ap;
  int l = snprintf(path, PATH_MAX, "%s/", j->projectdir);

  va_start(ap, fmt);
  vsnprintf(path + l, PATH_MAX - l, fmt, ap);
  va_end(ap);
  if(mkdir(path, 0777) && errno != EEXIST) {
    job_report_temp_fail(j, "Unable to create dir %s -- %s",
                         path, strerror(errno));
    return -1;
  }
  return 0;
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

  htsmsg_print(msg);

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


  // Create project heap

  char errbuf[512];
  char heapdir[PATH_MAX];
  int r = projects_heap_mgr->open_heap(projects_heap_mgr,
                                       j.project,
                                       heapdir,
                                       errbuf, sizeof(errbuf), 1);

  if(r) {
    job_report_fail(&j, "%s", errbuf);
    return;
  }

  j.projectdir = heapdir;

  char checkoutdir[PATH_MAX];
  if(job_mkdir(&j, checkoutdir, "checkout"))
    return;

  char repodir[PATH_MAX];
  if(job_mkdir(&j, repodir, "checkout/%s", j.project))
    return;
  j.repodir = repodir;

  char workdir[PATH_MAX];
  if(job_mkdir(&j, workdir, "workdir"))
    return;
  j.workdir = repodir;


  // Checkout from GIT

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
