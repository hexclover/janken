#include "common.h"

int recv_count(int fd, void *buf, size_t len, bool wait) {
    char *p = buf;
    while (len) {
        ssize_t cnt = recv(fd, p, len, wait ? 0 : MSG_DONTWAIT);
        if (cnt > 0) {
            len -= cnt;
        } else {
            return -1;
        }
    }
    return 0;
}

bool null_terminated(const char *str, size_t maxlen) {
    return strnlen(str, maxlen) < maxlen;
}

bool is_nickchar(char c) { return isalnum(c) || ispunct(c); }

bool is_nickstr(const char *s) {
    if (s[0] == '\0' || strnlen(s, NICKNAME_LEN) == NICKNAME_LEN)
        return false;
    while (*s) {
        if (!is_nickchar(*s))
            return false;
        ++s;
    }
    return true;
}

int nick_cmp(const char *s, const char *t) {
    return strncasecmp(s, t, NICKNAME_LEN);
}

void *xmalloc(size_t sz) {
    sz = max_(sz, 1);
    void *p = malloc(sz);
    if (p)
        return p;
    ppanic("Memory allocation failed");
}

void *xcalloc(size_t nmemb, size_t sz) {
    nmemb = max_(nmemb, 1);
    sz = max_(sz, 1);
    void *p = calloc(nmemb, sz);
    if (p)
        return p;
    ppanic("Memory allocation failed");
}
