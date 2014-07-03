
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

#include "libsvc/misc.h"
#include "libsvc/htsmsg_json.h"
#include "libsvc/trace.h"
#include "libsvc/db.h"
#include "libsvc/talloc.h"
#include "libsvc/cmd.h"
#include "libsvc/threading.h"

#include "releasemaker.h"
#include "git.h"
#include "sql_statements.h"
#include "s3.h"

typedef struct releasemaker {
  project_t *p;

  cfg_t *pc;
  cfg_t *rt_cfg;
  cfg_t *tracks_cfg;
  cfg_t *targets_cfg;
  int num_targets;

  struct build_queue builds;
  struct target_queue targets;

} releasemaker_t;



/**
 * Write a manifest file
 */
static int
write_manifest(releasemaker_t *rm, htsmsg_t *m, const char *name)
{
  int err;
  const char *manifestdir = cfg_get_str(rm->rt_cfg, CFG("manifestDir"), NULL);
  project_t *p = rm->p;
  char path[PATH_MAX];
  if(manifestdir == NULL) {
    plog(p, "release/info/all", "No manifestDir configured");
    return EINVAL;
  }

  char *json = htsmsg_json_serialize_to_str(m, 1);

  if(!strncmp(manifestdir, "s3://", strlen("s3://"))) {
    manifestdir += strlen("s3://");

    const char *bucket = cfg_get_str(rm->pc, CFG("s3", "bucket"), NULL);
    const char *secret = cfg_get_str(rm->pc, CFG("s3", "secret"), NULL);
    const char *awsid  = cfg_get_str(rm->pc, CFG("s3", "awsid"),  NULL);

    if(bucket == NULL || secret == NULL || awsid == NULL) {
      free(json);
      return EINVAL;
    }

    snprintf(path, sizeof(path), "%s/%s", manifestdir, name);

    char errbuf[512];

    int r = aws_s3_put_file(bucket, awsid, secret, path,
                            errbuf, sizeof(errbuf), json, strlen(json),
                            "application/json");
    free(json);
    if(r) {
      plog(p, "storage/s3", "Unable to upload %s -- %s", path, errbuf);
      return EIO;
    }
    return 0;

  } else {
    makedirs(manifestdir);
    snprintf(path, sizeof(path), "%s/%s", manifestdir, name);

    err = writefile(path, json, strlen(json));
  }

  free(json);
  return err;
}




/**
 * Given a project + commit OID and a target, open
 * Manifests/<target>.json and return it
 *
 * This should use common stuff from git.c instead
 */
static htsmsg_t *
get_embedded_manifest(project_t *p, const char *target,
                      const git_oid *oid, const char *logctx)
{
  git_tree *tree = NULL;
  git_commit *commit = NULL;
  git_object *manifests = NULL;
  git_object *blob = NULL;
  const git_tree_entry *e;

  htsmsg_t *r = NULL;

  scoped_lock(&p->p_repo_mutex);

  if(git_commit_lookup(&commit, p->p_repo, oid)) {
    plog(p, logctx, "Unable to lookup commit id when looking for manifest");
    return NULL;
  }

  if(git_commit_tree(&tree, commit)) {
    plog(p, logctx, "Unable to open git tree when looking for manifest");
    goto cleanup;
  }

  if((e = git_tree_entry_byname(tree, "Manifests")) == NULL)  {
    plog(p, logctx, "Manifests directory not found");
    goto cleanup;
  }

  if(git_tree_entry_to_object(&manifests, p->p_repo, e)) {
    plog(p, logctx, "Unable to lookup manifest tree object");
    goto cleanup;
  }

  if(git_object_type(manifests) != GIT_OBJ_TREE) {
    plog(p, logctx, "Manifests/ is not a tree object");
    goto cleanup;
  }

  char filename[128];
  snprintf(filename, sizeof(filename), "%s.json", target);

  if((e = git_tree_entry_byname((git_tree *)manifests, filename)) == NULL)  {
    plog(p, logctx, "%s not found", filename);
    goto cleanup;
  }

  if(git_tree_entry_to_object(&blob, p->p_repo, e)) {
    plog(p, logctx, "Unable to lookup %s object", filename);
    goto cleanup;
  }

  if(git_object_type(blob) != GIT_OBJ_BLOB) {
    plog(p, logctx, "%s is not a file", filename);
    goto cleanup;
  }


  size_t size = git_blob_rawsize((git_blob *)blob);
  char *data = talloc_malloc(size + 1);
  memcpy(data, git_blob_rawcontent((git_blob *)blob), size);
  data[size] = 0;

  char errbuf[512];

  r = htsmsg_json_deserialize(data, errbuf, sizeof(errbuf));
  if(r == NULL)
    plog(p, logctx, "Unable to decode JSON in '%s' -- %s", filename, errbuf);


 cleanup:
  git_object_free(blob);
  git_object_free(manifests);
  if(tree != NULL)
    git_tree_free(tree);
  if(commit != NULL)
    git_commit_free(commit);
  return r;
}

