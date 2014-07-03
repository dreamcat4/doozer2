#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "doozer.h"
#include "git.h"
#include "libsvc/threading.h"
#include "libsvc/misc.h"
#include "libsvc/talloc.h"

/**
 * Must be called with lock held
 * Return 0 if repo exists and is usable
 */
static int
ensure_repo(project_t *p)
{
  const char *repo;

  if(p->p_repo != NULL)
    return 0;

  project_cfg(pc, p->p_id);
  if(pc == NULL) {
    plog(p, "git/repo", "Unable to open GIT repo -- No project config");
    return DOOZER_ERROR_PERMANENT;
  }

  repo = cfg_get_str(pc, CFG("repo"), NULL);

  if(repo == NULL) {
    cfg_root(root);

    char path[512];
    if(p->p_repo != NULL)
      return 0;

    const char *repos = cfg_get_str(root, CFG("repos"),
				    "/var/tmp/doozer-git-repos");

    snprintf(path, sizeof(path), "%s/%s", repos, p->p_id);

    repo = path;
  }

  int r;
  if((r = git_repository_open_bare(&p->p_repo, repo)) < 0) {

    if(r == GIT_ENOTFOUND) {
      plog(p, "git/repo",
           "Creating new repository at %s", repo);
      r = git_repository_init(&p->p_repo, repo, 1);
    }
  }

  if(r < 0) {
    plog(p, "git/repo", "Unable to open GIT repo %s -- %s",
         repo, giterr());
    return DOOZER_ERROR_TRANSIENT;
  }
  plog(p, "git/repo", "Git repo at %s opened", repo);
  return 0;
}



/**
 *
 */
static int
update_cb(const char *refname, const git_oid *a, const git_oid *b, void *data)
{
  project_t *p = data;
  char a_str[GIT_OID_HEXSZ+1], b_str[GIT_OID_HEXSZ+1];

  git_oid_fmt(b_str, b);
  b_str[GIT_OID_HEXSZ] = '\0';

  if(git_oid_iszero(a)) {
    plog(p, "git/branch",
         "GIT: [new]     %.20s %s", b_str, refname);
  } else {
    git_oid_fmt(a_str, a);
    a_str[GIT_OID_HEXSZ] = '\0';
    plog(p, "git/branch",
         "GIT: [updated] %.10s..%.10s %s",
         a_str, b_str, refname);
  }

  project_schedule_job(p,
                       PROJECT_JOB_CHECK_FOR_BUILDS |
                       PROJECT_JOB_NOTIFY_REPO_UPDATE |
                       PROJECT_JOB_GENERATE_RELEASES);
  return 0;
}

/**
 *
 */
static int
progress_cb(const char *str, int len, void *data)
{
  printf("remote: %.*s", len, str);
  fflush(stdout); /* We don't have the \n to force the flush */
  return 0;
}


/**
 *
 */
static int
cred_acquire_cb(git_cred **out,
		const char *url,
		const char *username_from_url,
		unsigned int allowed_types,
		void *payload)
{
  project_t *p = payload;

  project_cfg(pc, p->p_id);
  if(pc == NULL)
    return -1;

  const char *username = cfg_get_str(pc, CFG("gitrepo", "username"),
				     username_from_url);

  if(allowed_types & GIT_CREDTYPE_USERPASS_PLAINTEXT) {
    const char *password = cfg_get_str(pc, CFG("gitrepo", "password"),
				       NULL);
    
    if(password != NULL) {
      plog(p, "git/repo", "Trying password authentication");
      return git_cred_userpass_plaintext_new(out, username, password);
    }
  }


  if(allowed_types & GIT_CREDTYPE_SSH_KEY) {

    const char *home = getenv("HOME");
    char buf_pub_path[PATH_MAX];
    char buf_priv_path[PATH_MAX];
    const char *priv_path = NULL;
    const char *pub_path = NULL;

    if(home != NULL) {
      snprintf(buf_pub_path, PATH_MAX, "%s/.ssh/id_rsa.pub", home);
      snprintf(buf_priv_path, PATH_MAX, "%s/.ssh/id_rsa", home);
      if(!access(buf_pub_path, R_OK) && !access(buf_priv_path, R_OK)) {
	pub_path  = buf_pub_path;
	priv_path = buf_priv_path;
      } else {
	snprintf(buf_pub_path, PATH_MAX, "%s/.ssh/id_dsa.pub", home);
	snprintf(buf_priv_path, PATH_MAX, "%s/.ssh/id_dsa", home);
	if(!access(buf_pub_path, R_OK) && !access(buf_priv_path, R_OK)) {
	  pub_path  = buf_pub_path;
	  priv_path = buf_priv_path;
	}
      }
    }

    pub_path  = cfg_get_str(pc, CFG("gitrepo", "ssh", "pubPath"),  pub_path);
    priv_path = cfg_get_str(pc, CFG("gitrepo", "ssh", "privPath"), priv_path);
    const char *pw = cfg_get_str(pc, CFG("gitrepo", "ssh", "password"), NULL);

    if(pub_path != NULL && priv_path != NULL) {
      plog(p, "git/repo", "Trying SSH key authentication using %s %s",
           pub_path, priv_path);
      return git_cred_ssh_key_new(out, username, pub_path, priv_path, pw);
    }
  }
  plog(p, "git/repo", "No available authentication methods");
  return -1;
}

