#include <sys/param.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>

#include "libsvc/htsmsg.h"
#include "libsvc/misc.h"
#include "libsvc/trace.h"
#include "libsvc/htsbuf.h"

#include "job.h"
#include "git.h"
#include "autobuild.h"

/**
 *
 */
static int
autobuild_get_version(job_t *j)
{
  char cmd[PATH_MAX];
  char line[64] = {};

  snprintf(cmd, sizeof(cmd), "%s -v", j->autobuild);
  FILE *f = popen(cmd, "r");
  if(f == NULL) {
    job_report_fail(j, "Failed to execute Autobuild.sh -- %s",
                    strerror(errno));
    return -1;
  }

  if(fgets(line, sizeof(line) - 1, f) == NULL) {
    job_report_fail(j, "Failed read version from Autobuild.sh");
    fclose(f);
    return -1;
  }

  fclose(f);

  j->autobuild_version = atoi(line);
  return 0;
}


/**
 *
 */
void
autobuild_process(job_t *j)
{
  if(autobuild_get_version(j))
    return;

  if(j->autobuild_version != 3) {
    // This is the only version we support right now
    job_report_fail(j, "Unsupported autobuild version %d",
                    j->autobuild_version);
    return;
  }

  htsbuf_queue_t output;
  htsbuf_queue_init2(&output, 100000);

  char errbuf[1024];

  job_run_command(j, j->autobuild,
                  (const char *[]){j->autobuild,
                      "-t", j->target, "-o", "deps", NULL},
                  &output, j->autobuild,
                  errbuf, sizeof(errbuf));

  job_run_command(j, j->autobuild,
                  (const char *[]){j->autobuild,
                      "-t", j->target, "-o", "build", NULL},
                  &output, j->autobuild,
                  errbuf, sizeof(errbuf));
}
