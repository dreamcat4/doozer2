#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/sendfile.h>

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

#include "net/tcp.h"
#include "net/http.h"

#include "misc/misc.h"

#include "artifact_serve.h"
#include "db.h"
#include "cfg.h"

/**
 *
 */
static int
send_artifact(http_connection_t *hc, const char *remain, void *opaque)
{
  if(remain == NULL)
    return 404;

  int id = atoi(remain);

  conn_t *c = db_get_conn();
  if(c == NULL)
    return 500;

  if(db_stmt_exec(c->get_artifact, "i", id))
    return 500;

  char storage[32];
  char payload[20000];
  char project[128];
  char name[256];
  char type[128];
  char content_type[128];
  char content_encoding[128];
  int r = db_stream_row(0, c->get_artifact,
                        DB_RESULT_STRING(storage),
                        DB_RESULT_STRING(payload),
                        DB_RESULT_STRING(project),
                        DB_RESULT_STRING(name),
                        DB_RESULT_STRING(type),
                        DB_RESULT_STRING(content_type),
                        DB_RESULT_STRING(content_encoding),
                        NULL);

  mysql_stmt_reset(c->get_artifact);

  switch(r) {
  case DB_ERR_OTHER:
    return 500;
  case DB_ERR_NO_DATA:
    return 404;
  }

  const char *ct = content_type[0] ? content_type : "text/plain; charset=utf-8";
  const char *ce = content_encoding[0] ? content_encoding : NULL;

  if(!strcmp(storage, "embedded")) {
    // Easy one
    htsbuf_append(&hc->hc_reply, payload, strlen(payload));
    http_output_content(hc, ct);

  } else if(!strcmp(storage, "file")) {

    cfg_root(cfg);

    cfg_t *pc = cfg_get_project(cfg, project);
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

    int fd = open(path, O_RDONLY);
    if(fd == -1) {
      trace(LOG_INFO,
            "Missing file '%s' for artifact %d in project %s -- %s",
            path, id, project, strerror(errno));
      return 404;
    }

    struct stat st;
    if(fstat(fd, &st)) {
      trace(LOG_INFO,
            "Stat failed for file '%s' for artifact %d in project %s -- %s",
            path, id, project, strerror(errno));
      close(fd);
      return 404;
    }

    int64_t content_len = st.st_size;

    char *encodings[16];
    int nencodings = 0;

    char *ae = http_arg_get(&hc->hc_args, "Accept-Encoding");

    if(ae != NULL)
      nencodings = http_tokenize(ae, encodings, 16, ',');


    if(ce != NULL) {
      // Content on disk is encoded, need to check if the client accepts that
      // encoding
      int i;
      for(i = 0; i < nencodings; i++) {
        if(!strcasecmp(encodings[i], ce)) {
          goto send_file;
        }
      }


      if(!strcasecmp(ce, "gzip")) {
        char zbuf1[4096];
        char zbuf2[32768];
        z_stream z;
        int retval = -1;
        memset(&z, 0, sizeof(z));
        inflateInit2(&z, 16+MAX_WBITS);

        http_send_header(hc, HTTP_STATUS_OK, ct, 0, NULL,
                         NULL, 0, NULL, NULL, NULL);

        do {
          int r = read(fd, zbuf1, sizeof(zbuf1));
          z.next_in = (void *)zbuf1;
          z.avail_in = r >= 0 ? r : 0;
          while(z.avail_in) {

            z.next_out = (void *)zbuf2;
            z.avail_out = sizeof(zbuf2);

            int zr = inflate(&z, r <= 0 ? Z_FINISH : Z_NO_FLUSH);
            if(zr < 0)
              goto bad;

            int b = sizeof(zbuf2) - z.avail_out;
            if(write(hc->hc_fd, zbuf2, b) != b)
              goto bad;
          }
        } while(r);
        retval = 0;
      bad:
        close(fd);
        inflateEnd(&z);
        return retval;
      }
    }


  send_file:
    http_send_header(hc, HTTP_STATUS_OK, ct, content_len, ce,
                     NULL, 0, NULL, NULL, NULL);

    if(!hc->hc_no_output) {
      while(content_len > 0) {
        int chunk = MIN(1024 * 1024 * 1024, content_len);
        r = sendfile(hc->hc_fd, fd, NULL, chunk);
        if(r == -1) {
          close(fd);
          return -1;
        }
        content_len -= r;
      }
    }
    close(fd);
    return 0;

  } else if(!strcmp(storage, "s3")) {

    cfg_root(cfg);

    cfg_t *p = cfg_get_project(cfg, project);
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
    http_redirect(hc, location);


  } else {
    return 501;
  }

  db_stmt_exec(c->incr_dlcount, "i", id);
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
