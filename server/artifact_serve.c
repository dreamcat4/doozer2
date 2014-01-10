#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/sendfile.h>
#include <sys/mman.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <zlib.h>

#include <openssl/hmac.h>

#include "libsvc/tcp.h"
#include "libsvc/http.h"
#include "libsvc/misc.h"
#include "libsvc/trace.h"
#include "libsvc/cfg.h"
#include "libsvc/db.h"

#include "artifact_serve.h"
#include "doozer.h"
#include "bsdiff.h"
#include "project.h"
#include "sql_statements.h"

static pthread_mutex_t patch_mutex = PTHREAD_MUTEX_INITIALIZER;

#define TMPMEMSIZE (1024 * 1024 * 1024)

/**
 *
 */
static void *
load_fd(int fd, int insize, size_t *outsize, int gzipped)
{
  void *in = malloc(insize);

  if(in == NULL) {
    close(fd);
    return NULL;
  }

  if(read(fd, in, insize) != insize) {
    free(in);
    close(fd);
    return NULL;
  }
  close(fd);


  if(!gzipped) {
    *outsize = insize;
    return in;
  }

  void *out = mmap(NULL, TMPMEMSIZE,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);

  if(out == MAP_FAILED) {
    free(in);
    return NULL;
  }

  z_stream z;
  memset(&z, 0, sizeof(z));
  inflateInit2(&z, 16+MAX_WBITS);

  z.next_in  = in;
  z.avail_in = insize;
  z.next_out = out;
  z.avail_out = TMPMEMSIZE;

  int r = inflate(&z, Z_FINISH);
  free(in);
  inflateEnd(&z);
  if(r < 0) {
    munmap(out, TMPMEMSIZE);
    return NULL;
  }

  *outsize = TMPMEMSIZE - z.avail_out;

  void *ret = malloc(*outsize);
  memcpy(ret, out, *outsize);
  munmap(out, TMPMEMSIZE);
  return ret;
}


/**
 *
 */
static void *
load_file(const char *path, size_t *outsize, int gzipped)
{
  int fd = open(path, O_RDONLY);
  if(fd == -1)
    return NULL;

  struct stat st;
  if(fstat(fd, &st)) {
    int r = errno;
    close(fd);
    errno = r;
    return NULL;
  }
  return load_fd(fd, st.st_size, outsize, gzipped);
}


/**
 *
 */
static int
do_send_file(http_connection_t *hc, const char *ct,
             int content_len, const char *ce, int fd)
{

  http_send_header(hc, HTTP_STATUS_OK, ct, content_len, ce,
                   NULL, 0, NULL, NULL, NULL);

  if(!hc->hc_no_output) {
    if(tcp_sendfile(hc->hc_ts, fd, content_len)) {
      return -1;
    }
  }

  close(fd);
  return 0;
}


/**
 *
 */
