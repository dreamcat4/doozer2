#include "agent.h"

#include <sys/stat.h>
#include <sys/param.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <grp.h>

#include "libsvc/cfg.h"
#include "libsvc/trace.h"
#include "libsvc/htsmsg_json.h"
#include "libsvc/misc.h"
#include "libsvc/memstream.h"
#include "libsvc/talloc.h"
#include "libsvc/curlhelpers.h"

#include "agent.h"
#include "job.h"


/**
 *
 */
static char *
call_buildmaster0(buildmaster_t *bm, const char *accepthdr,
                  const char *path, va_list ap)
{
  char *out = NULL;
  size_t outlen;
  char url[2048];
  int l = snprintf(url, sizeof(url), "%s/buildmaster/", bm->url);

  vsnprintf(url + l, sizeof(url) - l, path, ap);

  CURL *curl = curl_easy_init();

  FILE *f = open_buffer(&out, &outlen);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERNAME, bm->agentid);
  curl_easy_setopt(curl, CURLOPT_PASSWORD, bm->secret);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, &libsvc_curl_sock_fn);
  curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, NULL);

  struct curl_slist *slist = NULL;
  if(accepthdr) {
    char b[128];
    snprintf(b, sizeof(b), "Accept: %s", accepthdr);
    slist = curl_slist_append(slist, b);
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

  CURLcode result = curl_easy_perform(curl);

  curl_slist_free_all(slist);

  if(result == CURLE_HTTP_RETURNED_ERROR) {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    snprintf(bm->rpc_errbuf, sizeof(bm->rpc_errbuf), "HTTP Error %lu", http_code);
    bm->last_rpc_error = bm->rpc_errbuf;
  } else {
    bm->last_rpc_error = curl_easy_strerror(result);
  }

  fwrite("", 1, 1, f);
  fclose(f);
  curl_easy_cleanup(curl);
  if(result) {
    free(out);
    return NULL;
  }
  return out;
}

/**
 *
 */
char *
call_buildmaster(buildmaster_t *bm, const char *path, ...)
{
  va_list ap;
  va_start(ap, path);
  char *r = call_buildmaster0(bm, NULL, path, ap);
  va_end(ap);
  return r;
}


/**
 *
 */
static htsmsg_t *
call_buildmaster_json(buildmaster_t *bm, const char *path, ...)
{
  va_list ap;
  va_start(ap, path);
  char *r = call_buildmaster0(bm, "application/json", path, ap);
  va_end(ap);

  if(r == NULL)
    return NULL;
  htsmsg_t *m = htsmsg_json_deserialize(r, bm->rpc_errbuf, sizeof(bm->rpc_errbuf));
  free(r);
  if(m == NULL)
    bm->last_rpc_error = bm->rpc_errbuf;
  return m;
}




/**
 *
 */
static int
getjob(buildmaster_t *bm)
{
  char buf[4096];
  int off = 0;
  htsmsg_t *msg = NULL;
  cfg_root(root);

  cfg_t *targets_msg = cfg_get_list(root, "targets");
  if(targets_msg == NULL) {
    trace(LOG_ERR, "No targets configured");
    return -1;
  }

  htsmsg_field_t *tfield;

  HTSMSG_FOREACH(tfield, targets_msg) {
    htsmsg_t *target = htsmsg_get_map_by_field(tfield);
    const char *t_name = cfg_get_str(target, CFG("name"), NULL);
    if(t_name == NULL)
      continue;

    off += snprintf(buf + off, sizeof(buf) - off, "%s%s",
                    off ? "," : "", t_name);
  }

  if(off == 0) {
    trace(LOG_ERR, "No targets configured");
    return -1;
  }

  msg = call_buildmaster_json(bm, "getjob?targets=%s", buf);
  if(msg == NULL) {
    trace(LOG_ERR, "Unable to getjob -- %s", bm->last_rpc_error);
    return -1;
  }
  job_process(bm, msg);
  htsmsg_destroy(msg);
  return 0;
}


/**
 *
 */
static int
agent_run(void)
{
  buildmaster_t bm = {};

  cfg_root(root);

  bm.url     = cfg_get_str(root, CFG("buildmaster", "url"), NULL);
  bm.agentid = cfg_get_str(root, CFG("buildmaster", "agentid"), NULL);
  bm.secret  = cfg_get_str(root, CFG("buildmaster", "secret"), NULL);

  const char *projects_dir = cfg_get_str(root, CFG("projectsdir"), NULL);
  //  const char *buildenv_dir = cfg_get_str(root, CFG("buildenvdir"), NULL);

  if(bm.url == NULL) {
    trace(LOG_ERR, "Missing configuration buildmaster.url");
    return -1;
  }

  if(bm.agentid == NULL) {
    trace(LOG_ERR, "Missing configuration buildmaster.agentid");
    return -1;
  }

  if(bm.secret == NULL) {
    trace(LOG_ERR, "Missing configuration buildmaster.secret");
    return -1;
  }

  if(projects_dir == NULL) {
    trace(LOG_ERR, "Missing configuration projectsdir");
    return -1;
  }

  if(mkdir(projects_dir, 0777) && errno != EEXIST) {
    trace(LOG_ERR, "Unable to create %s -- %s", projects_dir, strerror(errno));
    return -1;
  }

  char *msg = call_buildmaster(&bm, "hello");
  if(msg == NULL) {
    trace(LOG_ERR, "Not welcomed by buildmaster -- %s",
          bm.last_rpc_error);
    return -1;
  }
  free(msg);
  trace(LOG_DEBUG, "Welcomed by buildmaster");

  while(!getjob(&bm)) {
    talloc_cleanup();
  }

  return 0;
}


/**
 *
 */
static void *
agent_main(void *aux)
{
  int sleeper = 1;
  while(1) {

    talloc_cleanup();

    if(agent_run()) {
      sleeper = MIN(120, sleeper * 2);
      trace(LOG_ERR, "An error occured, sleeping for %d seconds", sleeper);
      sleep(sleeper);
    } else {
      sleeper = 1;
    }
  }
  return NULL;
}


/**
 *
 */
void
agent_init(void)
{

  pthread_t tid;
  pthread_create(&tid, NULL, agent_main, NULL);

}
