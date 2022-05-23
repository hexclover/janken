#include "common.h"

#define check_nt(pmsg, var, fld)                                               \
    do {                                                                       \
        if (!null_terminated((pmsg)->body.var.fld,                             \
                             sizeof((pmsg)->body.var.fld)))                    \
            return -1;                                                         \
    } while (0)
#define conv(fld)                                                              \
    do {                                                                       \
        static_assert(sizeof(fld) == 1 || sizeof(fld) == 2 ||                  \
                          sizeof(fld) == 4,                                    \
                      "Field to convert must be 1, 2 or 4 bytes long");        \
        if (sizeof(fld) == 1) {                                                \
            ;                                                                  \
        } else if (sizeof(fld) == 2) {                                         \
            if (l2n) {                                                         \
                fld = htons(fld);                                              \
            } else {                                                           \
                fld = ntohs(fld);                                              \
            }                                                                  \
        } else if (sizeof(fld) == 4) {                                         \
            if (l2n) {                                                         \
                fld = htonl(fld);                                              \
            } else {                                                           \
                fld = ntohl(fld);                                              \
            }                                                                  \
        }                                                                      \
    } while (0)
#define msg_body_conv                                                          \
    do {                                                                       \
        switch (kind) {                                                        \
        case JOIN:                                                             \
            break;                                                             \
        case JOIN_R:                                                           \
            conv(body->join_r.error);                                          \
            conv(body->join_r.id);                                             \
            conv(body->join_r.key);                                            \
            break;                                                             \
        case QUIT:                                                             \
            conv(body->quit.id);                                               \
            conv(body->quit.key);                                              \
            break;                                                             \
        case UCHANGE:                                                          \
            conv(body->uchange.count);                                         \
            for (size_t i = 0;                                                 \
                 i < body->uchange.count && i < UCHANGE_MAX_UCNT; ++i) {       \
                conv(body->uchange.users[i].id);                               \
                conv(body->uchange.users[i].state);                            \
            }                                                                  \
            break;                                                             \
        case CHALLENGE:                                                        \
            conv(body->challenge.id1);                                         \
            conv(body->challenge.id2);                                         \
            conv(body->challenge.key);                                         \
            conv(body->challenge.chid);                                        \
            conv(body->challenge.action);                                      \
            break;                                                             \
        case CHALLENGE_R:                                                      \
            conv(body->challenge_r.error);                                     \
            conv(body->challenge_r.id1);                                       \
            conv(body->challenge_r.id2);                                       \
            conv(body->challenge_r.chid);                                      \
            conv(body->challenge_r.is_id1);                                    \
            break;                                                             \
        case TURN:                                                             \
            conv(body->turn.user);                                             \
            conv(body->turn.chid);                                             \
            conv(body->turn.key);                                              \
            conv(body->turn.turn_no);                                          \
            conv(body->turn.action);                                           \
            break;                                                             \
        case TURN_R:                                                           \
            conv(body->turn_r.chid);                                           \
            conv(body->turn_r.turn_no);                                        \
            conv(body->turn_r.action1);                                        \
            conv(body->turn_r.action2);                                        \
            conv(body->turn_r.hp1);                                            \
            conv(body->turn_r.hp2);                                            \
            conv(body->turn_r.maxhp1);                                         \
            conv(body->turn_r.maxhp2);                                         \
            conv(body->turn_r.fin);                                            \
            conv(body->turn_r.winner);                                         \
            break;                                                             \
        case SENDMSG:                                                          \
            conv(body->sendmsg.id);                                            \
            conv(body->sendmsg.key);                                           \
            break;                                                             \
        default:                                                               \
            assert(0);                                                         \
            break;                                                             \
        }                                                                      \
    } while (0)
#define msg_head_conv                                                          \
    do {                                                                       \
        conv(head->kind);                                                      \
        conv(head->body_len);                                                  \
    } while (0)

const char *msg_strerror(msg_err_t err) {
    const static char *msg_err_desc[] = {"OK",
                                         "Duplicate nickname",
                                         "Non-existence nickname",
                                         "Illegal nickname",
                                         "Non-existent ID",
                                         "Incorrect key",
                                         "Non-existent challenge ID",
                                         "Too many users online",
                                         "Joining twice on the same client",
                                         "User is engaged in another battle",
                                         "Challenging onself",
                                         "Invalid argument",
                                         "Challenge is rejected",
                                         "Challenge has been cancelled",
                                         "Other errors"};
    static_assert(ARRAY_SIZE(msg_err_desc) == ME_OTHER - ME_OK + 1, "");

    if (err >= ME_OK && err <= ME_OTHER) {
        return msg_err_desc[err - ME_OK];
    }
    return msg_err_desc[ME_OTHER];
}

static size_t msg_body_size(msg_kind_t kind) {
    switch (kind) {
    case JOIN:
        return sizeof(msg_join_t);
        break;
    case JOIN_R:
        return sizeof(msg_join_r_t);
        break;
    case QUIT:
        return sizeof(msg_quit_t);
        break;
    case UCHANGE:
        return sizeof(msg_uchange_t);
        break;
    case CHALLENGE:
        return sizeof(msg_challenge_t);
        break;
    case CHALLENGE_R:
        return sizeof(msg_challenge_r_t);
        break;
    case TURN:
        return sizeof(msg_turn_t);
        break;
    case TURN_R:
        return sizeof(msg_turn_r_t);
        break;
    case SENDMSG:
        return sizeof(msg_sendmsg_t);
        break;
    case MSG_MAX:
        return 0;
        break;
    }
    log_error("Please implement me (kind = %d): %s:%d", kind, __FILE__,
              __LINE__);
    return 0;
}