/**
 *
 */
static int
releasemaker_init(releasemaker_t *rm, project_t *p, cfg_t *pc)
{
  TAILQ_INIT(&rm->builds);
  TAILQ_INIT(&rm->targets);

  rm->p = p;

  rm->pc = pc;

  rm->rt_cfg = cfg_get_map(pc, "releaseTracks");
  if(rm->rt_cfg == NULL) {
    plog(p, "release/info/all", "No releaseTracks configured");
    return DOOZER_ERROR_PERMANENT;
  }

  rm->tracks_cfg = cfg_get_list(rm->rt_cfg, "tracks");
  if(rm->tracks_cfg == NULL) {
    plog(p, "release/info/all", "No tracks configured");
    return DOOZER_ERROR_PERMANENT;
  }

  rm->targets_cfg = cfg_get_list(rm->rt_cfg, "targets");
  if(rm->targets_cfg == NULL) {
    plog(p, "release/info/all", "No targets configured");
    return DOOZER_ERROR_PERMANENT;
  }

  rm->num_targets = cfg_list_length(rm->targets_cfg);

  return 0;
}


/**
 * Given a Git revision (OID) we walk the tree and try to find
 * a successful build matching the revision
 */
static int
find_successful_build(releasemaker_t *rm, git_oid *start_oid, 
		      const char *branch)
{
  char oidtxt[41];
  git_oid oid;
  git_revwalk *walk;
  project_t *p = rm->p;
  db_conn_t *c = db_get_conn();
  build_t *b;

  if(c == NULL)
    return DOOZER_ERROR_TRANSIENT;


  struct build_queue tentative_builds;
  TAILQ_INIT(&tentative_builds);

  htsmsg_field_t *target_field;
  HTSMSG_FOREACH(target_field, rm->targets_cfg) {
    htsmsg_t *target_cfg = htsmsg_get_map_by_field(target_field);
    if(target_cfg == NULL)
      continue;
    const char *t_name = cfg_get_str(target_cfg, CFG("target"), NULL);
    if(t_name == NULL)
      continue;

    b = talloc_malloc(sizeof(build_t));
    snprintf(b->b_target, sizeof(b->b_target), "%s", t_name);
    snprintf(b->b_branch, sizeof(b->b_branch), "%s", branch);
    TAILQ_INSERT_TAIL(&tentative_builds, b, b_global_link);
  }


  scoped_lock(&p->p_repo_mutex);
  git_revwalk_new(&walk, p->p_repo);
  git_revwalk_push(walk, start_oid);
  git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL);
  oidtxt[40] = 0;

  db_stmt_t *s = db_stmt_get(c,
			      "SELECT id,target,version "
			      "FROM build "
			      "WHERE revision=? "
			      "AND project=? "
			      "AND status=? "
                              "ORDER BY id DESC");

  int cnt = 0;

  while(!git_revwalk_next(&oid, walk)) {
    cnt++;
    if(cnt == 100)
      break;

    git_oid_fmt(oidtxt, &oid);
    if(db_stmt_exec(s, "sss", oidtxt, p->p_id, "done"))
      goto bad;

    while(1) {
      int id;
      char target[64];
      char version[64];

      int r = db_stream_row(0, s, 
			    DB_RESULT_INT(id),
			    DB_RESULT_STRING(target),
			    DB_RESULT_STRING(version));
      if(r < 0) 
	goto bad;

      if(r)
	break;

      TAILQ_FOREACH(b, &tentative_builds, b_global_link) {
	if(!strcmp(b->b_target, target))
	  break;
      }

      if(b != NULL) {
	b->b_id = id;

        git_oid_cpy(&b->b_oid, &oid);
	strcpy(b->b_version, version);
	TAILQ_REMOVE(&tentative_builds, b, b_global_link);
	TAILQ_INSERT_TAIL(&rm->builds, b, b_global_link);
      }
    }

    if(TAILQ_FIRST(&tentative_builds) == NULL)
      break;
  }

  TAILQ_FOREACH(b, &tentative_builds, b_global_link) {
    char logctx[100];
    snprintf(logctx, sizeof(logctx), "release/info/%s", b->b_target);
    plog(rm->p, logctx, "No build for target %s in %s", b->b_target,
	 branch);
  }

  git_revwalk_free(walk);
  return 0;
 bad:
  git_revwalk_free(walk);
  return DOOZER_ERROR_TRANSIENT;
}



