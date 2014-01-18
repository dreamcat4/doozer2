#pragma once

/**
 *
 */
typedef struct buildmaster {
  const char *url;
  const char *agentid;
  const char *secret;
  const char *last_rpc_error;
  const char *workdir;

  char rpc_errbuf[128];
} buildmaster_t;


void agent_init(void);

char *call_buildmaster(buildmaster_t *bm, const char *path, ...);