/**
 *
 */
int
git_repo_sync(project_t *p)
{
  scoped_lock(&p->p_repo_mutex);

  int retval;
  if((retval = ensure_repo(p)))
    return retval;

  project_cfg(pc, p->p_id);
  if(pc == NULL)
    return DOOZER_ERROR_PERMANENT;

  const char *upstream = cfg_get_str(pc, CFG("gitrepo", "pub"), NULL);
  if(upstream == NULL) {
    plog(p, "git/repo", "No GIT upstream configured");
    return DOOZER_ERROR_PERMANENT;
  }

  const char *refspec = cfg_get_str(pc, CFG("gitrepo", "refspec"),
                                    "+refs/*:refs/*");

  plog(p, "git/repo", "Syncing repo from %s", upstream);

  git_remote *r;
  if(git_remote_create_inmemory(&r, p->p_repo, refspec, upstream) < 0) {
    plog(p, "git/repo", "Unable to create in-memory remote");
    return DOOZER_ERROR_TRANSIENT;
  }

  int err = DOOZER_ERROR_TRANSIENT;

  git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;

  callbacks.update_tips = &update_cb;
  callbacks.payload = p;
  if(isatty(1))
    callbacks.progress = &progress_cb;
  
  callbacks.credentials = &cred_acquire_cb;

  git_remote_set_callbacks(r, &callbacks);
  git_remote_set_autotag(r, GIT_REMOTE_DOWNLOAD_TAGS_AUTO);

  if (git_remote_connect(r, GIT_DIRECTION_FETCH) < 0) {
    plog(p, "git/repo", "Unable to connect to %s -- %s", upstream,
          giterr());
    goto done;
  }

  if (git_remote_download(r) < 0) {
    plog(p, "git/repo", "Unable to download from %s -- %s", upstream,
          giterr());
    goto disconnect;
  }

  if(git_remote_update_tips(r) < 0) {
    plog(p, "git/repo", "Unable to update tips from %s -- %s", upstream,
          giterr());
  } else {
    plog(p, "git/repo", "Synced repo from %s", upstream);
    err = 0;
  }

 disconnect:
  git_remote_disconnect(r);
 done:
  git_remote_free(r);
  return err;
}


/**
 *
 */
static int
branchcmp(const ref_t *a, const ref_t *b)
{
  return dictcmp(b->name, a->name);
}


/**
 *
 */
int
git_repo_list_branches(project_t *p, struct ref_list *bl)
{
  git_reference_iterator *iter;
  git_reference *ref;
  ref_t *b;
  LIST_INIT(bl);
  scoped_lock(&p->p_repo_mutex);

  git_reference_iterator_glob_new(&iter, p->p_repo, "refs/heads/*");
  while(!git_reference_next(&ref, iter)) {
    b = calloc(1, sizeof(ref_t));
    b->name = strdup(git_reference_name(ref) + strlen("refs/heads/"));
    git_oid_cpy(&b->oid, git_reference_target(ref));
    git_oid_fmt(b->oidtxt, &b->oid);
    LIST_INSERT_SORTED(bl, b, link, branchcmp);
    git_reference_free(ref);
  }
  git_reference_iterator_free(iter);
  return 0;
}

struct tag_list_aux {
  git_repository *repo;
  struct ref_list *rl;
};


/**
 *
 */
static int
tag_list_callback(const char *name, git_oid *oid, void *payload)
{
  struct tag_list_aux * aux = payload;
  git_tag *tag;

  ref_t *r = calloc(1, sizeof(ref_t));
  r->name = strdup(name + strlen("refs/tags/"));

  if(!git_tag_lookup(&tag, aux->repo, oid)) {
    git_oid_cpy(&r->oid, git_tag_target_id(tag));
  } else {
    git_oid_cpy(&r->oid, oid);
    tag = NULL;
  }

  git_oid_fmt(r->oidtxt, &r->oid);
  LIST_INSERT_HEAD(aux->rl, r, link);
  if(tag != NULL)
    git_tag_free(tag);
  return 0;
}


/**
 *
 */
