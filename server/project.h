#pragma once

#include <git2.h>
#include "libsvc/cfg.h"
#include "doozer.h"

#define PROJECT_JOB_UPDATE_REPO        0x1
#define PROJECT_JOB_CHECK_FOR_BUILDS   0x2
#define PROJECT_JOB_GENERATE_RELEASES  0x4
#define PROJECT_JOB_NOTIFY_REPO_UPDATE 0x8

LIST_HEAD(project_list, project);
LIST_HEAD(pconf_list, pconf);

extern pthread_mutex_t projects_mutex;
extern pthread_cond_t projects_cond;
extern pthread_mutex_t project_cfg_mutex;

extern struct project_list projects;
extern struct pconf_list pconfs;


/**
 *
 */
typedef struct pconf {
  LIST_ENTRY(pconf) pc_link;
  char *pc_id;
  int pc_mark;
  htsmsg_t *pc_msg;
  time_t pc_mtime;  // mtime of last read conf
} pconf_t;


/**
 *
 */
typedef struct project {

  // --------------------------------------------------
  // These fields are projected with the global project_mutex

  LIST_ENTRY(project) p_link;
  char *p_id;

  pthread_t p_thread;

  int p_pending_jobs;
  int p_active_jobs;
  int p_failed_jobs;

  // --------------------------------------------------
  // --------------------------------------------------

  pthread_mutex_t p_repo_mutex;
  git_repository *p_repo;

  // --------------------------------------------------
  // -- GIT Repo refresh time -------------------------

  int p_refresh_interval;
  time_t p_next_refresh;

} project_t;

project_t *project_get(const char *id);

project_t *project_init(const char *id, int forceinit);

void projects_reload(void);

void projects_init(void);

void project_schedule_job(project_t *p, int mask);

#define project_cfg(x, id) cfg_t *x __attribute__((cleanup(cfg_releasep))) = project_get_cfg(id);

cfg_t *project_get_cfg(const char *id);

const char *project_get_artifact_path(const char *id);

/**
 * Project specific log
 *
 *  List of "known" contexts
 *
 *   system                          - Various tech and internal
 *
 *   build/check                     - Buildmaster checking
 *   build/queue                     - Modifications to build queue
 *   build/artifact                  - Reception of artifacts
 *   build/status                    - Modification of build status
 *
 *   release/check                   - Releasemaker check
 *   release/info/<arch>             - JSON manifest updates
 *   release/publish/<arch>          - New relases published in JSON manifest
 *
 *   git/repo                        - Updates to repo
 *   git/branch                      - Changes to branches
 *
 *   github                          - Github API entry
 *
 *   notify                          - Notifications
 *
 *   artifact                        - Various stuff related to artifacts
 *
 */
void plog(project_t *p, const char *context, const char *fmt, ...)
 __attribute__ ((format (printf, 3, 4)));