int msg_check_form(const struct message_t *buf) {
    if (buf->head.kind <= 0 || buf->head.kind >= MSG_MAX)
        return -1;
    if (buf->head.body_len != msg_body_size(buf->head.kind))
        return -1;
    switch ((msg_kind_t)buf->head.kind) {
    case JOIN:
        check_nt(buf, join, nickname);
        break;
    case JOIN_R:
        check_nt(buf, join_r, nickname);
        break;
    case UCHANGE:
        if (buf->body.uchange.count > UCHANGE_MAX_UCNT) {
            return -1;
        }
        for (size_t i = 0; i < buf->body.uchange.count; ++i) {
            check_nt(buf, uchange.users[i], nickname);
        }
        break;
    case SENDMSG:
        check_nt(buf, sendmsg, text);
        break;
    case QUIT:
    case CHALLENGE:
    case CHALLENGE_R:
    case TURN:
    case TURN_R:
        break;
    case MSG_MAX:
    default:
        return -1;
        break;
    }
    return 0;
}

void init_msg_buf(message_t *msg, msg_kind_t kind) {
    memset(msg, 0, sizeof(*msg));
    msg->head.kind = kind;
    msg->head.body_len = msg_body_size(kind);
}

message_t *make_msg_buf(msg_kind_t kind) {
    message_t *msg = xmalloc(sizeof(*msg));
    init_msg_buf(msg, kind);
    return msg;
}

static void msg_head_n2l(msg_head_t *head) {
    const bool l2n = false;
    msg_head_conv;
}

static void msg_head_l2n(msg_head_t *head) {
    const bool l2n = true;
    msg_head_conv;
}

static void msg_body_n2l(msg_kind_t kind, msg_body_t *body) {
    const bool l2n = false;
    msg_body_conv;
}

static void msg_body_l2n(msg_kind_t kind, msg_body_t *body) {
    const bool l2n = true;
    msg_body_conv;
}

int msg_recv(int fd, struct message_t *buf, bool block_at_head) {
    if (recv_count(fd, buf, sizeof(buf->head), block_at_head) != 0) {
        return -1;
    }
    msg_head_n2l(&buf->head);
    if (buf->head.kind <= 0 || buf->head.kind >= MSG_MAX) {
        return -1;
    }
    size_t body_len = buf->head.body_len;
    if (body_len != msg_body_size(buf->head.kind)) {
        return -1;
    }
    if (recv_count(fd, &buf->body, body_len, true) != 0) {
        return -1;
    }
    msg_body_n2l(buf->head.kind, &buf->body);
    return msg_check_form(buf);
}

int msg_send(int fd, const struct message_t *orig) {
    message_t buf;
    static_assert(sizeof(buf) == sizeof(*orig), "");
    size_t body_len = orig->head.body_len;
    if (orig->head.kind <= 0 || orig->head.kind >= MSG_MAX) {
        return -1;
    }
    size_t sz = sizeof(orig->head) + body_len;
    memcpy(&buf, orig, sz);
    msg_body_l2n(buf.head.kind, &buf.body);
    msg_head_l2n(&buf.head);
    return send(fd, &buf, sz, 0) == sz ? 0 : -1;
}

message_t *msg_dup(const message_t *orig) {
    assert(orig);
    message_t *msg = make_msg_buf(orig->head.kind);
    memcpy(msg, orig,
           min_(sizeof(orig->head) + orig->head.body_len, sizeof(*orig)));
    return msg;
}

message_t *make_join(const char *nickname) {
    message_t *msg = make_msg_buf(JOIN);
    snprintf(msg->body.join.nickname, NICKNAME_LEN, "%s", nickname);
    return msg;
}

message_t *make_join_r(const char *nickname, uint16_t error, uint16_t id,
                       uint32_t key) {
    message_t *msg = make_msg_buf(JOIN_R);
    snprintf(msg->body.join_r.nickname, NICKNAME_LEN, "%s", nickname);
    msg->body.join_r.error = error;
    msg->body.join_r.id = id;
    msg->body.join_r.key = key;
    return msg;
}

message_t *make_uchange() {
    message_t *msg = make_msg_buf(UCHANGE);
    msg->body.uchange.count = 0;
    return msg;
}

message_t *make_challenge() {
    message_t *msg = make_msg_buf(CHALLENGE);
    return msg;
}

void init_challenge_r(message_t *msg) { init_msg_buf(msg, CHALLENGE_R); }

bool uchange_add_or_create(message_t *msg, message_t **newmsg,
                           const char *nickname, uint16_t id,
                           user_state_t state, int32_t score) {
    uint32_t cnt = msg->body.uchange.count;
    if (cnt >= UCHANGE_MAX_UCNT) {
        if (newmsg) {
            *newmsg = make_uchange();
            uchange_add_or_create(*newmsg, NULL, nickname, id, state, score);
        }
        return false;
    } else {
        msg->body.uchange.users[cnt].id = id;
        snprintf(msg->body.uchange.users[cnt].nickname, NICKNAME_LEN, "%s",
                 nickname);
        msg->body.uchange.users[cnt].state = state;
        msg->body.uchange.users[cnt].score = score;
        ++msg->body.uchange.count;
        return true;
    }
}
