#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <fnmatch.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "libsvc/http.h"
#include "libsvc/misc.h"
#include "libsvc/trace.h"
#include "libsvc/urlshorten.h"
#include "libsvc/db.h"
#include "libsvc/cmd.h"

#include "buildmaster.h"
#include "git.h"
#include "sql_statements.h"

static int add_build(project_t *p, const char *branch, const char *revision,
                     const char *target, const char *reason, int no_output);


/**
 *
 */
static const char *
build_url(cfg_t *pc, int id)
{
  static __thread char rbuf[512];

  const char *pfx = cfg_get_str(pc, CFG("buildUrlPrefix"), NULL);

  if(pfx == NULL || *pfx == 0)
    return NULL;
  snprintf(rbuf, sizeof(rbuf), "%s%s%d",
           pfx, pfx[strlen(pfx)-1] == '/' ? "" : "/", id);
  return urlshorten(rbuf);
}


/**
 *
 */
static cfg_t *
find_branch_config(project_t *p, cfg_t *bmconf, const char *id)
{
  cfg_t *branch_conf = cfg_get_list(bmconf, "branches");
  if(branch_conf == NULL) {
    plog(p, "system", "Project lacks buildmaster.branches config");
    return NULL;
  }

  htsmsg_field_t *f;
  HTSMSG_FOREACH(f, branch_conf) {
    htsmsg_t *m = htsmsg_get_map_by_field(f);
    if(m == NULL)
      continue;

    const char *pattern = htsmsg_get_str(m, "pattern");
    if(pattern != NULL && !fnmatch(pattern, id, FNM_PATHNAME))
      return m;
  }
  return NULL;
}

/**
 *
 */
int
buildmaster_check_for_builds(project_t *p)
{
  int retval = 0;
  plog(p, "build/check", "Checking if need to build anything");

  project_cfg(pc, p->p_id);
  if(pc == NULL)
    return DOOZER_ERROR_PERMANENT;

  cfg_t *bmconf = cfg_get_map(pc, "buildmaster");
  if(bmconf == NULL) {
    plog(p, "build/check", "Project lacks buildmaster config");
    return DOOZER_ERROR_PERMANENT;
  }

  cfg_t *tconf = cfg_get_list(bmconf, "targets");
  if(tconf == NULL) {
    plog(p, "build/check", "Project lacks buildmaster.targets config");
    return DOOZER_ERROR_PERMANENT;
  }

  int num_targets = cfg_list_length(tconf);
  char tmark[num_targets];

  conn_t *c = db_get_conn();
  if(c == NULL)
    return DOOZER_ERROR_TRANSIENT;

  struct ref_list refs;
  git_repo_list_branches(p, &refs);
  ref_t *b;

  LIST_FOREACH(b, &refs, link) {
    cfg_t *bc = find_branch_config(p, bmconf, b->name);
    if(bc == NULL)
      continue;

    if(!cfg_get_int(bc, CFG("autobuild"), 0))
      continue;

    plog(p, "build/check", "Checking build status for branch %s (%.8s)",
         b->name, b->oidtxt);

    MYSQL_STMT *s = db_stmt_get(c, SQL_GET_TARGETS_FOR_BUILD);
    if(db_stmt_exec(s, "sss", b->oidtxt, p->p_id, b->name)) {
      retval = DOOZER_ERROR_TRANSIENT;
      break;
    }

    // Clear out marked targets
    memset(tmark, 0, num_targets);

    while(1) {
      char target[64];
      int id;
      char status[64];
      int r = db_stream_row(0, s,
                            DB_RESULT_STRING(target),
                            DB_RESULT_INT(id),
                            DB_RESULT_STRING(status));
      if(r < 0)
        retval = DOOZER_ERROR_TRANSIENT;
      if(r)
        break;

      for(int i = 0; i < num_targets; i++) {
        const char *configured_target = cfg_get_str(tconf, CFGI(i), NULL);
        if(!strcmp(configured_target, target))
          tmark[i] = 1;
      }
    }

    for(int i = 0; i < num_targets; i++) {
      const char *configured_target = cfg_get_str(tconf, CFGI(i), NULL);
      if(tmark[i])
        continue;

      int no_output = cfg_get_int(bc, CFG("noOutput"), 0);
      add_build(p, b->name, b->oidtxt, configured_target,
                "Automatic build", no_output);
    }
  }
  git_repo_free_refs(&refs);
  return retval;
}


