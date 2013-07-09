#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>

#include "doozer.h"
#include "db.h"
#include "cfg.h"

static pthread_key_t dbkey;

/**
 *
 */
static MYSQL_STMT *
prep_stmt(MYSQL *m, const char *str)
{
  MYSQL_STMT *ms = mysql_stmt_init(m);
  if(mysql_stmt_prepare(ms, str, strlen(str))) {
    trace(LOG_ERR, "Unable to prepare statement '%s' -- %s",
          str, mysql_error(m));
    return NULL;
  }
  return ms;
}


/**
 *
 */
MYSQL_STMT *
db_stmt_prep(const char *sql)
{
  conn_t *c = db_get_conn();
  if(c == NULL)
    return NULL;
  return prep_stmt(c->m, sql);
}


/**
 *
 */
void
db_stmt_cleanup(MYSQL_STMT **ptr)
{
  if(*ptr)
    mysql_stmt_close(*ptr);
}


/**
 *
 */
conn_t *
db_get_conn(void)
{
  conn_t *c = pthread_getspecific(dbkey);
  if(c == NULL) {
    mysql_thread_init();

    MYSQL *m = mysql_init(NULL);

    cfg_root(cfg);

    const char *username =
      cfg_get_str(cfg, CFG("buildmaster", "db", "username"), NULL);
    const char *password =
      cfg_get_str(cfg, CFG("buildmaster", "db", "password"), NULL);
    const char *database =
      cfg_get_str(cfg, CFG("buildmaster", "db", "database"), NULL);

    if(mysql_real_connect(m, "localhost", username,
                          password, database, 0, NULL, 0) != m) {
      trace(LOG_ERR, "Failed to connect: Error: %s\n", mysql_error(m));
      mysql_close(m);
      return NULL;
    }

    c = calloc(1, sizeof(conn_t));
    c->m = m;
    c->get_artifact          = prep_stmt(m, SQL_GET_ARTIFACT_BY_ID);
    c->incr_dlcount          = prep_stmt(m, SQL_INCREASE_DLCOUNT);
    c->get_targets_for_build = prep_stmt(m, SQL_GET_TARGETS_FOR_BUILD);
    c->insert_build          = prep_stmt(m, SQL_INSERT_BUILD);
    c->alloc_build           = prep_stmt(m, SQL_ALLOC_BUILD);
    c->get_build_by_id       = prep_stmt(m, SQL_GET_BUILD_BY_ID);
    c->insert_artifact       = prep_stmt(m, SQL_INSERT_ARTIFACT);
    c->build_finished        = prep_stmt(m, SQL_BUILD_FINISHED);
    c->build_progress_update = prep_stmt(m, SQL_BUILD_PROGRESS_UPDATE);
    c->get_expired_builds    = prep_stmt(m, SQL_GET_EXPIRED_BUILDS);
    c->restart_build         = prep_stmt(m, SQL_RESTART_BUILD);
    c->get_releases          = prep_stmt(m, SQL_GET_RELEASES);
    c->get_artifacts         = prep_stmt(m, SQL_GET_ARTIFACTS);
    pthread_setspecific(dbkey, c);
  }
  return c;
}


/**
 *
 */
static void
db_cleanup(void *aux)
{
  conn_t *c = aux;
  mysql_close(c->m);
  free(c);
}



/**
 *
 */
void
db_init(void)
{
  pthread_key_create(&dbkey, db_cleanup);
}



