#pragma once

int aws_s3_delete_file(const char *bucket, const char *awsid, const char *secret,
                       const char *path, char *errbuf, size_t errlen);

int aws_s3_put_file(const char *bucket, const char *awsid, const char *secret,
                    const char *path, char *errbuf, size_t errlen,
                    void *data, size_t len, const char *content_type);