/**
 *
 */
static int
add_build(project_t *p, const char *branch, const char *revision,
          const char *target, const char *reason, int no_output)
{
  char ver[512];

  if(git_describe(ver, sizeof(ver), p, revision, 1))
    return DOOZER_ERROR_PERMANENT;

  conn_t *c = db_get_conn();
  if(c == NULL)
    return DOOZER_ERROR_TRANSIENT;

  plog(p, "build/queue",
       "Enqueue build for %s (%s %.8s) on %s by '%s'%s",
       ver, branch, revision, target, reason,
       no_output ? ", No artifacts will be stored" : "");

  db_stmt_exec(db_stmt_get(c, SQL_INSERT_BUILD), "sssssssi",
               p->p_id, revision, target, reason,
               "pending", branch, ver, no_output);
  return 0;
}


/**
 *
 */
static int
getjob(char **targets, unsigned int numtargets, buildjob_t *bj,
       const char *agent)
{
  if(numtargets == 0)
    return DOOZER_ERROR_INVALID_ARGS;

  char query[1024];

  int i, l = 0;
  l += snprintf(query, sizeof(query),
                "SELECT id,revision,target,project,version,no_output "
                "FROM build "
                "WHERE status='pending' AND (");

  for(i = 0; i < numtargets; i++)
    l += snprintf(query + l, sizeof(query) - l, "target=?%s",
                  i != numtargets - 1 ? " OR " : " ");

  snprintf(query + l, sizeof(query) - l,
           ") ORDER BY created LIMIT 1 FOR UPDATE");

  conn_t *c = db_get_conn();
  if(c == NULL)
    return DOOZER_ERROR_PERMANENT;

  scoped_db_stmt(s, query);
  if(s == NULL)
    return DOOZER_ERROR_PERMANENT;

  assert(mysql_stmt_param_count(s) == numtargets);

  MYSQL_BIND in[numtargets];
  memset(in, 0, sizeof(MYSQL_BIND) * numtargets);


  for(i = 0; i < numtargets; i++) {
    in[i].buffer_type = MYSQL_TYPE_STRING;
    in[i].buffer = (char *)targets[i];
    in[i].buffer_length = strlen(targets[i]);
  }

  if(mysql_stmt_bind_param(s, in)) {
    trace(LOG_ERR,
          "Failed to bind parameters to prepared statement %s -- %s",
          mysql_stmt_sqlstate(s), mysql_stmt_error(s));
    return -1;
  }

  db_begin(c);

  if(mysql_stmt_execute(s)) {
    trace(LOG_ERR, "Failed to execute statement %s -- %s",
          mysql_stmt_sqlstate(s), mysql_stmt_error(s));
    return -1;
  }

  int r = db_stream_row(0, s,
                        DB_RESULT_INT(bj->id),
                        DB_RESULT_STRING(bj->revision),
                        DB_RESULT_STRING(bj->target),
                        DB_RESULT_STRING(bj->project),
                        DB_RESULT_STRING(bj->version),
                        DB_RESULT_INT(bj->no_output),
                        NULL);

  mysql_stmt_reset(s);

  switch(r) {
  case DB_ERR_OTHER:
    db_rollback(c);
    return DOOZER_ERROR_TRANSIENT;
  case DB_ERR_NO_DATA:
    db_rollback(c);
    return DOOZER_ERROR_NO_DATA;
  }

  snprintf(bj->jobsecret, sizeof(bj->jobsecret), "%u",
           (unsigned int)lrand48());

  db_stmt_exec(db_stmt_get(c, SQL_ALLOC_BUILD), "sssi",
               agent, "building", bj->jobsecret, bj->id);

  bj->db = c;
  return 0;
}


/**
 *
 */
