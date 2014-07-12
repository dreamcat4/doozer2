#pragma once

#include "project.h"

int buildmaster_check_for_builds(project_t *p);

void buildmaster_init(void);

typedef struct buildjob {
  int id;
  char revision[64];
  char target[64];
  char jobsecret[64];
  char project[128];
  char version[64];
  int no_output;
  struct db_conn *db;
} buildjob_t;



// Temporary until real command parser
int
buildmaster_add_build(const char *project, const char *branch,
                      const char *target, const char *buildenv,
                      const char *reason,
                      void (*msg)(void *opaque, const char *fmt, ...),
                      void *opaque);
