#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <git2.h>
#include <mysql.h>

#include "misc/htsmsg_json.h"

#include "net/tcp.h"
#include "net/http.h"


#include "artifact_serve.h"
#include "db.h"
#include "doozer.h"
#include "cfg.h"
#include "project.h"
#include "buildmaster.h"
#include "irc.h"
#include "github.h"
#include "restapi.h"

#include <sys/types.h>
#include <regex.h>


static int dosyslog = 0;
static int running = 1;
static int reload = 0;

/**
 *
 */
static void
handle_sigpipe(int x)
{
  return;
}


/**
 *
 */
static void
doexit(int x)
{
  running = 0;
}



/**
 *
 */
static void
doreload(int x)
{
  reload = 1;
}

/**
 *
 */
void
tracev(int level, const char *fmt, va_list ap)
{
  if(dosyslog) {
    va_list aq;
    va_copy(aq, ap);
    vsyslog(level, fmt, aq);
    va_end(aq);
  }

  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}


/**
 *
 */
void
trace(int level, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);

  if(dosyslog) {
    va_list aq;
    va_copy(aq, ap);
    vsyslog(level, fmt, aq);
    va_end(aq);
  }

  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}



/**
 *
 */
static void
refresh_subsystems(void)
{
  irc_refresh_config();
  projects_reload();
}


/**
 *
 */
int
main(int argc, char **argv)
{
  int c;
  sigset_t set;
  const char *cfgfile = NULL;

  signal(SIGPIPE, handle_sigpipe);

  while((c = getopt(argc, argv, "c:")) != -1) {
    switch(c) {
    case 'c':
      cfgfile = optarg;
      break;
    }
  }

  sigfillset(&set);
  sigprocmask(SIG_BLOCK, &set, NULL);

  srand48(getpid() ^ time(NULL));

  if(cfg_load(cfgfile))
    exit(1);

  git_threads_init();

  mysql_library_init(0, NULL, NULL);

  tcp_server_init();

  {
    cfg_root(cr);
    http_server_init(cfg_get_int(cr, CFG("webservice", "port"), 9000));
  }

  db_init();

  artifact_serve_init();

  projects_init();

  buildmaster_init();

  github_init();

  restapi_init();

  running = 1;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGHUP);

  signal(SIGTERM, doexit);
  signal(SIGINT, doexit);
  signal(SIGHUP, doreload);

  pthread_sigmask(SIG_UNBLOCK, &set, NULL);

  while(running) {
    if(reload) {
      reload = 0;
      if(!cfg_load(NULL)) {
        refresh_subsystems();
      }
    }
    pause();
  }

  return 0;
}


/**
 *
 */
void
mutex_unlock_ptr(pthread_mutex_t **p)
{
  pthread_mutex_unlock(*p);
}