static int
http_getjob(http_connection_t *hc, const char *remain, void *opaque)
{
  cfg_root(root);

  const char *agent   = http_arg_get(&hc->hc_req_args, "agent");
  const char *secret  = http_arg_get(&hc->hc_req_args, "secret");
  char *targetsarg = http_arg_get(&hc->hc_req_args, "targets");
  if(agent == NULL || secret == NULL || targetsarg == NULL)
    return 400;

  const char *s = cfg_get_str(root, CFG("buildmaster", "agents",
                                        agent, "secret"), NULL);

  int longpolltimeout =
    cfg_get_int(root, CFG("http", "longpollTimeout"), 60);

  if(s == NULL) {
    trace(LOG_ERR, "Agent '%s' not configured", agent);
    return 403;
  }

  if(strcmp(s, secret)) {
    trace(LOG_ERR, "agent '%s' rejected because of invalid secret '%s'",
          agent, secret);
    return 403;
  }

  char *targets[64];
  int numtargets = str_tokenize(targetsarg, targets, 64, ',');
  int fails = 0;
  time_t deadline = time(NULL) + longpolltimeout;

  while(1) {

    buildjob_t bj;
    int r = getjob(targets, numtargets, &bj, agent);

    switch(r) {
    default:
      return 500;

    case DOOZER_ERROR_INVALID_ARGS:
      return 400;

    case DOOZER_ERROR_TRANSIENT:
      fails++;
      if(fails < 10) {
        trace(LOG_INFO, "Transient error while querying db, retry #%d",
              fails);
        sleep(1);
        continue;
      }
    none:
      htsbuf_qprintf(&hc->hc_reply, "type=none\n");
      http_output_content(hc, "text/plain; charset=utf-8");
      return 0;

    case DOOZER_ERROR_NO_DATA:
      if(time(NULL) > deadline)
        goto none;
      sleep(1);
      continue;

    case 0: {
      project_cfg(pc, bj.project);
      if(pc == NULL) {
        db_rollback(bj.db);
        return 503;
      }

      project_t *p = project_get(bj.project);
      plog(p, "build/queue", "Build #%d: %s rev:%.8s claimed by %s",
            bj.id, bj.version, bj.revision, agent);

      const char *upstream = cfg_get_str(pc, CFG("gitrepo", "pub"), NULL);
      if(upstream == NULL) {
        db_rollback(bj.db);
        return 503;
      }

      htsbuf_qprintf(&hc->hc_reply,
                     "type=build\n"
                     "id=%d\n"
                     "revision=%s\n"
                     "target=%s\n"
                     "jobsecret=%s\n"
                     "project=%s\n"
                     "repo=%s\n"
                     "postfix=%s\n"
                     "no_output=%d\n",
                     bj.id,
                     bj.revision,
                     bj.target,
                     bj.jobsecret,
                     bj.project,
                     upstream,
                     bj.version,
                     bj.no_output);

      if(http_output_content(hc, "text/plain; charset=utf-8")) {
        plog(p, "build/queue", "Build #%d: Transaction aborted, "
              "HTTP write failed to agent %s",
              bj.id, agent);
        db_rollback(bj.db);
        return 0;
      } else {
        db_commit(bj.db);
      }
      return 0;
    }
    }
  }
}


/**
 *
 */