int
git_repo_list_tags(project_t *p, struct ref_list *bl)
{
  struct tag_list_aux aux;

  scoped_lock(&p->p_repo_mutex);
  LIST_INIT(bl);
  aux.repo = p->p_repo;
  aux.rl = bl;
  git_tag_foreach(p->p_repo, &tag_list_callback, &aux);
  return 0;
}


/**
 *
 */
void
git_repo_free_refs(struct ref_list *bl)
{
  ref_t *b;
  while((b = LIST_FIRST(bl)) != NULL) {
    LIST_REMOVE(b, link);
    free(b->name);
    free(b);
  }
}

/**
 *
 */
static void
version_snprint(char *out, size_t outlen, const char *tag, int distance,
                const git_oid *oid)
{
  if(tag == NULL)
    tag = "0.0";

  if(distance) {
    if(oid != NULL) {
      char outhash[41];
      git_oid_fmt(outhash, oid);
      snprintf(out, outlen, "%s.%d-g%.8s", tag, distance, outhash);
    } else {
      snprintf(out, outlen, "%s.%d", tag, distance);
    }
  } else {
    snprintf(out, outlen, "%s", tag);
  }
}



/**
 *
 */
static ref_t *
ref_from_oid(const struct ref_list *rl, const git_oid *oid)
{
  ref_t *r;
  LIST_FOREACH(r, rl, link)
    if(!git_oid_cmp(&r->oid, oid))
      return r;
  return NULL;
}


/**
 *
 */
int
git_describe(char *out, size_t outlen, project_t *p, const char *revision,
             int with_hash)
{
  git_oid start_oid, oid;

  if(git_oid_fromstr(&start_oid, revision))
    return DOOZER_ERROR_PERMANENT;

  scoped_lock(&p->p_repo_mutex);

  struct ref_list rl;
  LIST_INIT(&rl);
  struct tag_list_aux tla;
  tla.repo = p->p_repo;
  tla.rl = &rl;
  git_tag_foreach(p->p_repo, &tag_list_callback, &tla);

  git_revwalk *walk;
  git_revwalk_new(&walk, p->p_repo);
  git_revwalk_push(walk, &start_oid);
  git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL);
  int distance = 0;
  ref_t *r = NULL;
  int retval = 1;
  while(!git_revwalk_next(&oid, walk)) {
    retval = 0;
    if((r = ref_from_oid(&rl, &oid)) != NULL)
      break;
    distance++;
  }

  version_snprint(out, outlen, r ? r->name : NULL, distance,
                  with_hash ? &start_oid : NULL);
  git_repo_free_refs(&rl);
  git_revwalk_free(walk);

  return retval;
}



/**
 *
 */
static change_t *
make_change_from_ref(const git_oid *oid, struct ref_list *rl,
                     struct change_queue *cq)
{
  change_t *c = calloc(1, sizeof(change_t));
  TAILQ_INSERT_TAIL(cq, c, link);
  git_oid_cpy(&c->oid, oid);
  ref_t *r = ref_from_oid(rl, oid);
  if(r != NULL)
    c->tag = strdup(r->name);
  return c;
}


/**
 *
 */
static void
change_free(change_t *c)
{
  free(c->msg);
  free(c->tag);
  free(c);
}


/**
 *
 */
int
git_changelog(struct change_queue *cq, project_t *p, const git_oid *start_oid,
              int offset, int count, int all, const char *target)
{
  git_oid oid;
  int retval;
  char tchangelog[128];
  change_t *c;

  TAILQ_INIT(cq);

  if(count == 0)
    return 0;

  scoped_lock(&p->p_repo_mutex);

  if((retval = ensure_repo(p)))
    return retval;

  // If we want to include target specific changelog, create the ref
  if(target) {
    snprintf(tchangelog, sizeof(tchangelog), "refs/notes/changelog-%s",
             target);
    target = tchangelog;
  }

  // First, figure out the tags we have

  struct ref_list rl;
  LIST_INIT(&rl);
  struct tag_list_aux tla;
  tla.repo = p->p_repo;
  tla.rl = &rl;
  git_tag_foreach(p->p_repo, &tag_list_callback, &tla);

  git_revwalk *walk;
  git_revwalk_new(&walk, p->p_repo);
  git_revwalk_push(walk, start_oid);

  git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL);

  // Walk refs and search for matching changelog entries

  while(!git_revwalk_next(&oid, walk) && count) {
    c = make_change_from_ref(&oid, &rl, cq);
    git_note *note;

    if(target != NULL) {
      if(!git_note_read(&note, p->p_repo, target, &oid)) {
        c->msg = strdup(git_note_message(note));
        git_note_free(note);
      }
    }

    if(!git_note_read(&note, p->p_repo, "refs/notes/changelog", &oid)) {
      if(c->msg != NULL) {
        const char *m = git_note_message(note);
        int ml = strlen(m);
        int cl = strlen(c->msg);
        char *out = malloc(cl + ml + 2);

        memcpy(out, m, ml);
        out[ml] = '\n';
        memcpy(out + ml + 1, c->msg, cl);
        out[cl + ml + 1] = 0;
        free(c->msg);
        c->msg = out;

      } else {
        c->msg = strdup(git_note_message(note));
      }
      git_note_free(note);
    }

    if(all || c->msg)
      count--;
  }

  int distance = 0;
  const char *tag = NULL;

  c = TAILQ_LAST(cq, change_queue);
  if(c != NULL) {
    if(c->tag != NULL) {
      tag = c->tag;
    } else {

      // Need to do some additional walking to find preceding tag and its distance
      while(!git_revwalk_next(&oid, walk)) {
        distance++;
        ref_t *r = ref_from_oid(&rl, &oid);
        if(r != NULL) {
          tag = r->name;
          break;
        }
      }
      if(tag == NULL)
        tag = "0.0";
    }

    // Generate version strings for each change

    TAILQ_FOREACH_REVERSE(c, cq, change_queue, link) {
      if(c->tag != NULL) {
        tag = c->tag;
        distance = 0;
      }
      version_snprint(c->version, sizeof(c->version), tag, distance, NULL);
      distance++;
    }

    if(!all) {

      // Get rid of changes that does not have a changelog entry

      change_t *d;
      for(c = TAILQ_FIRST(cq); c != NULL; c = d) {
        d = TAILQ_NEXT(c, link);
        if(c->msg == NULL) {
          TAILQ_REMOVE(cq, c, link);
          change_free(c);
        }
      }
    }
  }
  git_repo_free_refs(&rl);
  git_revwalk_free(walk);
  return 0;
}