/**
 * This function tries to, for each configured branch, find a
 * successful build with as high revision (as close to the branch ref)
 * as possible.
 */
static int
find_successful_builds(releasemaker_t *rm)
{
  struct ref_list refs;
  ref_t *r;

  /**
   * git_repo_list_branches() returns the ref names in descending
   * dictionary order (so 4.3 comes before 4.1, etc)
   */

  git_repo_list_branches(rm->p, &refs);

  htsmsg_field_t *f;
  HTSMSG_FOREACH(f, rm->tracks_cfg) {
    htsmsg_t *m = htsmsg_get_map_by_field(f);
    if(m == NULL)
      continue;

    const char *pattern = htsmsg_get_str(m, "branch");
    if(pattern == NULL)
      continue;

    LIST_FOREACH(r, &refs, link) {
      if(!fnmatch(pattern, r->name, FNM_PATHNAME))
	break;
    }

    if(r == NULL) {
      plog(rm->p, "release/info/all", "No matching ref for branch pattern %s",
	   pattern);
      continue;
    }
    find_successful_build(rm, &r->oid, r->name);
  }
  git_repo_free_refs(&refs);
  return 0;
}




/**
 *
 */
static int
releasemaker_list_builds(releasemaker_t *rm)
{
  build_t *b;
  target_t *t;

  find_successful_builds(rm);

  db_conn_t *c = db_get_conn();

  if(c == NULL)
    return DOOZER_ERROR_TRANSIENT;

  TAILQ_FOREACH(b, &rm->builds, b_global_link) {

    db_stmt_t *s = db_stmt_get(c, SQL_GET_ARTIFACTS);

    if(db_stmt_exec(s, "i", b->b_id))
      return DOOZER_ERROR_TRANSIENT;

    TAILQ_INIT(&b->b_artifacts);
    while(1) {
      artifact_t *a = talloc_malloc(sizeof(artifact_t));
      int r = db_stream_row(0, s,
                            DB_RESULT_INT(a->a_id),
                            DB_RESULT_STRING(a->a_type),
                            DB_RESULT_STRING(a->a_sha1),
                            DB_RESULT_INT(a->a_size),
                            DB_RESULT_STRING(a->a_name));
      if(r < 0)
        return DOOZER_ERROR_TRANSIENT;
      if(r)
        break;
      TAILQ_INSERT_HEAD(&b->b_artifacts, a, a_link);
    }
  }



  TAILQ_FOREACH(b, &rm->builds, b_global_link) {
    TAILQ_FOREACH(t, &rm->targets, t_link)
      if(!strcmp(t->t_target, b->b_target))
        break;

    if(t == NULL) {
      t = talloc_malloc(sizeof(target_t));
      strcpy(t->t_target, b->b_target);
      TAILQ_INSERT_TAIL(&rm->targets, t, t_link);
      TAILQ_INIT(&t->t_builds);
    }
    TAILQ_INSERT_TAIL(&t->t_builds, b, b_target_link);
  }
  return 0;
}


/**
 *
 */
