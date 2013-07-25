#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define CTRLSOCKPATH "/tmp/doozerctrl"

/**
 *
 */
static int
docmd(FILE *f, const char *str)
{
  char line[4096];
  fprintf(f, "X%s\n", str);

  while(fgets(line, sizeof(line), f) != NULL) {
    char *r = strchr(line, '\n');
    if(r == NULL) {
      exit(2);
    }
    *r = 0;
    if(isdigit(*line))
      return atoi(line);
    else if(*line == ':')
      printf("%s\n", line + 1);
    else
      printf("???: %s\n", line);
  }
  return 1;
}


/**
 *
 */
int
main(int argc, char **argv)
{
  char buf[2048];

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd == -1) {
    perror("socket");
    exit(1);
  }

  struct sockaddr_un sun;

  memset(&sun, 0, sizeof(sun));
  sun.sun_family = AF_UNIX;
  strcpy(sun.sun_path, CTRLSOCKPATH);

  if(connect(fd, (struct sockaddr *)&sun, sizeof(sun))) {
    perror("connect");
    exit(1);
  }

  FILE *f = fdopen(fd, "r+");

  int i, l = 0;
  buf[0] = 0;
  for(i = 1; i < argc; i++) {
    l += snprintf(buf + l, sizeof(buf) - l, "%s%s",
                  i == 1 ? "" : " ", argv[i]);
  }

  if(buf[0] == 0)
    exit(0);

  docmd(f, buf);
}
