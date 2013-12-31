#pragma once

#include "libsvc/db.h"
#include "project.h"

TAILQ_HEAD(artifact_queue, artifact);
TAILQ_HEAD(build_queue, build);
TAILQ_HEAD(target_queue, target);

typedef struct artifact {
  TAILQ_ENTRY(artifact) a_link;
  int a_id;
  char a_type[64];
  char a_sha1[51];
  char a_name[64];
  int a_size;
} artifact_t;

typedef struct build {
  TAILQ_ENTRY(build) b_global_link;
  TAILQ_ENTRY(build) b_target_link;
  int b_id;
  git_oid b_oid;
  char b_branch[64];
  char b_target[64];
  char b_version[64];
  struct artifact_queue b_artifacts;
} build_t;

typedef struct target {
  TAILQ_ENTRY(target) t_link;
  char t_target[64];
  struct build_queue t_builds;
} target_t;

int releasemaker_update_project(project_t *p);
