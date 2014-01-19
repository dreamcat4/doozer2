#pragma once
#include "job.h"

/**
 *
 */
typedef struct artifact {

  LIST_ENTRY(artifact) link;
  TAILQ_ENTRY(artifact) xfer_link;

  job_t *job;
  char *type;
  char *filename;
  char *content_type;

  void *data;
  size_t size;
  size_t mapsize;

  off_t fpos;

  int result;
  int do_abort;
  int gzip;

  char sha1txt[41];
  char md5txt[33];

  char errbuf[128];

} artifact_t;

int artifact_add_file(job_t *j, const char *type, const char *filename,
                      const char *content_type, const char *path,
                      int gzip, char *errbuf, size_t errlen)
  __attribute__ ((warn_unused_result));

int artifact_add_htsbuf(job_t *j, const char *type, const char *filename,
                        const char *content_type, struct htsbuf_queue *q,
                        int gzip)
  __attribute__ ((warn_unused_result));

void artifact_init(void);

int aritfacts_wait(job_t *j)  __attribute__ ((warn_unused_result));
