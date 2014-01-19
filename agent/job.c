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
typedef struct job_run_command_aux {
  job_t *job;
  const char **argv;
} job_run_command_aux_t;


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
    return SPAWN_PERMANENT_FAIL;
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
    return SPAWN_PERMANENT_FAIL;
  return 0;
}


/**
 *
 */
static int
job_run_command_line_intercept(void *opaque,
                               const char *line,
                               char *errbuf,
                               size_t errlen)
{
  job_run_command_aux_t *aux = opaque;
  job_t *j = aux->job;
  const char *a;
  int err = 0;

  if((a = mystrbegins(line, "doozer-artifact:")) != NULL)
    err = intercept_doozer_artifact(j, a, 0, errbuf, errlen);
  else if((a = mystrbegins(line, "doozer-artifact-gzip:")) != NULL)
    err = intercept_doozer_artifact(j, a, 1, errbuf, errlen);
  return err;
}

/**
 *
 */
static int
job_run_command_spawn(void *opaque)
{
  job_run_command_aux_t *aux = opaque;
  job_t *j = aux->job;

  if(chdir(j->repodir)) {
    fprintf(stderr, "Unable to chdir to %s -- %s\n",
            j->repodir, strerror(errno));
    return 1;
  }

  // First, go back to root

  if(setuid(0)) {
    fprintf(stderr, "Unable to setuid(0) -- %s\n",
            strerror(errno));
    return 1;
  }

  // Then setuid to build_uid

  if(setuid(build_uid)) {
    fprintf(stderr, "Unable to setuid(%d) -- %s\n",
            build_uid, strerror(errno));
    return 1;
  }

  execv(aux->argv[0], (void *)aux->argv);
  fprintf(stderr, "Unable to execute %s -- %s\n",
          aux->argv[0], strerror(errno));
  return 127;
}


/**
 *
 */
int
job_run_command(job_t *j, const char **argv,
                struct htsbuf_queue *output,
                int flags, char *errbuf, size_t errlen)
{
  job_run_command_aux_t aux;
  aux.job = j;
  aux.argv = argv;
  return spawn(job_run_command_spawn,
               job_run_command_line_intercept,
               &aux, output, 600, flags,
               errbuf, errlen);
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
