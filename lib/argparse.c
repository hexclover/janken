#include "common.h"
#include <getopt.h>
const static struct option long_options[] = {
    {"help", optional_argument, 0, 'h'},
    {"port", required_argument, 0, 'p'},
    {"loglevel", required_argument, 0, 'l'},
    {0, 0, 0, 0}};

// TODO: generate help
noreturn void display_help(bool err, const char *appname,
                           const char *address_str) {
    fprintf(err ? stderr : stdout,
            "Usage: %s [-h|--help] [-l|--loglevel LOGLEVEL] [-p|--port PORT] "
            "[%s]\n",
            appname, address_str);
    exit(err ? 1 : 0);
}

void parse_args(int argc, char **argv, char *addr, size_t addr_len,
                uint32_t *port, const char *appname, const char *addr_desc) {
    int c;
    bool error = true;

    while (1) {
        int ind;
        c = getopt_long(argc, argv, "h::l:p:", long_options, &ind);
        if (c == -1)
            break;

        switch (c) {
        case 'h':
            error = false;
            goto help;
            break;
        case 'l': {
            int lvl = parse_log_level(optarg);
            if (lvl > LOGLV_MIN && lvl < LOGLV_MAX) {
                set_loglevel(lvl);
            } else {
                fprintf(stderr, "Invalid log level: %s\n", optarg);
                goto help;
            }
        } break;
        case 'p': {
            char *endptr;
            long p = strtol(optarg, &endptr, 10);
            if (*optarg != '\0' && *endptr == '\0') {
                if (p >= 1 && p <= 65535) {
                    *port = p;
                } else {
                    fprintf(stderr, "Port number out of range: %ld\n", p);
                    goto help;
                }
            } else {
                fprintf(stderr, "Invalid character in port: %c\n", *endptr);
                goto help;
            }
        } break;
        case '?':
            goto help;
            break;
        }
    }

    bool got_addr = false;
    for (int i = optind; i < argc; ++i) {
        if (!got_addr) {
            if (strnlen(argv[i], addr_len) + 1 >= addr_len) {
                fprintf(stderr, "Address too long: %s\n", argv[i]);
                goto help;
            } else {
                snprintf(addr, addr_len, "%s", argv[i]);
            }
        } else {
            fprintf(stderr, "Superfluous argument -- `%s'\n", argv[i]);
            goto help;
        }
    }

    return;
help:
    display_help(error, appname, addr_desc);
}
