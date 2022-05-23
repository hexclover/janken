#include "common.h"
#include <stdarg.h>

static loglevel_t glob_loglevel = LOGLV_MAX;
#define LLVN_MAX_LEN 10

int parse_log_level(const char *s) {
#define X(lvl, LVL)                                                            \
    if (strncasecmp((s), #lvl, LLVN_MAX_LEN) == 0) {                           \
        return (LVL);                                                          \
    }
    for_valid_log_levels(X);
#undef X
    return -1;
}

void set_loglevel(loglevel_t lv) {
    glob_loglevel = min_(max_(lv, LOGLV_MIN), LOGLV_MAX);
}

static const char *lv_string(loglevel_t lv) {
#define X(_, LVL)                                                              \
    case LVL: {                                                                \
        return #LVL;                                                           \
        break;                                                                 \
    }
    switch (lv) {
        for_valid_log_levels(X);
    default:
        return "UNKNOWN";
        break;
    }
#undef X
}

static void log_internal(loglevel_t lv, const char *fmt, va_list arg) {
    time_t tm = time(NULL);
    struct tm tmp;
    gmtime_r(&tm, &tmp);
    char timestamp[64] = {'\0'};
    strftime(timestamp, sizeof(timestamp), "%FT%TZ", &tmp);
    char msg[256] = {'\0'};
    vsnprintf(msg, sizeof(msg), fmt, arg);
    fprintf(stderr, "%s %s: %s\n", timestamp, lv_string(lv), msg);
}

#define make_log_func(lvl, LVL)                                                \
    void log_##lvl(const char *fmt, ...) {                                     \
        if (glob_loglevel < (LVL))                                             \
            return;                                                            \
        va_list arg;                                                           \
        va_start(arg, fmt);                                                    \
        log_internal((LVL), fmt, arg);                                         \
        va_end(arg);                                                           \
    }
for_valid_log_levels(make_log_func)