static int
http_artifact(http_connection_t *hc, const char *remain, void *opaque)
{
  const char *jobidstr  = http_arg_get(&hc->hc_req_args, "jobid");
  const char *jobsecret = http_arg_get(&hc->hc_req_args, "jobsecret");
  const char *type      = http_arg_get(&hc->hc_req_args, "type");
  const char *name      = http_arg_get(&hc->hc_req_args, "name");
  const char *md5sum    = http_arg_get(&hc->hc_req_args, "md5sum");
  const char *sha1sum   = http_arg_get(&hc->hc_req_args, "sha1sum");

  const char *encoding     = http_arg_get(&hc->hc_args, "content-encoding");
  const char *contenttype  = http_arg_get(&hc->hc_args, "content-type");


  if(jobidstr == NULL ||
     jobsecret == NULL ||
     type == NULL ||
     name == NULL ||
     md5sum == NULL ||
     sha1sum == NULL)
    return 400;

  int jobid = atoi(jobidstr);

  if(hc->hc_cmd != HTTP_CMD_PUT)
    return 405;

  trace(LOG_DEBUG,
        "Build #%d: Received artifact '%s' "
        "content-encoding:'%s' content-type:'%s'",
        jobid, name, encoding ?: "<unset>", contenttype ?: "<unset>");

  conn_t *c = db_get_conn();
  if(c == NULL)
    return 503;

  MYSQL_STMT *s = db_stmt_get(c, SQL_GET_BUILD_BY_ID);

  if(db_stmt_exec(s, "i", jobid))
    return 503;

  char project[64];
  char revision[64];
  char target[64];
  char jobtype[64];
  char agent[64];
  char jobsecret2[64];
  char status[64];
  char version[64];
  char branch[128];

  int r = db_stream_row(0, s,
                        DB_RESULT_STRING(project),
                        DB_RESULT_STRING(revision),
                        DB_RESULT_STRING(target),
                        DB_RESULT_STRING(jobtype),
                        DB_RESULT_STRING(agent),
                        DB_RESULT_STRING(jobsecret2),
                        DB_RESULT_STRING(status),
                        DB_RESULT_STRING(version),
                        DB_RESULT_STRING(branch));

  mysql_stmt_reset(s);

  switch(r) {
  case DB_ERR_OTHER:
    return 500;
  case DB_ERR_NO_DATA:
    trace(LOG_ERR, "Received artifact for unknown job %d", jobid);
    return 404;
  }

  project_t *p = project_get(project);

  if(strcmp(status, "building")) {
    plog(p, "build/artifact",
          "Build #%d: Received artifact '%s' rejected because job is in state %s",
          jobid, name, status);
    return 412;
  }

  if(strcmp(jobsecret, jobsecret2)) {
    plog(p, "build/artifact",
          "Build #%d: Received artifact with invalid jobsecret '%s' expected '%s'",
          jobid, jobsecret, jobsecret2);
    return 403;
  }

  project_cfg(pc, project);
  if(pc == NULL)
    return 410;

  s = db_stmt_get(c, SQL_INSERT_BUILD);

  if(hc->hc_post_len > 16384 ||
     !strcmp(encoding ?: "", "gzip") ||
     !mystrbegins(contenttype, "text/plain")) {

    char path[PATH_MAX];

    const char *basepath = cfg_get_str(pc, CFG("artifactPath"), NULL);
    if(basepath == NULL) {
      plog(p, "build/artifact",
            "Build #%d: Missing artifactPath for project %s",
            jobid, project);
      return 500;
    }
    makedirs(basepath);
    snprintf(path, sizeof(path), "%s/%d", basepath, jobid);
    mkdir(path, 0770);
    snprintf(path, sizeof(path), "%s/%d/%s", basepath, jobid, name);

    int fd = open(path, O_CREAT|O_TRUNC|O_WRONLY, 0640);
    if(fd == -1) {
      plog(p, "build/artifact",
            "Build #%d: Unable to open file '%s' for artifact '%s' -- %s",
            jobid, path, name, strerror(errno));
      return 500;
    }
    if(write(fd, hc->hc_post_data, hc->hc_post_len) != hc->hc_post_len) {
      plog(p, "build/artifact",
            "Build #%d: Unable to write file '%s' for artifact '%s' -- %s",
            jobid, path, name, strerror(errno));
      close(fd);
      unlink(path);
      return 500;
    }

    if(close(fd)) {
      plog(p, "build/artifact",
            "Build #%d: Unable to close file '%s' for artifact '%s' -- %s",
            jobid, path, name, strerror(errno));
      unlink(path);
      return 500;
    }


    snprintf(path, sizeof(path), "%d/%s", jobid, name);

    db_stmt_exec(s, "issssissss",
                 jobid,
                 type,
                 path,
                 "file",
                 name,
                 hc->hc_post_len,
                 md5sum,
                 sha1sum,
                 contenttype,
                 encoding);

    plog(p, "build/artifact",
         "Build #%d: Artifact '%s' stored as file '%s'",
          jobid, name, path);

  } else {

    db_stmt_exec(s, "isbssissss",
                 jobid,
                 type,
                 hc->hc_post_data, hc->hc_post_len,
                 "embedded",
                 name,
                 hc->hc_post_len,
                 md5sum,
                 sha1sum,
                 contenttype,
                 encoding);

    plog(p, "build/artifact",
         "Build #%d: Artifact '%s' stored in db", jobid, name);
  }
  return 200;
}


/**
 *
 */
