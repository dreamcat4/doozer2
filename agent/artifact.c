#include <sys/mman.h>
#include <sys/param.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>

#include <zlib.h>
#include <curl/curl.h>
#include <openssl/sha.h>
#include <openssl/md5.h>

#include "libsvc/htsmsg.h"
#include "libsvc/misc.h"
#include "libsvc/trace.h"
#include "libsvc/htsbuf.h"
#include "libsvc/curlhelpers.h"

#include "job.h"
#include "artifact.h"

TAILQ_HEAD(artifact_queue, artifact);

static pthread_mutex_t artifact_mutex;

static struct artifact_queue artifact_xfers;
static pthread_cond_t artifact_xfers_cond;


/**
 *
 */
static void *
artifact_process_thread(void *aux)
{
  artifact_t *a = aux;
  const job_t *j = a->job;

  uint8_t sha1_digest[20];
  uint8_t md5_digest[16];

  SHA1(a->data, a->size, sha1_digest);
  bin2hex(a->sha1txt, sizeof(a->sha1txt), sha1_digest, sizeof(sha1_digest));

  MD5(a->data,  a->size, md5_digest);
  bin2hex(a->md5txt,  sizeof(a->md5txt),  md5_digest,  sizeof(md5_digest));

  trace(LOG_INFO, "Project: %s (%s): Artifact %s SHA1:%s MD5:%s",
        j->project ?: "<Unknown project>",
        j->version ?: "<Unknown version>",
        a->filename,
        a->sha1txt,
        a->md5txt);



  if(a->gzip) {
    void *out = mmap(NULL, a->size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);

    if(out == MAP_FAILED) {
      a->gzip = 0;
      goto enq;
    }

    z_stream z;
    memset(&z, 0, sizeof(z));
    deflateInit2(&z, 9, Z_DEFLATED, 16+MAX_WBITS, 8, Z_DEFAULT_STRATEGY);

    z.next_in  = a->data;
    z.avail_in = a->size;
    z.next_out = out;
    z.avail_out = a->size;

    int r = deflate(&z, Z_FINISH);
    if(r < 0) {
      printf("Failed to compress: %d\n", r);
      munmap(out, a->mapsize);
      a->gzip = 0;
      goto enq;
    }

    a->size = a->mapsize - z.avail_out;

    trace(LOG_INFO,
          "Project: %s (%s): Artifact %s compressed from %zd to %zd bytes",
          j->project ?: "<Unknown project>",
          j->version ?: "<Unknown version>",
          a->filename,
          a->mapsize,
          a->size);

    munmap(a->data, a->mapsize);
    a->data = out;
  }
  enq:

  pthread_mutex_lock(&artifact_mutex);
  TAILQ_INSERT_TAIL(&artifact_xfers, a, xfer_link);
  pthread_cond_signal(&artifact_xfers_cond);
  pthread_mutex_unlock(&artifact_mutex);
  return NULL;
}


/**
 *
 */
static void
artifact_add(job_t *j, const char *type, const char *filename,
             const char *content_type, void *ptr, size_t size,
             int gzip)
{
  artifact_t *a = calloc(1, sizeof(artifact_t));
  a->size = size;
  a->mapsize = size;
  a->data = ptr;
  a->job = j;
  a->type = strdup(type);
  a->filename = strdup(filename);
  a->gzip = gzip;

  trace(LOG_INFO, "Project: %s (%s): Artifact %s (%d bytes) added to queue",
        j->project ?: "<Unknown project>",
        j->version ?: "<Unknown version>",
        a->filename, (int)size);

  a->content_type = strdup(content_type ?: "text/plain; charset=utf-8");
  a->result = -1;

  pthread_mutex_lock(&artifact_mutex);
  LIST_INSERT_HEAD(&j->artifacts, a, link);
  pthread_mutex_unlock(&artifact_mutex);

  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&tid, &attr, artifact_process_thread, a);
}


/**
 *
 */
