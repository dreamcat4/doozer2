#pragma once

int aws_s3_delete_file(const char *bucket, const char *awsid, const char *secret,
                       const char *path, char *errbuf, size_t errlen);