static int
http_report(http_connection_t *hc, const char *remain, void *opaque)
{
  const char *jobidstr  = http_arg_get(&hc->hc_req_args, "jobid");
  const char *jobsecret = http_arg_get(&hc->hc_req_args, "jobsecret");
  const char *newstatus = http_arg_get(&hc->hc_req_args, "status");
  const char *msg       = http_arg_get(&hc->hc_req_args, "msg");

  if(jobidstr == NULL ||
     jobsecret == NULL ||
     newstatus == NULL)
    return 400;

  int jobid = atoi(jobidstr);

  conn_t *c = db_get_conn();
  if(c == NULL)
    return 503;

  MYSQL_STMT *s = db_stmt_get(c, SQL_GET_BUILD_BY_ID);

  if(db_stmt_exec(s, "i", jobid))
    return 503;

  char project[64];
  char revision[64];
  char target[64];
  char jobtype[64];
  char agent[64];
  char jobsecret2[64];
  char status[64];
  char version[64];
  char branch[128];

  int r = db_stream_row(0, s,
                        DB_RESULT_STRING(project),
                        DB_RESULT_STRING(revision),
                        DB_RESULT_STRING(target),
                        DB_RESULT_STRING(jobtype),
                        DB_RESULT_STRING(agent),
                        DB_RESULT_STRING(jobsecret2),
                        DB_RESULT_STRING(status),
                        DB_RESULT_STRING(version),
                        DB_RESULT_STRING(branch));

  mysql_stmt_reset(s);

  switch(r) {
  case DB_ERR_OTHER:
    return 500;
  case DB_ERR_NO_DATA:
    trace(LOG_ERR, "Received report for unknown job %d", jobid);
    return 404;
  }

  project_t *p = project_get(project);

  if(strcmp(status, "building")) {
    plog(p, "build/status",
          "Build #%d: Received status update '%s' rejected because job is in state %s",
          jobid, newstatus, status);
    return 412;
  }

  if(strcmp(jobsecret, jobsecret2)) {
    plog(p, "build/status",
          "Build #%d: Received status update with invalid jobsecret '%s' expected '%s'",
          jobid, jobsecret, jobsecret2);
    return 403;
  }

  project_cfg(pc, project);
  const char *url = build_url(pc, jobid) ?: "";

  if(!strcmp(newstatus, "building")) {
    db_stmt_exec(db_stmt_get(c, SQL_BUILD_PROGRESS_UPDATE), "si", msg, jobid);
    plog(p, "build/status",
         "Build #%d: %s in %s for %s status: %s", jobid, version, branch, target, msg);
  } else if(!strcmp(newstatus, "failed")) {
    db_stmt_exec(db_stmt_get(c, SQL_BUILD_FINISHED), "ssi", "failed", msg, jobid);
    plog(p, "build/finalstatus",
         COLOR_RED "Build #%d: "COLOR_OFF"%s "COLOR_RED"in "COLOR_OFF"%s "COLOR_RED"for "COLOR_OFF"%s "COLOR_RED"failed: %s %s",
         jobid, version, branch, target, msg, url);
  } else if(!strcmp(newstatus, "done")) {
    db_stmt_exec(db_stmt_get(c, SQL_BUILD_FINISHED), "ssi", "done", NULL, jobid);
    plog(p, "build/finalstatus",
         COLOR_GREEN "Build #%d: "COLOR_OFF"%s "COLOR_GREEN"in "COLOR_OFF"%s "COLOR_GREEN"for "COLOR_OFF"%s "COLOR_GREEN"completed %s",
         jobid, version, branch, target, url);
    project_schedule_job(project_get(project), PROJECT_JOB_GENERATE_RELEASES);
  } else {
    return 400;
  }

  return 200;
}


/**
 *
 */
static int
http_hello(http_connection_t *hc, const char *remain, void *opaque)
{
  cfg_root(root);

  const char *agent  = http_arg_get(&hc->hc_req_args, "agent");
  const char *secret = http_arg_get(&hc->hc_req_args, "secret");
  if(agent == NULL || secret == NULL)
    return 400;

  const char *s = cfg_get_str(root, CFG("buildmaster", "agents",
                                        agent, "secret"), NULL);

  if(s == NULL || strcmp(s, secret))
    return 403;

  htsbuf_qprintf(&hc->hc_reply, "welcome\n");
  http_output_content(hc, "text/plain; charset=utf-8");
  return 200;
}



