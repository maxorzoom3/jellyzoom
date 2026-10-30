#include "../src/jf/http_client.h"

#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void serve(int listener, const char *method, const char *path, const char *body, const char *response)
{
	int client = accept(listener, NULL, NULL);
	assert(client >= 0);
	char request[4096] = {0};
	size_t used = 0;
	char *end = NULL;
	while (!end) {
		const ssize_t n = recv(client, request + used, sizeof(request) - used - 1, 0);
		assert(n > 0);
		used += (size_t)n;
		request[used] = '\0';
		end = strstr(request, "\r\n\r\n");
	}
	const size_t header_size = (size_t)(end + 4 - request);
	const size_t expected = strlen(body);
	while (used < header_size + expected) {
		const ssize_t n = recv(client, request + used, sizeof(request) - used - 1, 0);
		assert(n > 0);
		used += (size_t)n;
		request[used] = '\0';
	}
	char first[128];
	snprintf(first, sizeof(first), "%s %s HTTP/1.0\r\n", method, path);
	assert(strstr(request, first) == request);
	assert(strstr(request, "Authorization: test\r\n") != NULL);
	assert(strcmp(request + header_size, body) == 0);
	const size_t response_size = strlen(response);
	assert(send(client, response, response_size, 0) == (ssize_t)response_size);
	close(client);
}

int main(void)
{
	const int listener = socket(AF_INET, SOCK_STREAM, 0);
	assert(listener >= 0);
	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
	assert(listen(listener, 3) == 0);
	socklen_t length = sizeof(address);
	assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
	const pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		const char *empty = "HTTP/1.0 204 No Content\r\nContent-Length: 0\r\n\r\n";
		serve(listener, "POST", "/Users/user/PlayedItems/item", "{}", empty);
		serve(listener, "DELETE", "/Users/user/PlayedItems/item", "", empty);
		serve(listener, "GET", "/Items/season/Images/Primary", "",
		      "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n");
		close(listener);
		_exit(0);
	}
	const int port = ntohs(address.sin_port);
	HttpResponse response;
	assert(http_post_json("127.0.0.1", port, "/Users/user/PlayedItems/item", "Authorization: test\r\n",
	                      "{}", &response) == 0);
	assert(response.status == 204 && response.body_size == 0);
	http_response_free(&response);
	assert(http_delete("127.0.0.1", port, "/Users/user/PlayedItems/item", "Authorization: test\r\n",
	                   &response) == 0);
	assert(response.status == 204 && response.body_size == 0);
	http_response_free(&response);
	HttpBinaryResponse image;
	assert(http_get_binary("127.0.0.1", port, "/Items/season/Images/Primary", "Authorization: test\r\n", &image) == 0);
	assert(image.status == 404 && image.size == 0);
	http_binary_response_free(&image);
	close(listener);
	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
