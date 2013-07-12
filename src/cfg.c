#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>

#include "misc/htsmsg_json.h"
#include "misc/misc.h"
#include "cfg.h"
#include "project.h"

LIST_HEAD(pconf_list, pconf);

typedef struct pconf {
  LIST_ENTRY(pconf) pc_link;
  char *pc_id;
  int pc_mark;
  htsmsg_t *pc_msg;
} pconf_t;

pthread_mutex_t cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

static cfg_t *cfgroot;
static struct pconf_list pconfs;


/**
 *
 */
cfg_t *
cfg_get_root(void)
{
  pthread_mutex_lock(&cfg_mutex);
  cfg_t *c = cfgroot;
  htsmsg_retain(c);
  pthread_mutex_unlock(&cfg_mutex);
  return c;
}

void
cfg_releasep(cfg_t **p)
{
  if(*p)
    htsmsg_release(*p);
}


/**
 *
 */
int
cfg_load(const char *filename)
{
  int err;
  static char *lastfilename;

  if(filename == NULL) {
    filename = lastfilename;

  } else {

    free(lastfilename);
    lastfilename = strdup(filename);
  }

  if(filename == NULL)
    filename = "config.json";

  trace(LOG_NOTICE, "About to load config form %s", filename);

  char *cfgtxt = readfile(filename, &err);
  if(cfgtxt == NULL) {
    trace(LOG_ERR, "Unable to read file %s -- %s", filename, strerror(err));
    trace(LOG_ERR, "Config not updated");
    return -1;
  }

  char errbuf[256];
  htsmsg_t *m = htsmsg_json_deserialize(cfgtxt, errbuf, sizeof(errbuf));
  free(cfgtxt);
  if(m == NULL) {
    trace(LOG_ERR, "Unable to parse file %s -- %s", filename, errbuf);
    trace(LOG_ERR, "Config not updated");
    return -1;
  }

  pthread_mutex_lock(&cfg_mutex);
  if(cfgroot != NULL)
    htsmsg_release(cfgroot);

  cfgroot = m;
  htsmsg_retain(m);
  pthread_mutex_unlock(&cfg_mutex);
  trace(LOG_NOTICE, "Config updated");
  return 0;
}


/**
 *
 */
static void
project_load_conf(char *fname, const char *path)
{
  char buf[PATH_MAX];

  if(*fname == '#' || *fname == '.')
    return;

  char *postfix = strrchr(fname, '.');
  if(postfix == NULL || strcmp(postfix, ".json"))
    return;

  *postfix = 0;

  snprintf(buf, sizeof(buf), "%s/%s.json", path, fname);

  int err;
  char *json = readfile(buf, &err);
  if(json == NULL) {
    trace(LOG_ERR, "Unable to read file %s -- %s", buf, strerror(err));
    trace(LOG_ERR, "Config for project '%s' not updated", fname);
    return;
  }

  char errbuf[256];
  htsmsg_t *m = htsmsg_json_deserialize(json, errbuf, sizeof(errbuf));
  free(json);
  if(m == NULL) {
    trace(LOG_ERR, "Unable to parse file %s -- %s", buf, errbuf);
    trace(LOG_ERR, "Config for project '%s' not updated", fname);
    return;
  }

  pconf_t *pc;

  LIST_FOREACH(pc, &pconfs, pc_link)
    if(!strcmp(pc->pc_id, fname))
      break;

  if(pc == NULL) {
    pc = malloc(sizeof(pconf_t));
    LIST_INSERT_HEAD(&pconfs, pc, pc_link);
    pc->pc_id = strdup(fname);
    pc->pc_mark = 0;
  } else {
    htsmsg_release(pc->pc_msg);
  }

  pc->pc_msg = m;
  htsmsg_retain(m);

  project_init(fname, 1);
}


/**
 *
 */
void
projects_reload(void)
{
  pconf_t *pc;
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

  scoped_lock(&cfg_mutex);

  LIST_FOREACH(pc, &pconfs, pc_link)
    pc->pc_mark = 1;

  while(n--) {
    project_load_conf(namelist[n]->d_name, path);
    free(namelist[n]);
  }
  free(namelist);
}


/**
 *
 */
cfg_t *
cfg_get_project(const char *id)
{
  pconf_t *pc;
  scoped_lock(&cfg_mutex);

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
static htsmsg_field_t *
field_from_vec(cfg_t *m, const char **vec)
{
  htsmsg_field_t *f = NULL;
  while(*vec) {
    f = htsmsg_field_find(m, vec[0]);
    if(f == NULL)
      return NULL;
    if(vec[1] == NULL)
      return f;
    if(f->hmf_type != HMF_MAP && f->hmf_type != HMF_LIST)
      return NULL;
    m = &f->hmf_msg;
    vec++;
  }
  return NULL;
}


/**
 *
 */
const char *
cfg_get_str(cfg_t *c, const char **vec, const char *def)
{
  htsmsg_field_t *f = field_from_vec(c, vec);
  if(f == NULL)
    return def;

  return htsmsg_field_get_string(f) ?: def;
}


/**
 *
 */
int64_t
cfg_get_s64(cfg_t *c, const char **path, int64_t def)
{
  htsmsg_field_t *f = field_from_vec(c, path);
  if(f == NULL)
    return def;

  switch(f->hmf_type) {
  default:
    return def;
  case HMF_STR:
    return strtoll(f->hmf_str, NULL, 0);
  case HMF_S64:
    return f->hmf_s64;
  }
}


/**
 *
 */
int
cfg_get_int(cfg_t *c, const char **path, int def)
{
  int64_t s64 = cfg_get_s64(c, path, def);

  if(s64 < -0x80000000LL || s64 > 0x7fffffffLL)
    return def;
  return s64;
}


#if 0
/**
 *
 */
cfg_t *
cfg_get_project(cfg_t *c, const char *id)
{
  htsmsg_t *m = htsmsg_get_map(c, "projects");
  m =  m ? htsmsg_get_map(m, id) : NULL;
  if(m == NULL)
    trace(LOG_ERR, "%s: No config for project", id);
  return m;
}
#endif


/**
 *
 */
cfg_t *
cfg_get_map(cfg_t *c, const char *id)
{
  return htsmsg_get_map(c, id);
}


/**
 *
 */
cfg_t *
cfg_get_list(cfg_t *c, const char *id)
{
  return htsmsg_get_list(c, id);
}


/**
 *
 */
int
cfg_list_length(cfg_t *c)
{
  htsmsg_field_t *f;
  int r = 0;
  HTSMSG_FOREACH(f, c)
    r++;
  return r;
}