static void
buildmaster_check_expired_builds(conn_t *c)
{
  int timeout;
  int maxattempts;

  if(db_begin(c))
    return;

  cfg_root(root);
  timeout     = cfg_get_int(root, CFG("buildmaster", "buildtimeout"), 300);
  maxattempts = cfg_get_int(root, CFG("buildmaster", "buildattempts"), 3);

  MYSQL_STMT *s = db_stmt_get(c, SQL_GET_EXPIRED_BUILDS);

  if(db_stmt_exec(s, "i", timeout))
    return;

  int do_commit = 0;

  while(1) {
    int id;
    char project[64];
    char revision[64];
    char agent[64];
    int attempts;

    int r = db_stream_row(DB_STORE_RESULT, s,
                          DB_RESULT_INT(id),
                          DB_RESULT_STRING(project),
                          DB_RESULT_STRING(revision),
                          DB_RESULT_STRING(agent),
                          DB_RESULT_INT(attempts));

    if(r)
      break;
    const char *newstatus;

    project_t *p = project_get(project);
    plog(p, "build/status",
         "Build #%d: Agent %s did not report back for attempt %d of '%s'",
         id, agent, attempts, project);

    if(attempts >= maxattempts) {
      newstatus = "too_many_attempts";

      project_cfg(pc, project);
      const char *url = build_url(pc, id) ?: "";

      plog(p, "build/finalstatus",
           COLOR_RED "Build #%d too many build attempts failed. Giving up. %s",
           id, url);
    } else {
      newstatus = "pending";
    }
    if(!db_stmt_exec(db_stmt_get(c, SQL_RESTART_BUILD), "si", newstatus, id)) {
      do_commit = 1;
    }
  }

  if(do_commit)
    db_commit(c);
  else
    db_rollback(c);
}


/**
 *
 */
static void *
buildmaster_periodic(void *aux)
{
  while(1) {
    conn_t *c = db_get_conn();
    if(c != NULL)
      buildmaster_check_expired_builds(c);
    sleep(60);
  }
  return NULL;
}


/**
 *
 */
int
buildmaster_add_build(const char *project, const char *branch,
                      const char *target,  const char *reason,
                      void (*msg)(void *opaque, const char *fmt, ...),
                      void *opaque)
{
  project_t *p = project_get(project);
  if(p == NULL) {
    msg(opaque, "No such project: %s", project);
    return 1;
  }

  struct ref_list refs;
  git_repo_list_branches(p, &refs);
  ref_t *r;
  LIST_FOREACH(r, &refs, link) {
    if(!strcmp(branch, r->name))
      break;
  }

  if(r == NULL) {
    msg(opaque, "No such branch");
    goto bad;
  }


  if(add_build(p, branch, r->oidtxt, target, reason, 0)) {
    msg(opaque, "Failed to enqueue build");
    goto bad;
  }
  git_repo_free_refs(&refs);
  return 0;

 bad:
  git_repo_free_refs(&refs);
  return 1;
}

/**
 *
 */
void
buildmaster_init(void)
{
  pthread_t tid;
  pthread_create(&tid, NULL, buildmaster_periodic, NULL);
  http_path_add("/buildmaster/getjob",   NULL, http_getjob);
  http_path_add("/buildmaster/artifact", NULL, http_artifact);
  http_path_add("/buildmaster/report",   NULL, http_report);
  http_path_add("/buildmaster/hello",    NULL, http_hello);
}


static int
buildmaster_cli_build(const char *user,
                      int argc, char **argv, int *intv,
                      void (*msg)(void *opaque, const char *fmt, ...),
                      void *opaque)
{
  char reason[256];
  snprintf(reason, sizeof(reason), "Requested by %s", user);
  return buildmaster_add_build(argv[0], argv[1], argv[2],
                               reason, msg, opaque);
}

CMD(buildmaster_cli_build,
    CMD_LITERAL("build"),
    CMD_VARSTR("project"),
    CMD_VARSTR("branch | revision"),
    CMD_VARSTR("target"));

static int
buildmaster_status(const char *user,
                      int argc, char **argv, int *intv,
                      void (*msg)(void *opaque, const char *fmt, ...),
                      void *opaque)
{
  msg(opaque, "The status is awesome");
  return 0;
}
CMD(buildmaster_status,
    CMD_LITERAL("show"),
    CMD_LITERAL("status"));
