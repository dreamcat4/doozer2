#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "libsvc/trace.h"
#include "libsvc/misc.h"

#include "heap.h"
#include "agent.h"

typedef struct heapmgr_simple {
  heapmgr_t super;

  char *path;

} heapmgr_simple_t;


/**
 *
 */
static void
heap_simple_dtor(heapmgr_t *super)
{
  heapmgr_simple_t *hm = (heapmgr_simple_t *)super;
  free(hm->path);
  free(hm);
}




/**
 *
 */
static int
heap_simple_open(struct heapmgr *super, const char *id,
                char outpath[PATH_MAX],
                char *errbuf, size_t errlen, int create)
{
  heapmgr_simple_t *hm = (heapmgr_simple_t *)super;
  snprintf(outpath, PATH_MAX, "%s/%s", hm->path, id);

  struct stat st;
  int r = stat(outpath, &st);
  if(r == 0)
    return 0;

  if(!create) {
    snprintf(errbuf, errlen, "%s does not exist", outpath);
    return -1;
  }

  r = makedirs(outpath);
  if(r) {
    snprintf(errbuf, errlen,
             "Unable to create directory %s -- %s",
             outpath, strerror(r));
    return -1;
  }
  return 0;
}


/**
 *
 */
static int
heap_simple_delete(struct heapmgr *hm, const char *name)
{
  return 0;
}

/**
 *
 */
heapmgr_t *
heap_simple_init(const char *path)
{
  int r = makedirs(path);
  if(r) {
    trace(LOG_WARNING, "heap_simple: Unable create %s -- %s",
          path, strerror(r));
    return NULL;
  }

  if(chown(path, build_uid, build_gid)) {
    trace(LOG_WARNING, "heap_simple: Unable to set uid/gid of %s -- %s",
          path, strerror(errno));
    return NULL;
  }

  heapmgr_simple_t *hm = calloc(1, sizeof(heapmgr_simple_t));

  hm->path = strdup(path);
  hm->super.dtor = heap_simple_dtor;
  hm->super.open_heap = heap_simple_open;
  hm->super.delete_heap = heap_simple_delete;

  return &hm->super;
}

