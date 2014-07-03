#pragma once

#include "project.h"

LIST_HEAD(ref_list, ref);
TAILQ_HEAD(change_queue, change);

typedef struct change {
  TAILQ_ENTRY(change) link;
  char version[64];
  char *msg;
  char *tag;
  git_oid oid;
} change_t;

typedef struct ref {
  LIST_ENTRY(ref) link;
  char *name;
  git_oid oid;
  char oidtxt[41];
} ref_t;

int git_repo_sync(project_t *p);

int git_repo_list_branches(project_t *p, struct ref_list *rl);

int git_repo_list_tags(project_t *p, struct ref_list *rl);

void git_repo_free_refs(struct ref_list *rl);

int git_describe(char *out, size_t outlen, project_t *p, const char *rev,
                 int with_hash);

int git_changelog(struct change_queue *cq, project_t *p, const git_oid *rev,
                  int offset, int count, int all, const char *target);

void git_changlog_free(struct change_queue *cq);

int git_get_file(project_t *p, const git_oid *oid,
                 const char *path, void **datap, size_t *sizep,
                 char *errbuf, size_t errlen);

const char *giterr(void);