static int
send_patch(http_connection_t *hc, const char *oldsha1, const char *newsha1,
           const char *newpath, const char *newencoding,
           conn_t *c, const char *basepath)
{
  if(newencoding != NULL && strcmp(newencoding, "gzip"))
    return 1;

  int err;
  char patchfile[PATH_MAX];
  char ce[256];
  const char *ct = "binary/bsdiff";
  cfg_root(root);

  snprintf(ce, sizeof(ce), "bspatch-from-%s", oldsha1);

  const char *patchstash = cfg_get_str(root, CFG("patchstash"),
                                       "/var/tmp/doozer/patchstash");

  if((err = makedirs(patchstash)) != 0) {
    trace(LOG_ERR, "Unable to create patchstash directory %s -- %s",
          patchstash, strerror(errno));
    return 1;
  }

  snprintf(patchfile, sizeof(patchfile), "%s/%s-%s", patchstash,
           oldsha1, newsha1);

  pthread_mutex_lock(&patch_mutex);

  int fd = open(patchfile, O_RDONLY);
  if(fd == -1) {

    // Make sure ''old'' file can be resolved before
    // we do anything else

    MYSQL_STMT *s = db_stmt_get(c, SQL_GET_ARTIFACT_BY_SHA1);

    if(db_stmt_exec(s, "s", oldsha1)) {
      pthread_mutex_unlock(&patch_mutex);
      return 1;
    }

    char storage[32];
    char payload[20000];
    char project[128];
    char name[256];
    char type[128];
    char content_type[128];
    char content_encoding[128];
    int r = db_stream_row(0, s,
                          DB_RESULT_STRING(storage),
                          DB_RESULT_STRING(payload),
                          DB_RESULT_STRING(project),
                          DB_RESULT_STRING(name),
                          DB_RESULT_STRING(type),
                          DB_RESULT_STRING(content_type),
                          DB_RESULT_STRING(content_encoding),
                          NULL);

    mysql_stmt_reset(s);

    if(r) {
      pthread_mutex_unlock(&patch_mutex);
      trace(LOG_DEBUG, "Unable to patch from unknown SHA-1 %s", oldsha1);
      return 1;
    }

    char oldpath[PATH_MAX];
    snprintf(oldpath, sizeof(oldpath), "%s/%s", basepath, payload);

    trace(LOG_INFO, "Generating new patch between %s (%s) => %s (%s)",
          oldsha1, oldpath, newsha1, newpath);

    size_t newsize, oldsize;

    void *new = load_file(newpath, &newsize, !strcmp(newencoding ?: "", "gzip"));
    if(new == NULL) {
      trace(LOG_ERR, "Unable to open file %s for patch creation -- %s",
            newpath, strerror(errno));
      pthread_mutex_unlock(&patch_mutex);
      return 1;
    }

    void *old = load_file(oldpath, &oldsize, !strcmp(content_encoding, "gzip"));
    if(old == NULL) {
      trace(LOG_ERR, "Unable to open file %s for patch creation -- %s",
            oldpath, strerror(errno));
      free(new);
      pthread_mutex_unlock(&patch_mutex);
      return 1;
    }

    int rval = make_bsdiff(old, oldsize, new, newsize, patchfile);

    trace(LOG_INFO, "Generated patch between %s (%s) => %s (%s) -- error: %d",
          oldsha1, oldpath, newsha1, newpath, rval);

    free(new);
    free(old);

    if(rval) {
      trace(LOG_ERR, "Unable to generate patch file %s", patchfile);
      pthread_mutex_unlock(&patch_mutex);
      return 1;
    }
    fd = open(patchfile, O_RDONLY);
    if(fd == -1) {
      trace(LOG_ERR, "Unable to open generated patch file %s -- %s",
            patchfile, strerror(errno));
      pthread_mutex_unlock(&patch_mutex);
      return 1;
    }
  }

  pthread_mutex_unlock(&patch_mutex);

  struct stat st;
  if(fstat(fd, &st)) {
    trace(LOG_INFO,
          "Stat failed for file '%s' -- %s",
          patchfile, strerror(errno));
    close(fd);
    return 1;
  }

  return do_send_file(hc, ct, st.st_size, ce, fd);
}


/**
 *
 */