int
artifact_add_file(job_t *j, const char *type, const char *filename,
                  const char *content_type, const char *path,
                  int gzip, char *errbuf, size_t errlen)
{
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if(fd == -1) {
    snprintf(errbuf, errlen, "Unable to open %s -- %s",
             path, strerror(errno));
    return -1;
  }
  struct stat st;
  if(fstat(fd, &st)) {
    snprintf(errbuf, errlen, "Unable to stat %s -- %s",
             path, strerror(errno));
    close(fd);
    return -1;
  }


  void *mem = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if(mem == MAP_FAILED) {
    snprintf(errbuf, errlen, "Unable to mmap %s", path);
    return -1;
  }
  artifact_add(j, type, filename, content_type, mem, st.st_size, gzip);
  return 0;
}


/**
 *
 */
int
artifact_add_htsbuf(job_t *j, const char *type, const char *filename,
                    const char *content_type, struct htsbuf_queue *q,
                    int gzip)
{
  size_t size = q->hq_size;
  void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
  if(mem == MAP_FAILED)
    return -1;

  htsbuf_read(q, mem, size);
  assert(q->hq_size == 0);

  artifact_add(j, type, filename, content_type, mem, size, gzip);
  return 0;
}


/**
 *
 */
static size_t
read_artifact(void *ptr, size_t size, size_t nmemb, void *userdata)
{
  ssize_t len = size * nmemb;
  artifact_t *a = userdata;

  len = MIN(len, a->size - a->fpos);
  if(len < 0)
    return 0;
  memcpy(ptr, a->data + a->fpos, len);
  a->fpos += len;
  return len;
}


/**
 *
 */
static int
seek_artifact(void *instream, curl_off_t offset, int origin)
{
  artifact_t *a = instream;
  off_t new_fpos;
  switch(origin) {
  case SEEK_SET:
    new_fpos = offset;
    break;
  case SEEK_CUR:
    new_fpos = a->fpos + offset;
    break;
  case SEEK_END:
    new_fpos = a->size + offset;
    break;
  default:
    return -1;
  }

  if(new_fpos < 0)
    return -1;

  a->fpos = MIN(a->size, new_fpos);
  return a->fpos;
}


/**
 *
 */
static int
progress_fn(void *clientp, double dltotal, double dlnow,
            double ultotal, double ulnow)
{
  artifact_t *a = clientp;
  return a->do_abort;
}


/**
 *
 */
static int
artifact_send(artifact_t *a)
{
  char url[2048];
  const job_t *j = a->job;
  const buildmaster_t *bm = j->bm;

  snprintf(url, sizeof(url),
           "%s/buildmaster/artifact"
           "?jobid=%d"
           "&jobsecret=%s"
           "&name=%s"
           "&type=%s"
           "&md5sum=%s"
           "&sha1sum=%s"
           ,
           bm->url,
           j->jobid,
           j->jobsecret,
           a->filename,
           a->type,
           a->md5txt,
           a->sha1txt);

  char ctbuf[256];
  snprintf(ctbuf, sizeof(ctbuf), "Content-Type: %s", a->content_type);
  struct curl_slist *slist = curl_slist_append(NULL, ctbuf);

  if(a->gzip)
    curl_slist_append(slist, "Content-Encoding: gzip");

  CURL *curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERNAME, bm->agentid);
  curl_easy_setopt(curl, CURLOPT_PASSWORD, bm->secret);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &libsvc_curl_waste_output);

  curl_easy_setopt(curl, CURLOPT_READDATA, (void *)a);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, &read_artifact);

  curl_easy_setopt(curl, CURLOPT_SEEKDATA, (void *)a);
  curl_easy_setopt(curl, CURLOPT_SEEKFUNCTION, &seek_artifact);

  curl_easy_setopt(curl, CURLOPT_PUT, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)a->size);

  curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, &progress_fn);
  curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, a);

  curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, &libsvc_curl_sock_fn);
  curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, NULL);

  trace(LOG_INFO, "Project: %s (%s): Artifact %s about to upload to %s",
        j->project ?: "<Unknown project>",
        j->version ?: "<Unknown version>",
        a->filename,
        url);

  CURLcode result = curl_easy_perform(curl);
  curl_slist_free_all(slist);


  if(result == CURLE_HTTP_RETURNED_ERROR) {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    snprintf(a->errbuf, sizeof(a->errbuf), "HTTP Error %d", (int)http_code);

  } else if(result) {
    snprintf(a->errbuf, sizeof(a->errbuf), "CURL failed: %s",
             curl_easy_strerror(result));
  }

  trace(LOG_INFO, "Project: %s (%s): Artifact %s uploaded: %s",
        j->project ?: "<Unknown project>",
        j->version ?: "<Unknown version>",
        a->filename,
        a->errbuf[0] ? a->errbuf : "OK");

  munmap(a->data, a->mapsize);
  a->data = NULL;

  curl_easy_cleanup(curl);
  return result;
}


