#pragma once

#include <mysql.h>

#define DB_STORE_RESULT 0x1

typedef struct conn {
  MYSQL *m;

  MYSQL_STMT *get_artifact_by_sha1;
  MYSQL_STMT *incr_dlcount_by_sha1;
  MYSQL_STMT *get_targets_for_build;
  MYSQL_STMT *insert_build;
  MYSQL_STMT *alloc_build;
  MYSQL_STMT *get_build_by_id;
  MYSQL_STMT *insert_artifact;
  MYSQL_STMT *build_progress_update;
  MYSQL_STMT *build_finished;
  MYSQL_STMT *get_expired_builds;
  MYSQL_STMT *restart_build;
  MYSQL_STMT *get_releases;
  MYSQL_STMT *get_artifacts;

} conn_t;

#define SQL_GET_ARTIFACT_BY_SHA1 "SELECT storage,payload,project,name,artifact.type,contenttype,encoding FROM artifact,build WHERE artifact.sha1=? AND build.id = artifact.build_id"

#define SQL_INCREASE_DLCOUNT_BY_SHA1 "UPDATE artifact SET dlcount = dlcount + 1 WHERE sha1 = ?"

#define SQL_GET_TARGETS_FOR_BUILD "SELECT target,id,status FROM build WHERE revision = ? AND project = ? AND branch = ?"

#define SQL_INSERT_BUILD "INSERT INTO build (project,revision,target,type,status,branch,version,no_output) VALUES (?,?,?,?,?,?,?,?)"

#define SQL_ALLOC_BUILD "UPDATE build SET agent=?, status=?, status_change=NOW(), buildstart=NOW(), attempts = attempts + 1, jobsecret=? WHERE id=?"

#define SQL_GET_BUILD_BY_ID "SELECT project,revision,target,type,agent,jobsecret,status,version FROM build WHERE id=?"

#define SQL_INSERT_ARTIFACT "INSERT INTO artifact (build_id, type, payload, storage, name, size, md5, sha1, contenttype, encoding) VALUES (?,?,?,?,?,?,?,?,?,?)"

#define SQL_BUILD_PROGRESS_UPDATE "UPDATE build SET progress_text=?,status_change=NOW() WHERE id=?"

#define SQL_BUILD_FINISHED "UPDATE build SET status=?, progress_text=?,status_change=NOW(),buildend=NOW() WHERE id=?"

#define SQL_GET_EXPIRED_BUILDS "SELECT id,project,revision,agent,attempts FROM build WHERE status='building' AND TIMESTAMPDIFF(MINUTE, status_change, now()) >= ?"

#define SQL_RESTART_BUILD "UPDATE build SET status=?, status_change=NOW(), jobsecret = NULL WHERE id=?"

#define SQL_GET_RELEASES "SELECT id,branch,target,version,revision FROM build INNER JOIN (SELECT max(id) AS id FROM build WHERE status='done' AND project=? GROUP BY target,branch) latest USING (id)"

#define SQL_GET_ARTIFACTS "SELECT id,type,sha1,size FROM artifact WHERE build_id = ?"

conn_t *db_get_conn(void);

void db_init(void);

#define DB_RESULT_TAG_STR 1
#define DB_RESULT_TAG_INT 2

#define DB_RESULT_STRING(x) DB_RESULT_TAG_STR, x, sizeof(x)
#define DB_RESULT_INT(x)    DB_RESULT_TAG_INT, &x

int db_stmt_exec(MYSQL_STMT *s, const char *fmt, ...);

int db_stream_row(int flags, MYSQL_STMT *s, ...);

#define DB_ERR_NO_DATA 1
#define DB_ERR_OK      0
#define DB_ERR_OTHER  -1

MYSQL_STMT *db_stmt_prep(const char *sql);

void db_stmt_cleanup(MYSQL_STMT **ptr);

#define scoped_db_stmt(x, sql) \
 MYSQL_STMT *x __attribute__((cleanup(db_stmt_cleanup)))=db_stmt_prep(sql);

int db_begin(conn_t *c);

int db_commit(conn_t *c);

int db_rollback(conn_t *c);
