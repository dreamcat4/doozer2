#include <sys/param.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <fnmatch.h>
#include <dirent.h>
#include <errno.h>

#include <curl/curl.h>

#include "libsvc/misc.h"
#include "libsvc/htsmsg_json.h"
#include "libsvc/trace.h"
#include "libsvc/threading.h"
#include "libsvc/irc.h"
#include "libsvc/cfg.h"
#include "libsvc/talloc.h"

#include "doozer.h"
#include "project.h"
#include "git.h"
#include "buildmaster.h"
#include "releasemaker.h"


LIST_HEAD(project_list, project);
LIST_HEAD(pconf_list, pconf);

static struct project_list projects;

static pthread_mutex_t projects_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t projects_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t project_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct pconf {
  LIST_ENTRY(pconf) pc_link;
  char *pc_id;
  int pc_mark;
  htsmsg_t *pc_msg;
  time_t pc_mtime;  // mtime of last read conf
} pconf_t;

static struct pconf_list pconfs;


static void project_notify_repo_update(project_t *p);


/**
 *
 */
void
plog(project_t *p, const char *ctx, const char *fmt, ...)
{
  char buf_color[2048];

  project_cfg(pc, p->p_id);
  if(pc == NULL)
    return;

  int did_syslog = 0;

  va_list ap;

  va_start(ap, fmt);
  int len = snprintf(buf_color, sizeof(buf_color), "%s: ", p->p_id);
  vsnprintf(buf_color + len, sizeof(buf_color) - len, fmt, ap);
  va_end(ap);

  char *buf_nocolor = mystrdupa(buf_color);
  decolorize(buf_nocolor);

  for(int i = 0; ; i++) {

    const char *target  =
      cfg_get_str(pc, CFG("log", CFG_INDEX(i), "target"), NULL);
    if(target == NULL)
      break;

    int match = 0;

    for(int j = 0; ; j++) {
      const char *context =
        cfg_get_str(pc, CFG("log", CFG_INDEX(i), "context", CFG_INDEX(j)), NULL);

      if(context == NULL)
        break;

      int inverse = 0;
      if(*context == '!') {
	inverse = 1;
	context++;
      }

      if(!fnmatch(context, ctx, 0)) {
	if(inverse)
	  break;
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

    int pp = cfg_get_int(pc, CFG("log", CFG_INDEX(i), "prefixProject"), 1);

    if(!strcmp(proto, "syslog") && !did_syslog) {
      trace(LOG_INFO, "%s", pp ? buf_nocolor : buf_nocolor + len);
      did_syslog = 1;
    } else if(!strcmp(proto, "irc")) {
      path[0] = '#';  // Replace '/' with '#'
      irc_msg_channel(hostname, path, pp ? buf_color : buf_color + len);
    }
  }

  if(!did_syslog && isatty(2))
    fprintf(stderr, "%s\n", buf_nocolor);
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
    trace(LOG_INFO, "%s: Project initialized", p->p_id);
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

    talloc_cleanup();

    int pendings = p->p_pending_jobs;
    p->p_pending_jobs = 0;
    pthread_mutex_unlock(&projects_mutex);

    if(pendings & PROJECT_JOB_UPDATE_REPO)
      git_repo_sync(p);

    if(pendings & PROJECT_JOB_NOTIFY_REPO_UPDATE)
      project_notify_repo_update(p);

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

  time_t now;
  int next_check;

  while(1) {

    talloc_cleanup();

    next_check = INT32_MAX;
    now = time(NULL);

    // If there are pending jobs for a project and no worker thread
    // we need to create a worker thread for it
    project_t *p;
    LIST_FOREACH(p, &projects, p_link) {

      if(p->p_next_refresh) {

        if(now >= p->p_next_refresh) {
          p->p_pending_jobs |= PROJECT_JOB_UPDATE_REPO;
          p->p_next_refresh = now + p->p_refresh_interval;
        } else if(p->p_next_refresh) {
          next_check = MIN(next_check, p->p_next_refresh);
        }
      }
      if(p->p_pending_jobs && !p->p_thread)
        break;
    }

    if(p == NULL) {

      if(next_check != INT32_MAX) {
        struct timespec ts;
        ts.tv_sec = next_check;
        ts.tv_nsec = 0;
        pthread_cond_timedwait(&projects_cond, &projects_mutex, &ts);
      } else {
        pthread_cond_wait(&projects_cond, &projects_mutex);
      }
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




static size_t
dump_output(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  return size * nmemb;
}

/**
 *
 */
static void
project_notify_repo_update(project_t *p)
{
  project_cfg(pc, p->p_id);
  if(pc == NULL)
    return;

  for(int i = 0; ; i++) {
    const char *url =
      cfg_get_str(pc, CFG("repoUpdateNotifications", CFG_INDEX(i)), NULL);
    if(url == NULL)
      break;

    plog(p, "notify", "Invoking %s", url);

    CURL *curl = curl_easy_init();
    if(curl != NULL) {
      curl_easy_setopt(curl, CURLOPT_URL, url);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &dump_output);
      curl_easy_perform(curl);
      curl_easy_cleanup(curl);
    }
  }
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


/**
 *
 */
static void
project_load_conf(char *fname, const char *path, const char *org)
{
  char buf[PATH_MAX];

  if(*fname == '#' || *fname == '.')
    return;

  char *postfix = strrchr(fname, '.');
  if(postfix == NULL || strcmp(postfix, ".json"))
    return;

  *postfix = 0;

  snprintf(buf, sizeof(buf), "%s/%s.json", path, fname);

  char id[512];
  snprintf(id, sizeof(id), "%s/%s", org, fname);

  pconf_t *pc;

  LIST_FOREACH(pc, &pconfs, pc_link)
    if(!strcmp(pc->pc_id, id))
      break;

  int err;
  time_t mtime;
  char *json = readfile(buf, &err, &mtime);
  if(json == NULL) {
    trace(LOG_ERR, "Unable to read file %s -- %s", buf, strerror(err));
    trace(LOG_ERR, "Config for project '%s' not updated", id);
    if(pc != NULL)
      pc->pc_mark = 0;
    return;
  }

  char errbuf[256];
  htsmsg_t *m = htsmsg_json_deserialize(json, errbuf, sizeof(errbuf));
  free(json);
  if(m == NULL) {
    trace(LOG_ERR, "Unable to parse file %s -- %s", buf, errbuf);
    trace(LOG_ERR, "Config for project '%s' not updated", id);
    if(pc != NULL)
      pc->pc_mark = 0;
    return;
  }

  if(pc == NULL) {
    pc = calloc(1, sizeof(pconf_t));
    LIST_INSERT_HEAD(&pconfs, pc, pc_link);
    pc->pc_id = strdup(id);
  } else {
    pc->pc_mark = 0;

    if(pc->pc_mtime == mtime)
      return;

    htsmsg_release(pc->pc_msg);
  }

  pc->pc_msg = m;
  htsmsg_retain(m);

  project_t *p = project_init(id, pc->pc_mtime != mtime);

  pc->pc_mtime = mtime;

  p->p_refresh_interval =
    cfg_get_int(pc->pc_msg, CFG("gitrepo", "refreshInterval"), 0);

  if(p->p_refresh_interval)
    p->p_next_refresh = time(NULL) + p->p_refresh_interval;
  else
    p->p_next_refresh = 0;

  trace(LOG_INFO, "%s: Config loaded", id);
}


/**
 *
 */
static void
project_load_dir(char *fname, const char *path)
{
  if(*fname == '#' || *fname == '.')
    return;

  char buf[PATH_MAX];
  snprintf(buf, sizeof(buf), "%s/%s", path, fname);

  struct dirent **namelist;
  int n = scandir(buf, &namelist, NULL, NULL);
  if(n < 0) {
    trace(LOG_ERR, "Unable to scan project config dir %s -- %s",
          path, strerror(errno));
    return;
  }

  while(n--) {
    if(namelist[n]->d_type == DT_REG)
      project_load_conf(namelist[n]->d_name, buf, fname);
    free(namelist[n]);
  }
  free(namelist);
}


/**
 *
 */
void
projects_reload(void)
{
  pconf_t *pc, *next;
  cfg_root(root);

  const char *path = cfg_get_str(root, CFG("projectConfigDir"), "projects");

  if(path == NULL)
    return;

  struct dirent **namelist;
  int n = scandir(path, &namelist, NULL, NULL);
  if(n < 0) {
    trace(LOG_ERR, "Unable to scan project config dir %s -- %s",
          path, strerror(errno));
    return;
  }

  scoped_lock(&project_cfg_mutex);

  LIST_FOREACH(pc, &pconfs, pc_link)
    pc->pc_mark = 1;

  while(n--) {
    if(namelist[n]->d_type == DT_DIR)
      project_load_dir(namelist[n]->d_name, path);
    free(namelist[n]);
  }
  free(namelist);


  for(pc = LIST_FIRST(&pconfs); pc != NULL; pc = next) {
    next = LIST_NEXT(pc, pc_link);
    if(!pc->pc_mark)
      continue;

    trace(LOG_INFO, "%s: Config unloaded", pc->pc_id);

    LIST_REMOVE(pc, pc_link);
    free(pc->pc_id);
    htsmsg_release(pc->pc_msg);
    free(pc);
  }
}


/**
 *
 */
cfg_t *
project_get_cfg(const char *id)
{
  pconf_t *pc;
  scoped_lock(&project_cfg_mutex);

  LIST_FOREACH(pc, &pconfs, pc_link) {
    if(!strcmp(pc->pc_id, id)) {
      htsmsg_t *c = pc->pc_msg;
      htsmsg_retain(c);
      return c;
    }
  }
  return NULL;
}

/**
 *
 */
const char *
project_get_artifact_path(const char *id)
{
  const char *s;
  static __thread char path[PATH_MAX];

  project_cfg(pc, id);
  if(pc == NULL)
    return NULL;

  s = cfg_get_str(pc, CFG("artifactPath"), NULL);
  if(s != NULL) {
    snprintf(path, sizeof(path), "%s", s);
    return path;
  }

  cfg_root(root);
  s = cfg_get_str(root, CFG("artifactPath"), NULL);
  if(s == NULL)
    s = "/var/tmp/doozer-artifacts";

  snprintf(path, sizeof(path), "%s/%s", s, id);
  return path;
}
