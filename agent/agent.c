#include "agent.h"

#include "libsvc/cfg.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdarg.h>


typedef struct buildmaster {
  const char *url;
  const char *agentid;
  const char *secret;
} buildmaster_t;


/**
 *
 */
static char *
call_buildmaster(const buildmaster_t *bm, const char *path, ...)
{
  va_list ap;
  char *out = NULL;
  size_t outlen;
  char url[1024];
  int l = snprintf(url, sizeof(url), "%s/buildmaster/", bm->url);

  va_start(ap, path);
  vsnprintf(url + l, sizeof(url) - l, path, ap);
  va_end(ap);

  CURL *curl = curl_easy_init();

  FILE *f = open_memstream(&out, &outlen);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERNAME, bm->agentid);
  curl_easy_setopt(curl, CURLOPT_PASSWORD, bm->secret);

  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  CURLcode result = curl_easy_perform(curl);
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
static void
agent_run(void)
{
  buildmaster_t bm;

  cfg_root(root);

  bm.url     = cfg_get_str(root, CFG("buildmaster", "url"), NULL);
  bm.agentid = cfg_get_str(root, CFG("buildmaster", "agent"), NULL);
  bm.secret  = cfg_get_str(root, CFG("buildmaster", "secret"), NULL);

  printf("%s %s %s\n", bm.url, bm.agentid, bm.secret);

  if(bm.url == NULL || bm.agentid == NULL || bm.secret == NULL)
    return;

  char *msg = call_buildmaster(&bm, "hello");
  printf("msg = %s\n", msg);
  free(msg);
}

static void *
agent_main(void *aux)
{
  while(1) {
    agent_run();
    sleep(1);
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
