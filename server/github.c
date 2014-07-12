#include <string.h>
#include <stdio.h>

#include "libsvc/http.h"
#include "libsvc/htsmsg_json.h"
#include "libsvc/trace.h"
#include "libsvc/cfg.h"
#include "libsvc/urlshorten.h"

#include "github.h"
#include "project.h"


/**
 *
 */
static int
count_list(htsmsg_t *m, const char *fname)
{
  htsmsg_t *l = htsmsg_get_list(m, fname);
  if(l == NULL)
    return 0;
  htsmsg_field_t *f;
  int cnt = 0;
  HTSMSG_FOREACH(f, l)
    cnt++;
  return cnt;
}


/**
 *
 */
static int
http_github(http_connection_t *hc, const char *remain, void *opaque)
{
  const char *pid = http_arg_get(&hc->hc_req_args, "project");
  const char *key = http_arg_get(&hc->hc_req_args, "key");


  if(pid == NULL) {
    trace(LOG_WARNING, "github: Missing 'project' in request");
    return 400;
  }

  if(key == NULL) {
    trace(LOG_WARNING, "github: Missing 'key' in request");
    return 400;
  }

  project_cfg(pc, pid);
  if(pc == NULL) {
    trace(LOG_DEBUG, "github: Project '%s' not configured", pid);
    return 404;
  }

  const char *mykey = cfg_get_str(pc, CFG("github", "key"), "");

  if(strcmp(mykey, key)) {
    trace(LOG_WARNING, "github: Invalid key received (%s) for project %s",
          key, pid);
    return 403;
  }

  project_t *p = project_get(pid);

  const char *json = http_arg_get(&hc->hc_req_args, "payload");
  if(json == NULL) {
    plog(p, "github", "github: Missing payload in request");
    return 400;
  }

  char errbuf[256];
  htsmsg_t *msg = htsmsg_json_deserialize(json, errbuf, sizeof(errbuf));
  if(msg == NULL) {
    plog(p, "github", "github: Malformed JSON in github request -- %s",
         errbuf);
    return 400;
  }

  const char *ref = htsmsg_get_str(msg, "ref");
  if(ref != NULL && !strncmp(ref, "refs/heads/", strlen("refs/heads/")))
    ref += strlen("refs/heads/");

  htsmsg_t *list = htsmsg_get_list(msg, "commits");
  if(ref != NULL && list != NULL) {
    htsmsg_field_t *f;
    HTSMSG_FOREACH(f, list) {
      htsmsg_t *c = htsmsg_get_map_by_field(f);
      if(c == NULL)
        continue;

      const char *url = htsmsg_get_str(c, "url");
      const char *msg = htsmsg_get_str(c, "message");
      htsmsg_t *a = htsmsg_get_map(c, "author");
      const char *author = a ? htsmsg_get_str(a, "name") : NULL;

      int added    = count_list(c, "added");
      int removed  = count_list(c, "removed");
      int modified = count_list(c, "modified");

      int len;
      char buf[512];
      char ctx[128];

      url = url ? urlshorten(url) : NULL;

      snprintf(ctx, sizeof(ctx), "changes/%s", ref);

      len = snprintf(buf, sizeof(buf),
                     "Commit in '"COLOR_BLUE"%s"COLOR_OFF"' by "COLOR_PURPLE"%s"COLOR_OFF" [",
                     ref, author ?: "???");

      if(added)
        len += snprintf(buf + len, sizeof(buf) - len,
                        COLOR_GREEN "%d file%s added",
                        added, added == 1 ? "" : "s");

      if(modified)
        len += snprintf(buf + len, sizeof(buf) - len,
                        COLOR_YELLOW "%s%d file%s modified",
                        added ? ", "  : "",
                        modified, modified == 1 ? "" : "s");

      if(removed)
        len += snprintf(buf + len, sizeof(buf) - len,
                        COLOR_RED "%s%d file%s removed",
                        added || modified ? ", "  : "",
                        removed, removed == 1 ? "" : "s");

      snprintf(buf + len, sizeof(buf) - len, COLOR_OFF"]%s%s",
               url ? " " : "", url ?: "");

      plog(p, ctx, "%s", buf);
      plog(p, ctx, "%s", msg);
    }
    project_schedule_job(p, PROJECT_JOB_UPDATE_REPO);
  }
  htsmsg_destroy(msg);
  return 200;
}


/**
 *
 */
void
github_init(void)
{
  http_path_add("/github",   NULL, http_github);
}