int
db_stmt_exec(MYSQL_STMT *s, const char *fmt, ...)
{
  int p, args = strlen(fmt);
  int *x;

  if(mysql_stmt_param_count(s) != args)
    return -1;

  MYSQL_BIND in[args];
  memset(in, 0, sizeof(MYSQL_BIND) * args);
  va_list ap;
  va_start(ap, fmt);

  for(p = 0; *fmt; p++, fmt++) {

    switch(*fmt) {
    case 'i':
      x = alloca(sizeof(int));
      *x = va_arg(ap, int);
      in[p].buffer_type = MYSQL_TYPE_LONG;
      in[p].buffer = (char *)x;
      break;

    case 's':
      in[p].buffer = va_arg(ap, char *);
      if(in[p].buffer != NULL) {
        in[p].buffer_type = MYSQL_TYPE_STRING;
        in[p].buffer_length = strlen(in[p].buffer);
      } else {
        in[p].buffer_type = MYSQL_TYPE_NULL;
      }
      break;

    case 'b':
      in[p].buffer = va_arg(ap, char *);
      in[p].buffer_length = va_arg(ap, int);
      in[p].buffer_type = MYSQL_TYPE_STRING;
      break;

    default:
      abort();
    }
  }

  if(mysql_stmt_bind_param(s, in)) {
    trace(LOG_ERR, "Failed to bind parameters to prepared statement %s -- %s",
          mysql_stmt_sqlstate(s), mysql_stmt_error(s));
    return -1;
  }

  if(mysql_stmt_execute(s)) {
    trace(LOG_ERR, "Failed to execute prepared statement %s -- %s",
          mysql_stmt_sqlstate(s), mysql_stmt_error(s));
    return -1;
  }
  return 0;
}


/**
 *
 */
int
db_stream_row(int flags, MYSQL_STMT *s, ...)
{
  int fields = mysql_stmt_field_count(s);

  MYSQL_BIND out[fields];
  unsigned long lens[fields];
  int type;
  int p = 0, i;

  memset(out, 0, sizeof(MYSQL_BIND) * fields);
  memset(lens, 0, sizeof(unsigned long) * fields);

  va_list ap;
  va_start(ap, s);

  while((type = va_arg(ap, int)) != 0 && p < fields) {
    switch(type) {
    case DB_RESULT_TAG_STR:
      out[p].buffer_type = MYSQL_TYPE_STRING;
      out[p].buffer = va_arg(ap, char *);
      out[p].buffer_length = va_arg(ap, int) - 1;
      out[p].length = &lens[p];
      break;

    case DB_RESULT_TAG_INT:
      out[p].buffer_type = MYSQL_TYPE_LONG;
      out[p].buffer = va_arg(ap, int *);
      out[p].buffer_length = sizeof(int);
      out[p].length = &lens[p];
      break;

    default:
      abort();
    }
    p++;
  }

  if(fields != p) {
    trace(LOG_ERR, "Bind invalid number of arguments for %s -- %d vs %d",
          mysql_stmt_sqlstate(s), mysql_stmt_field_count(s), p);
    return -1;
  }

  if(mysql_stmt_bind_result(s, out)) {
    trace(LOG_ERR, "Bind failed for statement %s -- %s",
          mysql_stmt_sqlstate(s), mysql_stmt_error(s));
    return -1;
  }

  if(flags & DB_STORE_RESULT)
    mysql_stmt_store_result(s);

  switch(mysql_stmt_fetch(s)) {
  case 0:
    for(i = 0; i < p; i++) {
      if(out[i].buffer_type == MYSQL_TYPE_STRING)
        ((char *)out[i].buffer)[lens[i]] = 0;
    }
    return 0;

  case MYSQL_NO_DATA:
    return 1;

  case MYSQL_DATA_TRUNCATED:
    trace(LOG_ERR, "Data truncated for %s",
          mysql_stmt_sqlstate(s));
    return -1;

  default:
    trace(LOG_ERR, "Bind failed for statement %s -- %s",
          mysql_stmt_sqlstate(s), mysql_stmt_error(s));
    return -1;
  }
}


/**
 *
 */
int
db_begin(conn_t *c)
{
  int r = mysql_query(c->m, "START TRANSACTION");
  if(!r)
    return 0;
  trace(LOG_ERR, "Unable to start transaction -- %s",
        mysql_error(c->m));
  return -1;
}


/**
 *
 */
int
db_commit(conn_t *c)
{
  if(mysql_commit(c->m))
    trace(LOG_ERR, "Unable to commit transaction -- %s",
          mysql_error(c->m));
  return 0;
}


/**
 *
 */
int
db_rollback(conn_t *c)
{
  if(mysql_rollback(c->m))
    trace(LOG_ERR, "Unable to rollback transaction -- %s",
          mysql_error(c->m));
  return 0;
}