/**
 *
 */
static void *
artifact_xfer_thread(void *aux)
{
  pthread_mutex_lock(&artifact_mutex);
  artifact_t *a;
  while(1) {
    if((a = TAILQ_FIRST(&artifact_xfers)) == NULL) {
      pthread_cond_wait(&artifact_xfers_cond, &artifact_mutex);
      continue;
    }
    TAILQ_REMOVE(&artifact_xfers, a, xfer_link);
    a->result = -2;
    pthread_mutex_unlock(&artifact_mutex);
    int r = artifact_send(a);
    pthread_mutex_lock(&artifact_mutex);

    a->result = r;
    pthread_cond_signal(&a->job->artifact_cond);
  }
  return NULL;
}


/**
 *
 */
void
artifact_init(void)
{
  int i;
  pthread_t tid;

  pthread_mutex_init(&artifact_mutex, NULL);
  pthread_cond_init(&artifact_xfers_cond, NULL);
  TAILQ_INIT(&artifact_xfers);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  for(i = 0; i < 2; i++)
    pthread_create(&tid, &attr, artifact_xfer_thread, NULL);

  pthread_attr_destroy(&attr);
}


/**
 *
 */
static void
artifact_free(artifact_t *a)
{
  LIST_REMOVE(a, link);

  if(a->data != NULL)
    munmap(a->data, a->mapsize);

  free(a->type);
  free(a->filename);
  free(a->content_type);
  free(a);
}


/**
 *
 */
static void
artifacts_abort(job_t *j)
{
  artifact_t *a, *next;

  while(1) {

    for(a = LIST_FIRST(&j->artifacts); a != NULL; a = next) {
      next = LIST_NEXT(a, link);

      if(a->result == -2) {
        // Transfer in progress
        a->do_abort = 1;
        continue;
      }

      if(a->result == -1)
        TAILQ_REMOVE(&artifact_xfers, a, xfer_link);

      artifact_free(a);
    }

    if(a == NULL)
      return;
    pthread_cond_wait(&j->artifact_cond, &artifact_mutex);
  }
}


/**
 *
 */
static void
artifacts_cleanup(job_t *j)
{
  artifact_t *a;

  while((a = LIST_FIRST(&j->artifacts)) != NULL) {
    assert(a->result >= 0);
    artifact_free(a);
  }
}


/**
 *
 */
int
aritfacts_wait(job_t *j)
{
  artifact_t *a;

  pthread_mutex_lock(&artifact_mutex);

  while(1) {
    LIST_FOREACH(a, &j->artifacts, link)
      if(a->result > 0)
        break;

    if(a != NULL) {
      job_report_fail(j, "Unable to upload %s -- %s", a->filename, a->errbuf);
      artifacts_abort(j);
      pthread_mutex_unlock(&artifact_mutex);
      return 1;
    }

    int waitcnt = 0;
    LIST_FOREACH(a, &j->artifacts, link)
      if(a->result < 0)
        waitcnt++;

    if(waitcnt == 0)
      break;

    job_report_status(j, "building", "Waiting for %d artifacts to upload",
                      waitcnt);
    pthread_cond_wait(&j->artifact_cond, &artifact_mutex);
  }
  artifacts_cleanup(j);

  pthread_mutex_unlock(&artifact_mutex);
  return 0;
}
