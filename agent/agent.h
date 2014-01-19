#pragma once

#include <pwd.h>
#include <grp.h>

extern struct heapmgr *projects_heap_mgr;
extern struct heapmgr *buildenv_heap_mgr;
extern uid_t build_uid;
extern gid_t build_gid;


/**
 *
 */
typedef struct buildmaster {
  const char *url;
  const char *agentid;
  const char *secret;
  const char *last_rpc_error;

  char rpc_errbuf[128];
} buildmaster_t;


void agent_init(void);

char *call_buildmaster(buildmaster_t *bm, const char *path, ...);

