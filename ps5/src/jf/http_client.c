#include "http_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define HTTP_BUFFER_SIZE 16384

static int connect_host(const char *host, int port)
{
    int sock;
    struct sockaddr_in addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

static int send_all(int sock, const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;

    while (size > 0) {
        ssize_t n = send(sock, p, size, 0);

        if (n <= 0)
            return -1;

        p += n;
        size -= (size_t)n;
    }

    return 0;
}

static int read_all(int sock, unsigned char **out, size_t *out_size)
{
    unsigned char *buffer = NULL;
    size_t size = 0;
    size_t capacity = 0;

    for (;;) {
        unsigned char temp[HTTP_BUFFER_SIZE];
        ssize_t n = recv(sock, temp, sizeof(temp), 0);

        if (n == 0)
            break;

        if (n < 0) {
            free(buffer);
            return -1;
        }

        if (size + (size_t)n > capacity) {
            size_t new_capacity = capacity ? capacity * 2 : HTTP_BUFFER_SIZE;

            while (new_capacity < size + (size_t)n)
                new_capacity *= 2;

            unsigned char *new_buffer =
                realloc(buffer, new_capacity);

            if (!new_buffer) {
                free(buffer);
                return -1;
            }

            buffer = new_buffer;
            capacity = new_capacity;
        }

        memcpy(buffer + size, temp, (size_t)n);
        size += (size_t)n;
    }

    *out = buffer;
    *out_size = size;

    return 0;
}

static int parse_status(const unsigned char *data, size_t size)
{
    if (!data || size < 12)
        return -1;

    char status_line[64];
    size_t copy = size < sizeof(status_line) - 1
                     ? size
                     : sizeof(status_line) - 1;

    memcpy(status_line, data, copy);
    status_line[copy] = '\0';

    int status = 0;

    if (sscanf(status_line, "HTTP/%*s %d", &status) != 1)
        return -1;

    return status;
}

static unsigned char *find_body(unsigned char *data, size_t size,
                                size_t *body_size)
{
    if (!data || size == 0)
        return NULL;

    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == '\r' &&
            data[i + 1] == '\n' &&
            data[i + 2] == '\r' &&
            data[i + 3] == '\n') {

            size_t offset = i + 4;

            if (offset > size)
                return NULL;

            *body_size = size - offset;
            return data + offset;
        }
    }

    return NULL;
}

static int send_request(const char *host,
                        int port,
                        const char *method,
                        const char *path,
                        const char *content_type,
                        const char *body,
                        unsigned char **response,
                        size_t *response_size)
{
    int sock = connect_host(host, port);

    if (sock < 0)
        return -1;

    char header[8192];

    size_t body_length = body ? strlen(body) : 0;

    int header_length = snprintf(
        header,
        sizeof(header),
        "%s %s HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "%s"
        "Content-Length: %zu\r\n"
        "\r\n",
        method,
        path,
        host,
        port,
        content_type ? content_type : "",
        body_length
    );

    if (header_length <= 0 ||
        (size_t)header_length >= sizeof(header)) {
        close(sock);
        return -1;
    }

    if (send_all(sock, header, (size_t)header_length) != 0) {
        close(sock);
        return -1;
    }

    if (body_length > 0) {
        if (send_all(sock, body, body_length) != 0) {
            close(sock);
            return -1;
        }
    }

    int result = read_all(sock, response, response_size);

    close(sock);

    return result;
}

