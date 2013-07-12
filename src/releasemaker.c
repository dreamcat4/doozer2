#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <fnmatch.h>

#include "misc/misc.h"
#include "misc/htsmsg_json.h"

#include "releasemaker.h"
#include "db.h"
#include "git.h"


static void generate_update_manifest(project_t *p, struct build_queue *builds,
                                     struct target_queue *targets);

/**
 *
 */
static int
buildcmp(const build_t *a, const build_t *b)
{
  int r = strcmp(a->b_target, b->b_target);
  if(r)
    return r;
  return dictcmp(b->b_branch, a->b_branch);
}


/**
 *
 */
int
releasemaker_update_project(project_t *p)
{
  conn_t *c = db_get_conn();
  struct build_queue builds;
  build_t *b;
  struct target_queue targets;
  target_t *t;

  plog(p, "release/check", "Starting relesemaker check");

  if(c == NULL)
    return DOOZER_ERROR_TRANSIENT;

  if(db_stmt_exec(c->get_releases, "s", p->p_id))
    return DOOZER_ERROR_TRANSIENT;

  TAILQ_INIT(&builds);

  while(1) {
    b = alloca(sizeof(build_t));

    int r = db_stream_row(0, c->get_releases,
                          DB_RESULT_INT(b->b_id),
                          DB_RESULT_STRING(b->b_branch),
                          DB_RESULT_STRING(b->b_target),
                          DB_RESULT_STRING(b->b_version),
                          DB_RESULT_STRING(b->b_revision));
    if(r < 0)
      return DOOZER_ERROR_TRANSIENT;
    if(r)
      break;
    TAILQ_INSERT_SORTED(&builds, b, b_global_link, buildcmp);
  }

  TAILQ_FOREACH(b, &builds, b_global_link) {
    if(db_stmt_exec(c->get_artifacts, "i", b->b_id))
      return DOOZER_ERROR_TRANSIENT;

    TAILQ_INIT(&b->b_artifacts);
    while(1) {
      artifact_t *a = alloca(sizeof(artifact_t));
      int r = db_stream_row(0, c->get_artifacts,
                            DB_RESULT_INT(a->a_id),
                            DB_RESULT_STRING(a->a_type),
                            DB_RESULT_STRING(a->a_sha1),
                            DB_RESULT_INT(a->a_size));
      if(r < 0)
        return DOOZER_ERROR_TRANSIENT;
      if(r)
        break;
      TAILQ_INSERT_HEAD(&b->b_artifacts, a, a_link);
    }
  }


  TAILQ_INIT(&targets);

  TAILQ_FOREACH(b, &builds, b_global_link) {
    TAILQ_FOREACH(t, &targets, t_link)
      if(!strcmp(t->t_target, b->b_target))
        break;

    if(t == NULL) {
      t = alloca(sizeof(target_t));
      strcpy(t->t_target, b->b_target);
      TAILQ_INSERT_TAIL(&targets, t, t_link);
      TAILQ_INIT(&t->t_builds);
    }
    TAILQ_INSERT_TAIL(&t->t_builds, b, b_target_link);
  }

#if 0
  printf("Final list\n");
  TAILQ_FOREACH(t, &targets, t_link) {
    printf("  For %s\n", t->t_target);
    TAILQ_FOREACH(b, &t->t_builds, b_target_link) {
      printf("    %s from branch %s\n", b->b_version, b->b_branch);
      artifact_t *a;
      TAILQ_FOREACH(a, &b->b_artifacts, a_link) {
        printf("      #%-5d %-8s %s %d bytes\n",
               a->a_id, a->a_type, a->a_sha1, a->a_size);
      }
    }
  }
#endif
  generate_update_manifest(p, &builds, &targets);
  return 0;
}


/**
 *
 */
static void
generate_update_manifest(project_t *p, struct build_queue *builds,
                         struct target_queue *targets)
{
  build_t *b;
  artifact_t *a;
  target_t *t;
  cfg_root(root);
  cfg_project(pc, p->p_id);
  if(pc == NULL)
    return;

  const char *baseurl = cfg_get_str(root, CFG("artifactPrefix"), NULL);
  if(baseurl == NULL)
    return;

  const char *outpath = cfg_get_str(pc, CFG("updateManifest", "outdir"), NULL);
  if(outpath == NULL)
    return;

  for(int i = 0; ; i++) {
    const char *track  =
      cfg_get_str(pc, CFG("updateManifest", "tracks", CFG_INDEX(i), "name"),
                  NULL);
    const char *branch =
      cfg_get_str(pc, CFG("updateManifest", "tracks", CFG_INDEX(i), "branch"),
                  NULL);
    if(track == NULL || branch == NULL)
      break;

    TAILQ_FOREACH(t, targets, t_link) {

      TAILQ_FOREACH(b, &t->t_builds, b_target_link) {
        if(!fnmatch(branch, b->b_branch, FNM_PATHNAME))
          break;
      }

      char logctx[128];
      snprintf(logctx, sizeof(logctx), "release/manifest/info/%s",
               t->t_target);

      if(b == NULL) {
        plog(p, logctx,
             "Manifest: Target %s: Track '%s' no matching branch for '%s'",
              t->t_target, track, branch);
        continue;
      }
      plog(p, logctx,
           "Manifest: Target %s: Track '%s' matching branch '%s' for '%s'",
           t->t_target, track, b->b_branch, branch);

      htsmsg_t *out = htsmsg_create_map();

      htsmsg_add_str(out, "arch",    b->b_target);
      htsmsg_add_str(out, "version", b->b_version);
      htsmsg_add_str(out, "branch",  b->b_branch);

      htsmsg_t *artifacts = htsmsg_create_list();
      TAILQ_FOREACH(a, &b->b_artifacts, a_link) {
        htsmsg_t *artifact = htsmsg_create_map();
        char url[1024];
        htsmsg_add_str(artifact, "type", a->a_type);
        htsmsg_add_str(artifact, "sha1", a->a_sha1);
        htsmsg_add_u32(artifact, "size", a->a_size);
        snprintf(url, sizeof(url), "%s/file/%s", baseurl, a->a_sha1);
        htsmsg_add_str(artifact, "url", url);
        htsmsg_add_msg(artifacts, NULL, artifact);
      }
      htsmsg_add_msg(out, "artifacts", artifacts);

      struct change_queue cq;
      if(!git_changelog(&cq, p, b->b_revision, 0, 100, 0, b->b_target)) {
        htsmsg_t *changelog = htsmsg_create_list();
        change_t *c;
        TAILQ_FOREACH(c, &cq, link) {
          htsmsg_t *e = htsmsg_create_map();
          htsmsg_add_str(e, "version", c->version);
          htsmsg_add_str(e, "desc", c->msg);
          htsmsg_add_msg(changelog, NULL, e);
        }
        htsmsg_add_msg(out, "changelog", changelog);
        git_changlog_free(&cq);
      }

      char *json = htsmsg_json_serialize_to_str(out, 1);
      htsmsg_destroy(out);

      char path[PATH_MAX];
      snprintf(path, sizeof(path), "%s/%s-%s.json",
               outpath, track, b->b_target);

      int err = writefile(path, json, strlen(json));
      if(err == WRITEFILE_NO_CHANGE) {

      } else if(err) {
        plog(p, logctx,
             "Unable to wrtite updatemanifest file %s -- %s",
             path, strerror(err));
      } else {
        char logctx[128];
        snprintf(logctx, sizeof(logctx), "release/manifest/publish/%s",
                 b->b_target);
        plog(p, logctx,
             "New release '%s' available for %s",
             b->b_version, b->b_target);
      }
      free(json);
    }
  }
}
