#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "libsvc/trace.h"

#include "linux_btrfs.h"
#include "heap_btrfs.h"



typedef struct heapmgr_btrfs {
  heapmgr_t super;

  char *path;

} heapmgr_btrfs_t;


/**
 *
 */
static void
heap_btrfs_dtor(heapmgr_t *super)
{
  heapmgr_btrfs_t *hm = (heapmgr_btrfs_t *)super;
  free(hm->path);
  free(hm);
}




/**
 *
 */
static int
heap_btrfs_open(struct heapmgr *super, const char *id,
                char outpath[PATH_MAX],
                char *errbuf, size_t errlen, int create)
{
  heapmgr_btrfs_t *hm = (heapmgr_btrfs_t *)super;
  snprintf(outpath, PATH_MAX, "%s/%s", hm->path, id);

  struct stat st;
  int r = stat(outpath, &st);
  if(r == 0) {
    if(st.st_ino == 256 && S_ISDIR(st.st_mode))
      return 0;

    snprintf(errbuf, errlen, "%s exists but is not a Btrfs subvolume",
             outpath);
    return -1;
  }

  if(!create) {
    snprintf(errbuf, errlen, "%s does not exist", outpath);
    return -1;
  }

  int fd = open(hm->path, O_RDONLY);
  if(fd == -1) {
    snprintf(errbuf, errlen, "Unable to open parent dir %s -- %s",
             hm->path, strerror(errno));
    return -1;
  }

  struct btrfs_ioctl_vol_args args = {};
  snprintf(args.name, BTRFS_SUBVOL_NAME_MAX, "%s", id);
  r = ioctl(fd, BTRFS_IOC_SUBVOL_CREATE, &args);
  int err = errno;
  close(fd);

  if(r == 0)
    return 0;

  snprintf(errbuf, errlen,
           "Unable to create Btrfs subvolume %s at %s -- %s",
           id, hm->path, strerror(err));
  return -1;
}


/**
 *
 */
static int
heap_btrfs_delete(struct heapmgr *hm, const char *name)
{
  return 0;
}

/**
 *
 */
heapmgr_t *
heap_btrfs_init(const char *path)
{
  int fd = open(path, O_RDONLY);
  if(fd == -1) {
    trace(LOG_WARNING, "heap_btrfs: %s is not accessible", path);
    return NULL;
  }

  struct btrfs_ioctl_ino_lookup_args args = {};

  args.objectid = 256ULL;
  int r = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &args);
  close(fd);
  if(r < 0) {
    trace(LOG_WARNING,
          "heap_btrfs: %s is not on a Btrfs filesystem -- %s",
          path, strerror(errno));
    return NULL;
  }

  heapmgr_btrfs_t *hm = calloc(1, sizeof(heapmgr_btrfs_t));
  hm->path = strdup(path);
  hm->super.dtor = heap_btrfs_dtor;
  hm->super.open_heap = heap_btrfs_open;
  hm->super.delete_heap = heap_btrfs_delete;

  return &hm->super;
}

