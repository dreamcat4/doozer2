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

  if(!isatty(2))
    return;

  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}


/**
 *
 */
void
trace(int level, const char *fmt, ...)
{
  char *s = mystrdupa(fmt);
  decolorize(s);
  va_list ap;
  va_start(ap, fmt);
  tracev(level, s, ap);
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
static void
http_init(void)
{
  cfg_root(cr);

  int port = cfg_get_int(cr, CFG("http", "port"), 9000);
  const char *bindaddr = cfg_get_str(cr, CFG("http", "bindAddress"),
                                     "127.0.0.1");
  if(http_server_init(port, bindaddr))
    exit(1);
}



/**
 *
 */
static void
enable_syslog(const char *facility)
{
  int f;
  const char *x;
  if(!strcmp(facility, "daemon"))
    f = LOG_DAEMON;
  else if((x = mystrbegins(facility, "local")) != NULL)
    f = LOG_LOCAL0 + atoi(x);
  else {
    fprintf(stderr, "Invalid syslog config -- %s\n", facility);
    exit(1);
  }

  dosyslog = 1;
  openlog("doozer", LOG_PID, f);

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

  while((c = getopt(argc, argv, "c:s:")) != -1) {
    switch(c) {
    case 'c':
      cfgfile = optarg;
      break;
    case 's':
      enable_syslog(optarg);
      break;
    }
  }

  sigfillset(&set);
  sigprocmask(SIG_BLOCK, &set, NULL);

  srand48(getpid() ^ time(NULL));

  if(cfg_load(cfgfile)) {
    fprintf(stderr, "Unable to load config (check -c option). Giving up\n");
    exit(1);
  }

  git_threads_init();

  mysql_library_init(0, NULL, NULL);

  tcp_server_init();

  http_init();

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


/**
 *
 */
void
decolorize(char *s)
{
  char *d = s;
  while(*s) {
    if(*s == '\003') {
      s++;
      if(*s >= '0' && *s <= '9')
        s++;
      if(*s >= '0' && *s <= '9')
        s++;
      continue;
    }
    *d++ = *s++;
  }
  *d = 0;
}
