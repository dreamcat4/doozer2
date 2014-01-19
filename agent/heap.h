#pragma once

#include <limits.h>

typedef struct heapmgr {

  void (*dtor)(struct heapmgr *hm);

  int (*open_heap)(struct heapmgr *hm, const char *id,
                   char outpath[PATH_MAX],
                   char *errbuf, size_t errlen, int create);

  int (*delete_heap)(struct heapmgr *hm, const char *name);

} heapmgr_t;