static void
generate_update_tracks(releasemaker_t *rm)
{
  build_t *b;
  artifact_t *a;
  target_t *t;
  char logctx[128];
  project_t *p = rm->p;

  cfg_root(root);

  const char *baseurl;
  baseurl = cfg_get_str(rm->rt_cfg, CFG("artifactPrefix"), NULL);

  if(baseurl == NULL) {
    baseurl = cfg_get_str(root, CFG("artifactPrefix"), NULL);
    if(baseurl == NULL) {
      plog(p, "release/info/all", "No artifactPrefix configured");
      return;
    }
  }

  htsmsg_t *outtracks = htsmsg_create_list();

  for(int i = 0; ; i++) {
    const char *trackid =
      cfg_get_str(rm->tracks_cfg, CFG(CFG_INDEX(i), "name"),   NULL);
    const char *tracktitle  =
      cfg_get_str(rm->tracks_cfg, CFG(CFG_INDEX(i), "title"),   NULL);
    const char *branchpattern =
      cfg_get_str(rm->tracks_cfg, CFG(CFG_INDEX(i), "branch"), NULL);

    if(trackid == NULL || branchpattern == NULL || tracktitle == NULL)
      break;

    const char *desc =
      cfg_get_str(rm->tracks_cfg, CFG(CFG_INDEX(i), "description"), NULL);

    htsmsg_t *outtargets = htsmsg_create_list();

    htsmsg_field_t *tfield;
    HTSMSG_FOREACH(tfield, rm->targets_cfg) {
      htsmsg_t *target = htsmsg_get_map_by_field(tfield);
      if(target == NULL)
        continue;

      const char *t_name  = cfg_get_str(target, CFG("target"), NULL);
      const char *t_title = cfg_get_str(target, CFG("title"), NULL);
      if(t_name == NULL)
        continue;

      snprintf(logctx, sizeof(logctx), "release/info/%s", t_name);

      TAILQ_FOREACH(t, &rm->targets, t_link)
        if(!strcmp(t->t_target, t_name))
          break;

      if(t == NULL) {
        plog(p, logctx,
             "Manifest: Target %s: No builds available", t_name);
        continue;
      }

      TAILQ_FOREACH(b, &t->t_builds, b_target_link) {
        if(!fnmatch(branchpattern, b->b_branch, FNM_PATHNAME))
          break;
      }

      if(b == NULL) {
        plog(p, logctx,
             "ReleaseTrack %s: Target %s: no matching branch for pattern '%s'",
              trackid, t->t_target, branchpattern);
        continue;
      }
      plog(p, logctx,
           "ReleaseTrack: %s Target %s: Using branch '%s' for pattern '%s'",
           trackid, t->t_target, b->b_branch, branchpattern);

      htsmsg_t *manifest = get_embedded_manifest(p, t_name, &b->b_oid, logctx);

      cfg_t *artifacts_cfg = cfg_get_list(target, "artifacts");

      if(artifacts_cfg == NULL) {
        plog(p, logctx,
             "Manifest: Target %s: No artifacts configured", t_name);
        continue;
      }

      htsmsg_t *out_single = htsmsg_create_map(); // For a single track+target

      htsmsg_add_str(out_single, "arch",    b->b_target);
      htsmsg_add_str(out_single, "title",   t_title);
      htsmsg_add_str(out_single, "version", b->b_version);
      htsmsg_add_str(out_single, "branch",  b->b_branch);

      int out_all_got_artifacts = 0;

      htsmsg_t *out_all = htsmsg_copy(out_single); // For all.json

      htsmsg_t *artifacts_single = htsmsg_create_list();
      htsmsg_t *artifacts_all    = htsmsg_create_list();

      htsmsg_field_t *afield;
      HTSMSG_FOREACH(afield, artifacts_cfg) {
        htsmsg_t *am = htsmsg_get_map_by_field(afield);
        const char *amtype = cfg_get_str(am, CFG("type"), NULL);
        if(amtype == NULL)
          continue;
        const char *amtitle = cfg_get_str(am, CFG("title"), NULL);

        TAILQ_FOREACH(a, &b->b_artifacts, a_link) {
          if(!strcmp(a->a_type, amtype)) {

            htsmsg_t *artifact = htsmsg_create_map();
            char url[1024];

            htsmsg_add_str(artifact, "type", a->a_type);
            htsmsg_add_str(artifact, "name", a->a_name);
            htsmsg_add_str(artifact, "sha1", a->a_sha1);
            htsmsg_add_u32(artifact, "size", a->a_size);
            snprintf(url, sizeof(url), "%s/file/%s", baseurl, a->a_sha1);
            htsmsg_add_str(artifact, "url", url);

            // We only want to include artifacts with a title in all.json

            htsmsg_add_msg(artifacts_single, NULL, htsmsg_copy(artifact));

            if(amtitle != NULL) {
              htsmsg_add_str(artifact, "title", amtitle);
              htsmsg_add_msg(artifacts_all, NULL, artifact);
              out_all_got_artifacts = 1;
            } else {
              htsmsg_destroy(artifact);
            }
          }
        }
      }

      htsmsg_add_msg(out_single, "artifacts", artifacts_single);
      htsmsg_add_msg(out_all,    "artifacts", artifacts_all);

      if(manifest != NULL) {
        htsmsg_add_msg(out_single, "manifest", htsmsg_copy(manifest));
        htsmsg_add_msg(out_all,    "manifest", manifest);
      }

      struct change_queue cq;
      if(!git_changelog(&cq, p, &b->b_oid, 0, 100, 0, b->b_target)) {
        htsmsg_t *changelog = htsmsg_create_list();
        change_t *c;
        TAILQ_FOREACH(c, &cq, link) {
          htsmsg_t *e = htsmsg_create_map();
          htsmsg_add_str(e, "version", c->version);
          htsmsg_add_str(e, "desc", c->msg);
          htsmsg_add_msg(changelog, NULL, e);
        }
        htsmsg_add_msg(out_single, "changelog", changelog);
        git_changlog_free(&cq);
      }

      char mname[128];
      snprintf(mname, sizeof(mname), "%s-%s.json", trackid, b->b_target);

      int err = write_manifest(rm, out_single, mname);
      htsmsg_destroy(out_single);

      if(err == WRITEFILE_NO_CHANGE) {

      } else if(err) {
        plog(p, logctx,
             "Unable to write releasetrack file %s -- %s",
             mname, strerror(err));
      } else {
        snprintf(logctx, sizeof(logctx), "release/publish/%s",
                 b->b_target);
        plog(p, logctx,
             COLOR_GREEN "%s release '%s' available for %s",
             tracktitle, b->b_version, b->b_target);
      }


      // Only add to all.json if the target had any artifacts at all

      if(out_all_got_artifacts) {
        htsmsg_add_msg(outtargets, NULL, out_all);
      }
    }

    // If no description is set it wont be part of the all.json
    // (ie, on download pages, etc)

    if(desc == NULL) {
      htsmsg_destroy(outtargets);
    } else {
      htsmsg_t *outtrack = htsmsg_create_map();
      htsmsg_add_str(outtrack, "id", trackid);
      htsmsg_add_str(outtrack, "name", tracktitle);  // Legacy
      htsmsg_add_str(outtrack, "title", tracktitle);
      htsmsg_add_str(outtrack, "description", desc);
      htsmsg_add_msg(outtrack, "targets", outtargets);

      htsmsg_add_msg(outtracks, NULL, outtrack);
    }
  }

  int err = write_manifest(rm, outtracks, "all.json");
  htsmsg_destroy(outtracks);

  if(err == WRITEFILE_NO_CHANGE) {

  } else if(err) {
    plog(p, "release/info/all",
         "Unable to write updatemanifest file -- %s", strerror(err));
  } else {
    plog(p, "release/info/all", "Release manifest generated");
  }
}