static int
send_artifact(http_connection_t *hc, const char *remain, void *opaque)
{
  if(remain == NULL)
    return 404;

  conn_t *c = db_get_conn();
  if(c == NULL)
    return 500;

  MYSQL_STMT *s = db_stmt_get(c, SQL_GET_ARTIFACT_BY_SHA1);

  if(db_stmt_exec(s, "s", remain))
    return 500;

  char storage[32];
  char payload[20000];
  char project[128];
  char name[256];
  char type[128];
  char content_type[128];
  char content_encoding[128];
  int r = db_stream_row(0, s,
                        DB_RESULT_STRING(storage),
                        DB_RESULT_STRING(payload),
                        DB_RESULT_STRING(project),
                        DB_RESULT_STRING(name),
                        DB_RESULT_STRING(type),
                        DB_RESULT_STRING(content_type),
                        DB_RESULT_STRING(content_encoding),
                        NULL);

  mysql_stmt_reset(s);

  switch(r) {
  case DB_ERR_OTHER:
    return 500;
  case DB_ERR_NO_DATA:
    return 404;
  }

  const char *ct = content_type[0] ? content_type : "text/plain; charset=utf-8";
  const char *ce = content_encoding[0] ? content_encoding : NULL;

  if(strncmp(ct, "text/plain", strlen("text/plain"))) {
    char disp[256];
    snprintf(disp, sizeof(disp), "attachment; filename=%s", name);
    http_arg_set(&hc->hc_response_headers, "Content-Disposition",
                 disp);
  }

  if(!strcmp(storage, "embedded")) {
    // Easy one
    htsbuf_append(&hc->hc_reply, payload, strlen(payload));
    http_output_content(hc, ct);

  } else if(!strcmp(storage, "file")) {

    project_cfg(pc, project);
    if(pc == NULL)
      return 404;

    char path[PATH_MAX];
    const char *basepath = cfg_get_str(pc, CFG("artifactPath"), NULL);
    if(basepath == NULL) {
      trace(LOG_INFO,
            "Missing artifactPath for project %s -- unable to locate artifacts",
            project);
      return 500;
    }

    snprintf(path, sizeof(path), "%s/%s", basepath, payload);


    char *encodings[16];
    int nencodings = 0;
    char *ae = http_arg_get(&hc->hc_args, "Accept-Encoding");

    if(ae != NULL) {
      nencodings = str_tokenize(ae, encodings, 16, ',');
      for(int i = 0; i < nencodings; i++) {

        char *x = strchr(encodings[i], ';');
        if(x != NULL)
          *x = 0;
      }

      const char *src;
      if((src = mystrbegins(ae, "bspatch-from-")) != NULL) {
        switch(send_patch(hc, src, remain, path, ce, c, basepath)) {
        case -1:
          return -1;
        case 0:
          db_stmt_exec(db_stmt_get(c, SQL_INCREASE_PATCHCOUNT_BY_SHA1),
                       "s", remain);
          return 0;
        default:
          break;
        }
      }
    }

    int fd = open(path, O_RDONLY);
    if(fd == -1) {
      trace(LOG_INFO,
            "Missing file '%s' for artifact %s in project %s -- %s",
            path, remain, project, strerror(errno));
      return 404;
    }

    struct stat st;
    if(fstat(fd, &st)) {
      trace(LOG_INFO,
            "Stat failed for file '%s' for artifact %s in project %s -- %s",
            path, remain, project, strerror(errno));
      close(fd);
      return 404;
    }

    int64_t content_len = st.st_size;


    if(ce != NULL) {
      // Content on disk is encoded, need to check if the client accepts that
      // encoding
      for(int i = 0; i < nencodings; i++) {
        if(!strcasecmp(encodings[i], ce)) {
          goto send_file;
        }
      }

      if(!strcasecmp(ce, "gzip")) {
        size_t outsize;
        void *mem = load_fd(fd, st.st_size, &outsize, 1);
        if(mem == NULL)
          return 500;

        htsbuf_append_prealloc(&hc->hc_reply, mem, outsize);
        http_output_content(hc, ct);
        goto count;
      }
    }

  send_file:
    if(do_send_file(hc, ct, content_len, ce, fd))
      return -1;

  } else if(!strcmp(storage, "s3")) {

    project_cfg(p, project);
    if(p == NULL)
      return 404;

    char sigstr[512];
    uint8_t md[20];
    char b64[100];
    char sig[100];
    char location[512];
    time_t expire = time(NULL) + 60;

    const char *bucket = cfg_get_str(p, CFG("s3", "bucket"), NULL);
    const char *secret = cfg_get_str(p, CFG("s3", "secret"), NULL);
    const char *awsid  = cfg_get_str(p, CFG("s3", "awsid"),  NULL);


    if(bucket == NULL || secret == NULL || awsid == NULL) {
      trace(LOG_INFO,
	    "Missing S3 config for project '%s'. Unable to serve files", project);
      return 412;
    }

    snprintf(sigstr, sizeof(sigstr), "GET\n\n\n%ld\n/%s/%s",
             expire, bucket, payload);
    HMAC(EVP_sha1(), secret, strlen(secret), (void *)sigstr,
         strlen(sigstr), md, NULL);
    base64_encode(b64, sizeof(b64), md, sizeof(md));
    url_escape(sig, sizeof(sig), b64, URL_ESCAPE_PATH);
    snprintf(location, sizeof(location),
             "https://%s.s3.amazonaws.com/%s?Signature=%s&Expires=%ld"
             "&AWSAccessKeyId=%s",
             bucket, payload, sig, expire, awsid);
    http_redirect(hc, location, HTTP_STATUS_FOUND);


  } else {
    return 501;
  }
 count:
  db_stmt_exec(db_stmt_get(c, SQL_INCREASE_DLCOUNT_BY_SHA1), "s", remain);
  return 0;
}


/**
 *
 */
void
artifact_serve_init(void)
{
  http_path_add("/file",  NULL, send_artifact);
}
