#pragma once

#include <stdint.h>

#define URL_ESCAPE_PATH   1
#define URL_ESCAPE_PARAM  2

int url_escape(char *dst, const int size, const char *src, int how);

char *base64_encode(char *out, int out_size, const uint8_t *in, int in_size);

int  base64_decode(uint8_t *out, const char *in, int out_size);

#define AV_BASE64_SIZE(x)  (((x)+2) / 3 * 4 + 1)

int dictcmp(const char *a, const char *b);

#define WRITEFILE_NO_CHANGE 1000000

int writefile(const char *path, void *bug, int size);

char *readfile(const char *path, int *intptr);

void url_split(char *proto, int proto_size,
               char *authorization, int authorization_size,
               char *hostname, int hostname_size,
               int *port_ptr,
               char *path, int path_size,
               const char *url);