/**
 *
 */
void
git_changlog_free(struct change_queue *cq)
{
  change_t *c, *d;
  for(c = TAILQ_FIRST(cq); c != NULL; c = d) {
    d = TAILQ_NEXT(c, link);
    change_free(c);
  }
}



/**
 *
 */
const char *giterr(void)
{
  const git_error *ge = giterr_last();
  if(ge == NULL)
    return NULL;
  return ge->message;
}


int
git_get_file(project_t *p, const git_oid *oid,
             const char *path, void **datap, size_t *sizep,
             char *errbuf, size_t errlen)
{
  git_tree *tree = NULL;
  git_commit *commit = NULL;
  git_object *dir = NULL;
  git_object *blob = NULL;
  const git_tree_entry *e;

  char *filename = mystrdupa(path);
  char *next;

  int r = -1;

  scoped_lock(&p->p_repo_mutex);

  if(git_commit_lookup(&commit, p->p_repo, oid)) {
    snprintf(errbuf, errlen,
             "Unable to lookup commit id when looking for manifest");
    return -1;
  }

  if(git_commit_tree(&tree, commit)) {
    snprintf(errbuf, errlen,
             "Unable to open git tree when looking for manifest");
    goto cleanup;
  }

  while((next = strchr(filename, '/')) != NULL) {
    while(*next == '/')
      *next++ = 0;

    // Directory (or tree) component

    printf("filename=%s\n", filename);

    if((e = git_tree_entry_byname(tree, filename)) == NULL)  {
      snprintf(errbuf, errlen,
               "'%s' directory not found", filename);
      goto cleanup;
    }

    if(git_tree_entry_to_object(&dir, p->p_repo, e)) {
      snprintf(errbuf, errlen,
               "Unable to lookup '%s' tree object", filename);
      goto cleanup;
    }

    if(git_object_type(dir) != GIT_OBJ_TREE) {
      snprintf(errbuf, errlen,
               "'%s' is not a tree object", filename);
      goto cleanup;
    }

    git_tree_free(tree);
    tree = (git_tree *)dir;
    dir = NULL;
    filename = next;
  }

  if((e = git_tree_entry_byname((git_tree *)tree, filename)) == NULL)  {
    snprintf(errbuf, errlen, "'%s' not found", filename);
    goto cleanup;
  }

  if(git_tree_entry_to_object(&blob, p->p_repo, e)) {
    snprintf(errbuf, errlen, "Unable to lookup '%s' object", filename);
    goto cleanup;
  }

  if(git_object_type(blob) != GIT_OBJ_BLOB) {
    snprintf(errbuf, errlen, "'%s' is not a file", filename);
    goto cleanup;
  }


  size_t size = git_blob_rawsize((git_blob *)blob);
  char *data = talloc_malloc(size + 1);
  memcpy(data, git_blob_rawcontent((git_blob *)blob), size);
  data[size] = 0;

  if(sizep != NULL)
    *sizep = size;

  *datap = data;
  r = 0;

 cleanup:
  git_object_free(blob);
  if(tree != NULL)
    git_tree_free(tree);
  if(commit != NULL)
    git_commit_free(commit);
  return r;
}
