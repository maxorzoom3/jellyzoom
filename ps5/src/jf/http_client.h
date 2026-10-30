#pragma once

#include <stddef.h>

typedef struct {
    int status;
    char *body;
    size_t body_size;
} HttpResponse;

typedef struct {
    int status;
    unsigned char *data;
    size_t size;
    char content_type[128];
} HttpBinaryResponse;

int http_get(const char *host,
             int port,
             const char *path,
             const char *authorization,
             HttpResponse *out);

int http_post_json(const char *host,
                   int port,
                   const char *path,
                   const char *authorization,
                   const char *json,
                   HttpResponse *out);

int http_delete(const char *host,
                int port,
                const char *path,
                const char *authorization,
                HttpResponse *out);

int http_get_binary(const char *host,
                    int port,
                    const char *path,
                    const char *authorization,
                    HttpBinaryResponse *out);

int http_get_binary_limited(const char *host,
                            int port,
                            const char *path,
                            const char *authorization,
                            size_t max_body_size,
                            HttpBinaryResponse *out);

void http_response_free(HttpResponse *r);
void http_binary_response_free(HttpBinaryResponse *r);
