#ifndef __GAME_COMMON_H
#define __GAME_COMMON_H
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <search.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#define NICKNAME_LEN 32
#define ADDR_MAX_LEN 128
#define DEFAULT_PORT 22502
#define UCHANGE_MAX_UCNT 16

#define ARRAY_SIZE(a) (sizeof((a)) / sizeof((a)[0]))
#define max_(a, b) ((a) < (b) ? (b) : (a))
#define min_(a, b) ((a) > (b) ? (b) : (a))

inline void *deref_or_null(void **p) { return p ? *p : NULL; }

#define panic(msg, ...)                                                        \
    do {                                                                       \
        fprintf(stderr, msg "\n", ##__VA_ARGS__);                              \
        exit(1);                                                               \
    } while (0)
#define ppanic(msg, ...)                                                       \
    do {                                                                       \
        fprintf(stderr, msg ": %s\n", ##__VA_ARGS__, strerror(errno));         \
        exit(1);                                                               \
    } while (0)
#define for_valid_log_levels(X)                                                \
    X(fatal, FATAL)                                                            \
    X(error, ERROR)                                                            \
    X(warning, WARNING) X(info, INFO) X(debug, DEBUG) X(trace, TRACE)
typedef enum loglevel_t {
    // clang-format off
    LOGLV_MIN = 0,
#define define_loglevel(_, LVL) LVL,
    for_valid_log_levels(define_loglevel)
#undef define_loglevel
    LOGLV_MAX
    // clang-format on
} loglevel_t;

// clang-format off
#define declare_log_func(lvl, _)                                               \
    void log_##lvl(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
for_valid_log_levels(declare_log_func)
#undef declare_log_func
int parse_log_level(const char *);
void set_loglevel(loglevel_t);
// clang-format on

typedef enum user_state_t { UOFFLINE = 0, UONLINE, UBATTLING } user_state_t;

typedef struct user_info_t {
    char nickname[NICKNAME_LEN];
    uint16_t id;
    int32_t score;
    user_state_t state;
#ifdef __IS_SERVER
    int fd;
    uint32_t key;
    uint16_t chid;
#endif
} user_info_t;

typedef enum battle_act_t { B_ROCK, B_PAPER, B_SCISSORS } battle_act_t;
typedef enum turn_result_t { TR_WIN, TR_LOSE, TR_EVEN } turn_result_t;

#ifdef __IS_SERVER
typedef enum challenge_state_t { ASKING, STARTED } challenge_state_t;

typedef struct challenge_t {
    // User1 is the one who initiates the fighting
    uint16_t id;
    uint16_t user1, user2;
    // Expected next turn
    uint16_t turn_no;
    // Valid if turn_no > 1
    bool acted1, acted2;
    battle_act_t act1, act2;
    int32_t hp1, hp2, maxhp1, maxhp2;
    challenge_state_t state;
} challenge_t;
#endif

typedef enum msg_kind_t {
    JOIN = 1,
    JOIN_R,
    QUIT,
    UCHANGE,
    CHALLENGE,
    CHALLENGE_R,
    TURN,
    TURN_R,
    SENDMSG,
    MSG_MAX
} msg_kind_t;

typedef enum msg_err_t {
    ME_OK = 0,
    DUPNICK,
    NXNICK,
    ILLNICK,
    NXID,
    ICKEY,
    NXCHID,
    TOOMANYUSER,
    JOINTWICE,
    ENGAGED,
    CHLSELF,
    INVARG,
    REJECTED,
    CANCELLED,
    ME_OTHER
} msg_err_t;

inline turn_result_t get_turn_result(battle_act_t a, battle_act_t b) {
    if ((a == B_ROCK && b == B_SCISSORS) || (a == B_PAPER && b == B_ROCK) ||
        (a == B_SCISSORS && b == B_PAPER)) {
        return TR_WIN;
    }
    if ((b == B_ROCK && a == B_SCISSORS) || (b == B_PAPER && a == B_ROCK) ||
        (b == B_SCISSORS && a == B_PAPER)) {
        return TR_LOSE;
    }
    return TR_EVEN;
}

inline const char *act_name(battle_act_t act) {
    switch (act) {
    case B_ROCK:
        return "Rock";
    case B_PAPER:
        return "Paper";
    case B_SCISSORS:
        return "Scissors";
    }
    return "<unknown>";
}

const char *msg_strerror(msg_err_t);

typedef struct msg_head_t {
    uint16_t kind;
    uint16_t body_len;
} __attribute__((packed)) msg_head_t;

typedef struct msg_join_t {
    char nickname[NICKNAME_LEN];
} __attribute__((packed)) msg_join_t;

typedef struct msg_join_r_t {
    char nickname[NICKNAME_LEN];
    uint16_t error;
    uint16_t id;
    uint32_t key;
} __attribute__((packed)) msg_join_r_t;

typedef struct msg_quit_t {
    uint32_t key;
    uint16_t id;
} __attribute__((packed)) msg_quit_t;

typedef struct msg_uchange_t {
    uint32_t count;
    struct {
        char nickname[NICKNAME_LEN];
        uint16_t id;
        uint16_t state;
        int32_t score;
    } users[UCHANGE_MAX_UCNT];
} __attribute__((packed)) msg_uchange_t;

typedef enum challenge_action_t {
    C_START,
    C_REJECT,
    C_ACCEPT,
    C_CANCEL
} challenge_action_t;

typedef struct msg_challenge_t {
    uint16_t id1, id2;
    uint32_t key;
    uint16_t chid;
    int16_t action;
} __attribute__((packed)) msg_challenge_t;

typedef struct msg_challenge_r_t {
    uint16_t error;
    uint16_t id1, id2;
    uint16_t chid;
    uint8_t is_id1;
} __attribute__((packed)) msg_challenge_r_t;

typedef struct msg_turn_t {
    uint16_t user;
    uint16_t chid;
    uint32_t key;
    uint16_t turn_no;
    uint16_t action;
} __attribute__((packed)) msg_turn_t;

typedef struct msg_turn_r_t {
    uint16_t chid;
    uint16_t turn_no;
    uint16_t action1, action2;
    uint16_t winner;
    int32_t hp1, hp2, maxhp1, maxhp2;
    uint8_t fin;
} __attribute__((packed)) msg_turn_r_t;

typedef struct msg_sendmsg_t {
    uint16_t id;
    uint32_t key;
    char text[128];
} __attribute__((packed)) msg_sendmsg_t;

typedef union msg_body_t {
    msg_join_t join;
    msg_join_r_t join_r;
    msg_quit_t quit;
    msg_uchange_t uchange;
    msg_challenge_t challenge;
    msg_challenge_r_t challenge_r;
    msg_turn_t turn;
    msg_turn_r_t turn_r;
    msg_sendmsg_t sendmsg;
} __attribute__((packed)) msg_body_t;

typedef struct message_t {
    msg_head_t head;
    msg_body_t body;
} __attribute__((packed)) message_t;

int recv_count(int fd, void *buf, size_t len, bool wait);
int msg_check_form(const struct message_t *buf);
int msg_recv(int fd, struct message_t *buf, bool block_at_head);
/* buf should be in host byte order. */
int msg_send(int fd, const message_t *buf);

/* Initializes the head. Zero-initializes the body. */
void init_msg_buf(message_t *, msg_kind_t);
/* Initializes the head. Zero-initializes the body. */
message_t *make_msg_buf(msg_kind_t);
message_t *msg_dup(const message_t *);
message_t *make_join(const char *nickname);
message_t *make_join_r(const char *nickname, uint16_t error, uint16_t id,
                       uint32_t key);
message_t *make_uchange();
/* Returns false if original msg was full, and in this case it stores pointer to
 * a new message in newmsg and add this user into that new message; returns true
 * otherwise
 */
bool uchange_add_or_create(message_t *msg, message_t **newmsg,
                           const char *nickname, uint16_t id,
                           user_state_t state, int32_t score);
message_t *make_challenge();
message_t *make_challenge_r();
void init_challenge_r();

typedef enum queue_err_t { QOK = 0, QMEM, QFULL, QEMPTY } queue_err_t;

typedef struct queue_t queue_t;
queue_t *queue_create(size_t cap);
queue_err_t queue_add(queue_t *, void *, bool block);
queue_err_t queue_take(queue_t *, void **, bool block);
void queue_destroy(queue_t *);

bool null_terminated(const char *str, size_t maxlen);
bool is_nickchar(char);
bool is_nickstr(const char *);
int nick_cmp(const char *, const char *);

static inline int cmp_by_id(const void *u, const void *v) {
    const user_info_t *uu = u, *uv = v;
    return (uu->id > uv->id) - (uu->id < uv->id);
}
static inline int cmp_by_nick(const void *u, const void *v) {
    const user_info_t *uu = u, *uv = v;
    return nick_cmp(uu->nickname, uv->nickname);
}
#ifdef __IS_SERVER
static inline int cmp_by_fd(const void *u, const void *v) {
    const user_info_t *uu = u, *uv = v;
    return (uu->fd > uv->fd) - (uu->fd < uv->fd);
}
static inline int cmp_by_chid(const void *c, const void *d) {
    const challenge_t *cc = c, *dd = d;
    return (cc->id > dd->id) - (cc->id < dd->id);
}
#endif

void *xmalloc(size_t sz);
void *xcalloc(size_t nmemb, size_t sz);

noreturn void display_help(bool err, const char *appname,
                           const char *address_str);
void parse_args(int argc, char **argv, char *addr, size_t addr_len,
                uint32_t *port, const char *appname, const char *addr_desc);
#endif