int http_get(const char *host,
             int port,
             const char *path,
             const char *auth_header,
             HttpResponse *response)
{
    if (!response)
        return -1;

    memset(response, 0, sizeof(*response));

    int sock = connect_host(host, port);

    if (sock < 0)
        return -1;

    char header[8192];

    int header_length = snprintf(
        header,
        sizeof(header),
        "GET %s HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        path,
        host,
        port,
        auth_header ? auth_header : ""
    );

    if (header_length <= 0 ||
        (size_t)header_length >= sizeof(header)) {
        close(sock);
        return -1;
    }

    if (send_all(sock, header, (size_t)header_length) != 0) {
        close(sock);
        return -1;
    }

    unsigned char *raw = NULL;
    size_t raw_size = 0;

    if (read_all(sock, &raw, &raw_size) != 0) {
        close(sock);
        return -1;
    }

    close(sock);

    response->status = parse_status(raw, raw_size);

    size_t body_size = 0;
    unsigned char *body = find_body(raw, raw_size, &body_size);

    if (!body) {
        free(raw);
        return -1;
    }

    response->body = malloc(body_size + 1);

    if (!response->body) {
        free(raw);
        return -1;
    }

    memcpy(response->body, body, body_size);
    response->body[body_size] = '\0';
    response->body_size = body_size;

    free(raw);

    return 0;
}

int http_post_json(const char *host,
                   int port,
                   const char *path,
                   const char *auth_header,
                   const char *json,
                   HttpResponse *response)
{
    if (!response)
        return -1;

    memset(response, 0, sizeof(*response));

    char content_type[512];

    snprintf(
        content_type,
        sizeof(content_type),
        "Content-Type: application/json\r\n"
        "%s",
        auth_header ? auth_header : ""
    );

    unsigned char *raw = NULL;
    size_t raw_size = 0;

    if (send_request(
            host,
            port,
            "POST",
            path,
            content_type,
            json ? json : "",
            &raw,
            &raw_size) != 0) {
        return -1;
    }

    response->status = parse_status(raw, raw_size);

    size_t body_size = 0;
    unsigned char *body = find_body(raw, raw_size, &body_size);

    if (!body) {
        free(raw);
        return -1;
    }

    response->body = malloc(body_size + 1);

    if (!response->body) {
        free(raw);
        return -1;
    }

    memcpy(response->body, body, body_size);
    response->body[body_size] = '\0';
    response->body_size = body_size;

    free(raw);

    return 0;
}

int http_get_binary(const char *host,
                    int port,
                    const char *path,
                    const char *auth_header,
                    HttpBinaryResponse *response)
{
    if (!response)
        return -1;

    memset(response, 0, sizeof(*response));

    int sock = connect_host(host, port);

    if (sock < 0)
        return -1;

    char header[8192];

    int header_length = snprintf(
        header,
        sizeof(header),
        "GET %s HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "Accept: image/jpeg,image/png,image/webp,*/*\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        path,
        host,
        port,
        auth_header ? auth_header : ""
    );

    if (header_length <= 0 ||
        (size_t)header_length >= sizeof(header)) {
        close(sock);
        return -1;
    }

    if (send_all(sock, header, (size_t)header_length) != 0) {
        close(sock);
        return -1;
    }

    unsigned char *raw = NULL;
    size_t raw_size = 0;

    if (read_all(sock, &raw, &raw_size) != 0) {
        close(sock);
        return -1;
    }

    close(sock);

    response->status = parse_status(raw, raw_size);

    size_t body_size = 0;
    unsigned char *body = find_body(raw, raw_size, &body_size);

    if (!body) {
        free(raw);
        return -1;
    }

    response->data = malloc(body_size);

    if (!response->data) {
        free(raw);
        return -1;
    }

    memcpy(response->data, body, body_size);
    response->size = body_size;

    /*
     * SDL_image detects JPEG/PNG/WebP from the actual data,
     * so Content-Type is informational here.
     */
    snprintf(response->content_type,
             sizeof(response->content_type),
             "image/jpeg");

    free(raw);

    return 0;
}

void http_response_free(HttpResponse *response)
{
    if (!response)
        return;

    free(response->body);

    memset(response, 0, sizeof(*response));
}

void http_binary_response_free(HttpBinaryResponse *response)
{
    if (!response)
        return;

    free(response->data);

    memset(response, 0, sizeof(*response));
}
