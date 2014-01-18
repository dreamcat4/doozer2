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
#include "makefile.h"
#include "artifact.h"

/**
 *
 */
void
makefile_process(job_t *j)
{
  htsbuf_queue_t output;
  htsbuf_queue_init2(&output, 100000);

  int r = job_run_command(j, "/usr/bin/env",
                          (const char *[]){"/usr/bin/env", "make", NULL},
                          &output, "make");


  if(r == -1)
    return;

  if(output.hq_size) {
    if(artifact_add_htsbuf(j, "buildlog", "buildlog", NULL, &output, 1)) {
      return;
    }
  }

  if(aritfacts_wait(j)) {
    return;
  }

  if(r == 0) {
    job_report_status(j, "done", "Build done");
  } else if(r == -2) {
    job_report_temp_fail(j, "%s: Command timed out", "make");
  } else {
    job_report_fail(j, "%s: exited with %d", "make", r);
  }
}
