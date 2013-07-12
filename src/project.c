#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <fnmatch.h>
#include <dirent.h>
#include <errno.h>

#include "doozer.h"
#include "project.h"
#include "cfg.h"
#include "git.h"
#include "buildmaster.h"
#include "releasemaker.h"
#include "irc.h"

#include "misc/misc.h"
#include "misc/htsmsg_json.h"

LIST_HEAD(project_list, project);

static struct project_list projects;

static pthread_mutex_t projects_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t projects_cond = PTHREAD_COND_INITIALIZER;


/**
 *
 */
void
plog(project_t *p, const char *ctx, const char *fmt, ...)
{
  char buf[2048];

  cfg_project(pc, p->p_id);
  if(pc == NULL)
    return;

  int did_syslog = 0;

  va_list ap;

  va_start(ap, fmt);
  int len = snprintf(buf, sizeof(buf), "%s: ", p->p_id);
  vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
  va_end(ap);

  for(int i = 0; ; i++) {

    const char *target  =
      cfg_get_str(pc, CFG("log", CFG_INDEX(i), "target"), NULL);
    if(target == NULL)
      break;

    int match = 0;

    for(int j = 0; ; i++) {
      const char *context =
        cfg_get_str(pc, CFG("log", CFG_INDEX(i), "context", CFG_INDEX(j)), NULL);

      if(context == NULL)
        break;

      if(!fnmatch(context, ctx, 0)) {
        match = 1;
        break;
      }
    }

    if(!match)
      continue;

    char proto[32];
    char hostname[128];
    char path[256];

    url_split(proto, sizeof(proto), NULL, 0,
              hostname, sizeof(hostname),
              NULL,
              path, sizeof(path),
              target);

    int prefix_project =
      cfg_get_int(pc, CFG("log", CFG_INDEX(i), "prefixProject"), 1);

    // If we don't want to include project as prefix in message,
    // just skip over 'len' bytes (which is length of prefix)
    const char *msg = prefix_project ? buf : buf + len;

    if(!strcmp(proto, "syslog")) {
      trace(LOG_INFO, "%s", msg);
      did_syslog = 1;
    } else if(!strcmp(proto, "irc")) {
      path[0] = '#';  // Replace '/' with '#'
      irc_msg_channel(hostname, path, msg);
    }
  }

  if(!did_syslog && isatty(2))
    fprintf(stderr, "%s\n", buf);
}


/**
 *
 */
project_t *
project_get(const char *id)
{
  scoped_lock(&projects_mutex);

  project_t *p;

  LIST_FOREACH(p, &projects, p_link) {
    if(!strcmp(p->p_id, id)) {
      LIST_REMOVE(p, p_link);
      LIST_INSERT_HEAD(&projects, p, p_link);
      return p;
    }
  }
  return NULL;
}


/**
 *
 */
project_t *
project_init(const char *id, int forceinit)
{
  project_t *p = project_get(id);
  if(p == NULL) {

    p = calloc(1, sizeof(project_t));
    pthread_mutex_init(&p->p_repo_mutex, NULL);
    p->p_id = strdup(id);
    LIST_INSERT_HEAD(&projects, p, p_link);
    trace(LOG_INFO, "Project %s initialized", p->p_id);
    forceinit = 1;
  }

  if(forceinit) {
    p->p_pending_jobs |=
      PROJECT_JOB_UPDATE_REPO |
      PROJECT_JOB_CHECK_FOR_BUILDS |
      PROJECT_JOB_GENERATE_RELEASES;

    pthread_cond_signal(&projects_cond);
  }
  return p;
}


/**
 *
 */
static void *
project_worker(void *aux)
{
  project_t *p = aux;

  pthread_mutex_lock(&projects_mutex);
  while(p->p_pending_jobs) {
    int pendings = p->p_pending_jobs;
    p->p_pending_jobs = 0;
    pthread_mutex_unlock(&projects_mutex);

    if(pendings & PROJECT_JOB_UPDATE_REPO) {
      if(!git_repo_sync(p)) {
        pendings |= PROJECT_JOB_CHECK_FOR_BUILDS;
      }
    }

    if(pendings & PROJECT_JOB_CHECK_FOR_BUILDS)
      buildmaster_check_for_builds(p);

    if(pendings & PROJECT_JOB_GENERATE_RELEASES)
      releasemaker_update_project(p);

    pthread_mutex_lock(&projects_mutex);
  }

  plog(p, "system", "Stopping worker thread");

  // Terminate thread
  p->p_thread = 0;
  pthread_mutex_unlock(&projects_mutex);
  return NULL;
}


/**
 *
 */
static void *
project_thread(void *aux)
{
  pthread_mutex_lock(&projects_mutex);
  while(1) {

    // If there are pending jobs for a project and no worker thread
    // we need to create a worker thread for it
    project_t *p;
    LIST_FOREACH(p, &projects, p_link) {
      if(p->p_pending_jobs && !p->p_thread)
        break;
    }

    if(p == NULL) {
      pthread_cond_wait(&projects_cond, &projects_mutex);
      continue;
    }

    plog(p, "system", "Starting worker thread");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&p->p_thread, &attr, project_worker, p);
    pthread_attr_destroy(&attr);
  }
  return NULL;
}


/**
 *
 */
void
project_schedule_job(project_t *p, int mask)
{
  pthread_mutex_lock(&projects_mutex);
  p->p_pending_jobs = mask;
  pthread_cond_broadcast(&projects_cond);
  pthread_mutex_unlock(&projects_mutex);
}


/**
 *
 */
void
projects_init(void)
{
  projects_reload();
  pthread_t tid;
  pthread_create(&tid, NULL, project_thread, NULL);
}
