#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <curl/curl.h>

#define LISTEN_IP "127.0.0.1"
#define LISTEN_PORT 8085
#define UPSTREAM "http://127.0.0.1:1080"

struct buffer {
    char *data;
    size_t size;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    int fd = *(int *)userdata;
    size_t total = size * nmemb;
    if (send(fd, ptr, total, 0) < 0) return 0;
    return total;
}

static int recv_line(int fd, char **line) {
    size_t cap = 128, len = 0;
    char *buf = malloc(cap);
    if (!buf) return -1;
    while (1) {
        char c;
        if (recv(fd, &c, 1, 0) <= 0) {
            free(buf);
            return -1;
        }
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) return -1;
        }
        buf[len++] = c;
        if (len >= 2 && buf[len-2]=='\r' && buf[len-1]=='\n') break;
    }
    buf[len] = 0;
    *line = buf;
    return 0;
}

static int read_chunked(int fd, struct buffer *body) {
    body->data = NULL;
    body->size = 0;
    while (1) {
        char *line;
        if (recv_line(fd, &line)) return -1;
        size_t len = strtoul(line, 0, 16);
        free(line);
        if (!len) {
            recv_line(fd, &line);
            free(line);
            break;
        }
        char *tmp = realloc(body->data, body->size + len);
        if (!tmp) return -1;
        body->data = tmp;
        size_t r = 0;
        while (r < len) {
            ssize_t n = recv(fd, body->data + body->size + r, len - r, 0);
            if (n <= 0) return -1;
            r += n;
        }
        body->size += len;
        recv_line(fd, &line);
        free(line);
    }
    return 0;
}

static int read_body(int fd, size_t len, struct buffer *body) {
    body->data = malloc(len ? len : 1);
    body->size = len;
    size_t r = 0;
    while (r < len) {
        ssize_t n = recv(fd, body->data + r, len - r, 0);
        if (n <= 0) return -1;
        r += n;
    }
    return 0;
}

static int handle_request(int fd, CURL *curl) {
    char *line;
    if (recv_line(fd, &line)) return -1;
    char method[16], path[4096], version[16];
    if (sscanf(line, "%15s %4095s %15s", method, path, version) != 3) {
        free(line);
        return -1;
    }
    free(line);
    struct curl_slist *hdrs = NULL;
    int connection_close = 0;
    size_t content_length = 0;
    int chunked = 0;
    while (1) {
        if (recv_line(fd, &line)) {
            curl_slist_free_all(hdrs);
            return -1;
        }
        if (!strcmp(line, "\r\n")) { free(line); break; }
        char *p = strchr(line, ':');
        if (p) {
            *p++ = 0;
            while (*p==' '||*p=='\t') p++;
            if (!strcasecmp(line, "Content-Length")) content_length = strtoul(p,0,10);
            else if (!strcasecmp(line, "Transfer-Encoding") && !strncasecmp(p, "chunked",7)) chunked = 1;
            else if (!strcasecmp(line, "Connection") && !strncasecmp(p, "close",5)) connection_close = 1;
        }
        hdrs = curl_slist_append(hdrs, line);
        free(line);
    }
    struct buffer body = {0};
    if (chunked) {
        if (read_chunked(fd, &body)) { curl_slist_free_all(hdrs); return -1; }
    } else if (content_length) {
        if (read_body(fd, content_length, &body)) { curl_slist_free_all(hdrs); return -1; }
    }
    char url[8192];
    const char *host = NULL;
    struct curl_slist *h;
    for (h=hdrs; h; h=h->next) {
        if (!strncasecmp(h->data, "Host:",5)) {
            host = h->data + 5;
            while (*host==' '||*host=='\t') host++;
            break;
        }
    }
    if (!host) { curl_slist_free_all(hdrs); free(body.data); return -1; }
    snprintf(url, sizeof(url), "http://%s%s", host, path);
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_PROXY, UPSTREAM);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fd);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &fd);
    if (body.size) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data);
    }
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    free(body.data);
    if (res != CURLE_OK) return -1;
    return connection_close || !strcasecmp(version, "HTTP/1.0");
}

static void *client_thr(void *arg) {
    int fd = (int)(intptr_t)arg;
    CURL *curl = curl_easy_init();
    if (!curl) { close(fd); return NULL; }
    while (1) {
        int ret = handle_request(fd, curl);
        if (ret) break;
    }
    curl_easy_cleanup(curl);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return NULL;
}

int main() {
    curl_global_init(CURL_GLOBAL_ALL);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LISTEN_PORT);
    inet_pton(AF_INET, LISTEN_IP, &addr.sin_addr);
    bind(s, (struct sockaddr *)&addr, sizeof(addr));
    listen(s, 16);
    while (1) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        pthread_t t;
        pthread_create(&t, NULL, client_thr, (void *)(intptr_t)c);
        pthread_detach(t);
    }
    close(s);
    return 0;
}