/**
 *
 */
int
releasemaker_update_project(project_t *p)
{
  releasemaker_t rm;

  plog(p, "release/check", "Starting relesemaker check");

  project_cfg(pc, p->p_id);
  if(pc == NULL)
    return DOOZER_ERROR_PERMANENT;

  if(releasemaker_init(&rm, p, pc))
    return DOOZER_ERROR_PERMANENT;

  int r = releasemaker_list_builds(&rm);
  if(!r)
    generate_update_tracks(&rm);
  return r;
}


/**
 *
 */
static int
show_builds(const char *user,
            int argc, const char **argv, int *intv,
            void (*msg)(void *opaque, const char *fmt, ...),
            void *opaque)
{
  project_t *p = project_get(argv[0]);

  if(p == NULL)
    return 1;

  project_cfg(pc, p->p_id);
  if(pc == NULL)
    return 1;

  releasemaker_t rm;
  if(releasemaker_init(&rm, p, pc))
    return DOOZER_ERROR_PERMANENT;

  int r = releasemaker_list_builds(&rm);
  if(r)
    return r;

  target_t *t;
  build_t *b;

  msg(opaque, "Active builds for %s", argv[0]);
  TAILQ_FOREACH(t, &rm.targets, t_link) {
    msg(opaque, "  For %s", t->t_target);
    TAILQ_FOREACH(b, &t->t_builds, b_target_link) {
      msg(opaque, "    %s from branch %s (Build #%d)",
          b->b_version, b->b_branch, b->b_id);
      artifact_t *a;
      TAILQ_FOREACH(a, &b->b_artifacts, a_link) {
        msg(opaque, "      #%-5d %-8s %s %d bytes",
               a->a_id, a->a_type, a->a_sha1, a->a_size);
      }
    }
  }

  return 0;
}

