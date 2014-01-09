#include <string.h>

#include <openssl/hmac.h>
#include <curl/curl.h>

#include "libsvc/misc.h"
#include "libsvc/cmd.h"

#include "s3.h"

static size_t
dump_output(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  return size * nmemb;
}


/**
 *
 */
static struct curl_slist *
s3_makeauth(const char *bucket, const char *awsid, const char *secret,
            const char *path, const char *verb, const char *contenttype)
{
  time_t now;
  time(&now);
  uint8_t md[20];
  char b64[100];

  char sigstr[512];
  const char *timestr = time_to_RFC_1123(now);
  snprintf(sigstr, sizeof(sigstr), "%s\n\n%s\n%s\n/%s/%s",
           verb, contenttype ?: "", timestr, bucket, path);
  HMAC(EVP_sha1(), secret, strlen(secret), (void *)sigstr,
       strlen(sigstr), md, NULL);
  base64_encode(b64, sizeof(b64), md, sizeof(md));

  struct curl_slist *slist = NULL;
  char datebuf[128];
  snprintf(datebuf, sizeof(datebuf), "Date: %s", timestr);
  slist = curl_slist_append(slist, datebuf);


  if(contenttype) {
    char ctbuf[256];
    snprintf(ctbuf, sizeof(ctbuf), "Content-Type %s", contenttype);
    slist = curl_slist_append(slist, ctbuf);
  }

  char authbuf[128];
  snprintf(authbuf, sizeof(authbuf), "Authorization: AWS %s:%s",
           awsid, b64);


  return curl_slist_append(slist, authbuf);
}


/**
 *
 */
int
aws_s3_delete_file(const char *bucket, const char *awsid, const char *secret,
                   const char *path, char *errbuf, size_t errlen)
{
  while(*path == '/')
    path++;

  struct curl_slist *slist = s3_makeauth(bucket, awsid, secret, path, "DELETE",
                                         "");

  char url[1024];
  snprintf(url, sizeof(url), "https://%s.s3.amazonaws.com/%s", bucket, path);

  CURL *curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &dump_output);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

  CURLcode result = curl_easy_perform(curl);
  curl_slist_free_all(slist);

  curl_easy_cleanup(curl);

  if(result)
    snprintf(errbuf, errlen, "CURL error %d", result);
  return !!result;
}


/**
 *
 */
int
aws_s3_put_file(const char *bucket, const char *awsid, const char *secret,
                const char *path, char *errbuf, size_t errlen,
                void *data, size_t len, const char *content_type)
{
  FILE *f = fmemopen(data, len, "r");

  while(*path == '/')
    path++;

  struct curl_slist *slist = s3_makeauth(bucket, awsid, secret, path, "PUT",
                                         content_type);

  char url[1024];
  snprintf(url, sizeof(url), "https://%s.s3.amazonaws.com/%s", bucket, path);

  CURL *curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &dump_output);
  curl_easy_setopt(curl, CURLOPT_PUT, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_READDATA, (void *)f);
  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)len);

  CURLcode result = curl_easy_perform(curl);
  curl_slist_free_all(slist);

  curl_easy_cleanup(curl);

  if(result)
    snprintf(errbuf, errlen, "CURL error %d", result);
  fclose(f);
  return !!result;
}


/**
 *
 */
static int
aws_s3_cmd_delete(const char *user,
                  int argc, const char **argv, int *intv,
                  void (*msg)(void *opaque, const char *fmt, ...),
                  void *opaque)
{
  char errbuf[256];

  int n = aws_s3_delete_file(argv[0], argv[1], argv[2], argv[3],
                             errbuf, sizeof(errbuf));

  if(n)
    msg(opaque, "Unable to delete %s -- %s", argv[3], errbuf);
  else
    msg(opaque, "Deleted %s", argv[3]);

  return n;
}

CMD(aws_s3_cmd_delete,
    CMD_LITERAL("s3"),
    CMD_LITERAL("delete"),
    CMD_VARSTR("bucket"),
    CMD_VARSTR("awsid"),
    CMD_VARSTR("secret"),
    CMD_VARSTR("path"));
