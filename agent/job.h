#pragma once

#include <pthread.h>
#include <sys/queue.h>
#include "agent.h"


struct htsmsg;
struct htsbuf_queue;

LIST_HEAD(artifact_list, artifact);


/**
 *
 */
typedef struct job {
  struct buildmaster *bm;
  struct artifact_list artifacts;

  pthread_cond_t artifact_cond;

  int jobid;
  int can_temp_fail;
  const char *jobsecret;
  const char *repodir;

  const char *repourl;
  const char *project;
  const char *version;
  const char *revision;
  const char *target;

  // For autobuild mode

  const char *autobuild;
  int autobuild_version;

  // For doozerctrl mode

  const char *doozerctrl;

  // For raw Makefile mode

  const char *makefile;

} job_t;





void job_report_status(job_t *j, const char *status, const char *fmt, ...)
  __attribute__ ((format (printf, 3, 4)));

void job_report_fail(job_t *j, const char *fmt, ...)
  __attribute__ ((format (printf, 2, 3)));

void job_report_temp_fail(job_t *j, const char *fmt, ...)
  __attribute__ ((format (printf, 2, 3)));

void job_process(buildmaster_t *bm, struct htsmsg *msg);


//#define RUN_COMMAND_ERROR_REPORTED -1

int job_run_command(job_t *j, const char *path, const char *argv[],
                    struct htsbuf_queue *output, const char *cmd);