CMD(show_builds,
    CMD_LITERAL("show"),
    CMD_LITERAL("builds"),
    CMD_VARSTR("project"));


static int
do_delete_builds(const char *user,
                 int argc, const char **argv, int *intv,
                 void (*msg)(void *opaque, const char *fmt, ...),
                 void *opaque, int do_commit)
{
  const char *project = argv[0];
  project_t *p = project_get(project);
  const char *pfx = do_commit ? "Deleted " : "";
  if(p == NULL)
    return 1;

  project_cfg(pc, p->p_id);
  if(pc == NULL)
    return 1;

  releasemaker_t rm;
  if(releasemaker_init(&rm, p, pc))
    return 1;

  build_t *b;

  int deprecated = 0;
  const char *by_status = NULL;

  if(!strcmp(argv[1], "deprecated")) {
    deprecated = 1;
  } else if(!strcmp(argv[1], "failed")) {
    by_status = "failed";
  } else if(!strcmp(argv[1], "pending")) {
    by_status = "pending";
  } else {
    msg(opaque, "Unknown filter");
    return 1;
  }

  db_conn_t *c = db_get_conn();

  if(c == NULL)
    return DOOZER_ERROR_TRANSIENT;

  if(db_begin(c))
    return DOOZER_ERROR_TRANSIENT;

  if(deprecated) {

    int r = releasemaker_list_builds(&rm);
    if(r) {
      db_rollback(c);
      return r;
    }

    htsbuf_queue_t hq;
    htsbuf_queue_init(&hq, INT_MAX);

    htsbuf_qprintf(&hq, "DELETE FROM build WHERE project=? AND status=? ");

    if(TAILQ_FIRST(&rm.builds) != NULL) {
      htsbuf_qprintf(&hq, "AND id NOT IN (");
      int p=0;
      TAILQ_FOREACH(b, &rm.builds, b_global_link) {
        msg(opaque, "   Skipping active build #%-6d %-20s %-16s %-16s",
            b->b_id, b->b_version, b->b_branch , b->b_target);
        htsbuf_qprintf(&hq, "%s%d", p ? "," : "", b->b_id);
        p = 1;
      }
      htsbuf_qprintf(&hq, ")");
    }

    char *x = htsbuf_to_string(&hq);

    scoped_db_stmt(s, x);

    if(db_stmt_exec(s, "ss", project, "done")) {
      db_rollback(c);
      return 1;
    }

    msg(opaque, "%s%d deprecated builds", pfx, db_stmt_affected_rows(s));

    free(x);
  }

  if(by_status) {
    db_stmt_t *s =
      db_stmt_get(c,
                  "DELETE from build WHERE project=? AND status=?");

    if(db_stmt_exec(s, "ss", project, by_status)) {
      db_rollback(c);
      return DOOZER_ERROR_TRANSIENT;
    }

    msg(opaque, "%s%d %s builds",
        pfx, (int)db_stmt_affected_rows(s), by_status);
  }

  if(do_commit)
    db_commit(c);
  else
    db_rollback(c);

  return 0;
}

static int
delete_builds(const char *user,
              int argc, const char **argv, int *intv,
              void (*msg)(void *opaque, const char *fmt, ...),
              void *opaque)
{
  return do_delete_builds(user, argc, argv, intv, msg, opaque, 1);
}


CMD(delete_builds,
    CMD_LITERAL("delete"),
    CMD_LITERAL("builds"),
    CMD_VARSTR("project"),
    CMD_VARSTR("deprecated | failed | pending"));

static int
count_delete_builds(const char *user,
                    int argc, const char **argv, int *intv,
                    void (*msg)(void *opaque, const char *fmt, ...),
                    void *opaque)
{
  return do_delete_builds(user, argc, argv, intv, msg, opaque, 0);
}

CMD(count_delete_builds,
    CMD_LITERAL("count"),
    CMD_LITERAL("builds"),
    CMD_VARSTR("project"),
    CMD_VARSTR("deprecated | failed | pending"));

