#include <string.h>
#include <stdio.h>

#include "cmd.h"
#include "buildmaster.h"
#include "doozer.h"

#include "net/http.h" // wrong

int
cmd_exec(const char *line, const char *user,
         void (*msg)(void *opaque, const char *fmt, ...),
         void *opaque)
{
  char *l = mystrdupa(line);
  char *argv[64];

  int argc = http_tokenize(l, argv, 64, ' ');

  if(argc == 0) {
    msg(opaque, "No command given");
    return 1;
  }

  if(!strcmp(argv[0], "build")) {
    if(argc != 4) {
      msg(opaque,
          "usage: build <project> <branch | revision> <target>");
      return 1;
    }
    char reason[256];
    snprintf(reason, sizeof(reason), "Requested by %s", user);
    return buildmaster_add_build(argv[1], argv[2], argv[3],
                                 reason, msg, opaque);
  }


  msg(opaque, "Unknown command: %s", line);
  return 1;
}
