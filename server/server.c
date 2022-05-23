#define __IS_SERVER
#include "lib/common.h"
#include <netinet/in.h>
#include <stdbool.h>
#include <time.h>
const char *APPNAME = "game_server";
#define DEFAULT_LISTEN_ADDRESS "0.0.0.0"
#define MAX_USER_COUNT 1024
#define MAX_QUEUE_SIZE 65536
#define MAXHP 10

static void *user_by_id, *user_by_fd, *user_by_nick, *ch_by_id;
static size_t user_cnt = 0;

typedef struct send_uinfo_wkst_t {
    enum { MSG_TO_ALL, ALL_TO_ONE } type;
    union {
        int to_fd;
        int except_fd;
    };
    message_t *msg;
} send_uinfo_wkst_t;

static void send_user_info(const void *pnode, VISIT visit, void *parg) {
    if (visit != postorder && visit != leaf)
        return;

    send_uinfo_wkst_t *arg = parg;
    const user_info_t *user = *(const user_info_t **)pnode;
    switch (arg->type) {
    case ALL_TO_ONE: {
        assert(user->fd >= 0);
        // Send all users' info to the user at arg->to_fd
        if (arg->msg == NULL) {
            arg->msg = make_uchange();
        }
        message_t *newmsg = NULL;
        if (!uchange_add_or_create(arg->msg, &newmsg, user->nickname, user->id,
                                   user->state, user->score)) {
            // Must send the old info
            log_info("Sending info of user %u to fd %d", user->id, arg->to_fd);
            msg_send(arg->to_fd, arg->msg);
            free(arg->msg);
            arg->msg = newmsg;
        }
    } break;
    case MSG_TO_ALL: {
        // Send arg->msg to every user except arg->except_fd
        if (user->fd != arg->except_fd) {
            log_info("Broadcasting to fd %d (%s)", user->fd, user->nickname);
            msg_send(user->fd, arg->msg);
        }
    } break;
    }
}

static void broadcast_user_changes(user_info_t *usr1, user_info_t *usr2) {
    message_t *uchange = make_uchange();
    static_assert(UCHANGE_MAX_UCNT >= 2, "");
    user_info_t *u[2] = {usr1, usr2};
    for (int i = 0; i < ARRAY_SIZE(u); ++i) {
        if (u[i]) {
            uchange_add_or_create(uchange, NULL, u[i]->nickname, u[i]->id,
                                  u[i]->state, u[i]->score);
        }
    }
    send_uinfo_wkst_t arg = {
        .type = MSG_TO_ALL, .msg = uchange, .except_fd = -1};
    twalk_r(user_by_id, send_user_info, &arg);
    free(uchange);
}

typedef struct queue_entry_t {
    enum { EMSG, EDISCONN } kind;
    int fd;
    union {
        message_t *msg;
    };
} queue_entry_t;
static queue_t *incoming_queue = NULL;

typedef struct serve_arg_t {
    int fd;
    struct sockaddr_in sin;
} serve_arg_t;

static void random_init() {
    static bool ok = false;
    if (!ok) {
        time_t epoch = time(NULL);
        srandom(epoch);
        ok = true;
    }
}

static uint32_t random_key() {
    uint32_t ret = random();
    return random() % 2 ? ret : ~ret;
}

static void build_inet_addr(const char *address, uint32_t port,
                            struct sockaddr_in *sin) {
    struct in_addr saddr;
    if (inet_aton(address, &saddr) == 0) {
        ppanic("Invalid server IP address: %s", address);
    }

    sin->sin_addr = saddr;
    sin->sin_port = htons(port);
    sin->sin_family = AF_INET;
}

static int do_listen(const char *address, uint32_t port) {
    struct sockaddr_in sin;
    build_inet_addr(address, port, &sin);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        ppanic("socket()");
    }

    if (bind(fd, (const struct sockaddr *)&sin, sizeof(sin)) != 0) {
        ppanic("bind()");
    }
    if (listen(fd, 4096) != 0) {
        ppanic("listen()");
    }

    return fd;
}

static void *serve(void *parg) {
    serve_arg_t arg = *(serve_arg_t *)parg;
    free(parg);
    parg = NULL;

    int fd = arg.fd;
    char addr_buf[64];
    {
        char *addr = inet_ntoa(arg.sin.sin_addr);
        snprintf(addr_buf, sizeof(addr_buf), "%s:%u", addr,
                 ntohs(arg.sin.sin_port));
    }
    log_info("Accepted connection from %s", addr_buf);

    while (1) {
        message_t buf;
        if (msg_recv(fd, &buf, true) == 0) {
            log_info("Received packet; enqueueing it...");
            queue_entry_t *pq = xmalloc(sizeof(*pq));
            pq->kind = EMSG;
            pq->fd = fd;
            pq->msg = xmalloc(sizeof(*pq->msg));
            memcpy(pq->msg, &buf, sizeof(*pq->msg));
            queue_add(incoming_queue, pq, true);
        } else {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                log_error(
                    "Received corrupted packet from %s (%s); must shutdown...",
                    addr_buf, strerror(errno));
                break;
            }
        }
    }
    log_info("Disconnected from %s", addr_buf);
    queue_entry_t *pq = xmalloc(sizeof(*pq));
    pq->kind = EDISCONN;
    pq->fd = fd;
    queue_add(incoming_queue, pq, true);
    return 0;
}

static void model_init() {
    user_cnt = 0;
    user_by_id = user_by_fd = user_by_nick = ch_by_id = NULL;
}

// Copies nickname
static user_info_t *user_create(const char *nickname) {
    user_info_t *user = xmalloc(sizeof(*user));
    user->chid = user->id = 0;
    user->key = random_key();
    user->state = UONLINE;
    user->score = 0;
    // user->nickname = xmalloc(NICKNAME_LEN);
    snprintf(user->nickname, NICKNAME_LEN, "%s", nickname);
    return user;
}

static void user_destroy(user_info_t *user) {
    // free(user->nickname);
    free(user);
}

static user_info_t *user_add(int fd, const char *nickname, msg_err_t *err) {
    user_info_t *user = user_create(nickname);

    if (user_cnt >= MAX_USER_COUNT) {
        *err = TOOMANYUSER;
        goto free;
    }

    if (tfind(user, &user_by_fd, cmp_by_fd)) {
        *err = JOINTWICE;
        goto free;
    }

    if (tfind(user, &user_by_nick, cmp_by_nick)) {
        *err = DUPNICK;
        goto free;
    }

    uint16_t id = fd % UINT16_MAX;
    for (int _ = 0; _ < 16; ++_) {
        if (tfind(user, &user_by_id, cmp_by_id)) {
            id = (id + random()) % UINT16_MAX;
            continue;
        }

        user->id = id;
        user->fd = fd;
        user_info_t **ptr;
        // map id to user
        ptr = tsearch(user, &user_by_id, cmp_by_id);
        assert(*ptr == user);
        // map fd to user
        ptr = tsearch(user, &user_by_fd, cmp_by_fd);
        assert(*ptr == user);
        // map nickname to user
        ptr = tsearch(user, &user_by_nick, cmp_by_nick);
        assert(*ptr == user);

        ++user_cnt;
        *err = ME_OK;
        return user;
    }

    log_error("Could not allocate ID");
    *err = ME_OTHER;
    return NULL;

free:
    user_destroy(user);
    return NULL;
}

static challenge_t *add_challenge() {
    challenge_t *ch = xcalloc(1, sizeof(*ch));
    ch->state = ASKING;

    for (int _ = 0; _ < 16; ++_) {
        ch->id = random() % UINT16_MAX;
        if (ch->id == 0)
            ch->id = 1;
        if (tfind(ch, &ch_by_id, cmp_by_chid)) {
            continue;
        }
        challenge_t **node = tsearch(ch, &ch_by_id, cmp_by_chid);
        assert(*node == ch);
        return ch;
    }

    log_error("Could not allocate ID");
    assert(0);
    return NULL;
}

static void user_del_and_destroy(user_info_t *user) {
    user_info_t **node = tfind(user, &user_by_id, cmp_by_id);
    if (node) {
        user_info_t *user = *node;
        assert(user);
        if (tdelete(user, &user_by_id, cmp_by_id) == 0)
            assert(0);
        if (tdelete(user, &user_by_nick, cmp_by_nick) == 0)
            assert(0);
        if (tdelete(user, &user_by_fd, cmp_by_fd) == 0)
            log_error("Cannot unmap fd %d", user->fd);
        user_destroy(user);
        --user_cnt;
    }
}

static void handle_join(int fd, msg_join_t *join) {
    msg_err_t err;
    user_info_t *user = user_add(fd, join->nickname, &err);

    {
        message_t *msg;
        if (err == ME_OK) {
            assert(user);
            msg = make_join_r(join->nickname, ME_OK, user->id, user->key);
        } else {
            msg = make_join_r(join->nickname, err, 0, 0);
        }

        log_info("JOIN from %d: nickname = %s, err = %d, id = %d", fd,
                 join->nickname, msg->body.join_r.error, msg->body.join_r.id);
        msg_send(fd, msg);
        free(msg);
    }

    if (err == ME_OK) {
        // Send current all users' information to this new user
        {
            send_uinfo_wkst_t st = {
                .type = ALL_TO_ONE, .to_fd = fd, .msg = NULL};
            twalk_r(user_by_id, send_user_info, &st);
            if (st.msg) {
                log_info("Sending info of user %u (at %d) to fd %d", user->id,
                         user->fd, fd);
                msg_send(fd, st.msg);
                free(st.msg);
            }
        }
        // Send this new user's information to all users
        {
            send_uinfo_wkst_t st = {
                .type = MSG_TO_ALL, .except_fd = fd, .msg = make_uchange()};
            uchange_add_or_create(st.msg, NULL, user->nickname, user->id,
                                  user->state, user->score);
            twalk_r(user_by_id, send_user_info, &st);
            free(st.msg);
        }
    }
}

static void judge_turn(challenge_t *ch, int32_t force_lose_id) {
    bool fin = false;
    uint16_t winner;
    if (ch->turn_no == 0) {
        ch->acted1 = ch->acted2 = false;
        ch->turn_no = 1;
    } else {
        ++ch->turn_no;
    }
    if (ch->acted1 && ch->acted2) {
        int32_t damage = random() % 5 + 1;
        int32_t d1 = 0, d2 = 0;
        switch (get_turn_result(ch->act1, ch->act2)) {
        case TR_WIN:
            d1 = 0, d2 = damage;
            break;
        case TR_LOSE:
            d1 = damage, d2 = 0;
            break;
        default:
            d1 = d2 = 0;
            break;
        }
        ch->hp1 -= d1;
        ch->hp2 -= d2;
    }
    ch->acted1 = ch->acted2 = false;
    if (force_lose_id == ch->user1) {
        fin = true;
        winner = ch->user2;
    } else if (force_lose_id == ch->user2) {
        fin = true;
        winner = ch->user1;
    } else {
        if (ch->hp1 <= 0) {
            fin = true;
            winner = ch->user2;
        } else if (ch->hp2 <= 0) {
            fin = true;
            winner = ch->user1;
        }
    }

    message_t msg;
    init_msg_buf(&msg, TURN_R);
    msg_turn_r_t turn_r = {.turn_no = ch->turn_no,
                           .action1 = ch->act1,
                           .action2 = ch->act2,
                           .hp1 = ch->hp1,
                           .hp2 = ch->hp2,
                           .maxhp1 = ch->maxhp1,
                           .maxhp2 = ch->maxhp2,
                           .fin = fin};
    if (fin) {
        turn_r.winner = winner;
    }
    msg.body.turn_r = turn_r;
    user_info_t *user1, *user2;
    {
        user_info_t tmp = {.id = ch->user1};
        user1 = deref_or_null(tfind(&tmp, &user_by_id, cmp_by_id));
        tmp.id = ch->user2;
        user2 = deref_or_null(tfind(&tmp, &user_by_id, cmp_by_id));
    }

    msg_send(user1->fd, &msg);
    msg_send(user2->fd, &msg);

    if (fin) {
        // change scores and user states
        int bonus = random() % 4 + 1, penalty = random() % 4;
        if (user1->id == winner) {
            user1->score += bonus;
            user2->score -= penalty;
        } else {
            user2->score += bonus;
            user1->score -= penalty;
        }
        if (user1->state == UBATTLING)
            user1->state = UONLINE;
        if (user2->state == UBATTLING)
            user2->state = UONLINE;
        broadcast_user_changes(user1, user2);
        // delete challenge
        tdelete(ch, &ch_by_id, cmp_by_chid);
    }
}

static void quit_user(struct user_info_t *user) {
    assert(user_by_id);
    if (user->state == UBATTLING) {
        challenge_t *ch;
        {
            challenge_t tmp = {.id = user->chid};
            ch = deref_or_null(tfind(&tmp, &ch_by_id, cmp_by_chid));
        }
        if (ch) {
            judge_turn(ch, user->id);
        }
    }
    user->state = UOFFLINE;
    broadcast_user_changes(user, NULL);
    user_del_and_destroy(user);
}

static void handle_disconnect(int fd) {
    user_info_t tmp = {.fd = fd};
    user_info_t *user = deref_or_null(tfind(&tmp, &user_by_fd, cmp_by_fd));
    if (user) {
        quit_user(user);
    }
}

static void handle_quit(msg_quit_t *quit) {
    user_info_t tmp = {.id = quit->id};
    user_info_t **node = tfind(&tmp, &user_by_id, cmp_by_id);
    if (node == NULL) {
        log_warning("Ignored QUIT for unknown user: %u", quit->id);
    } else if ((*node)->key != quit->key) {
        log_warning("Ignored QUIT for user %u (%s): Incorrect key", quit->id,
                    (*node)->nickname);
    } else {
        quit_user(*node);
    }
}

static void handle_challenge(int fd, msg_challenge_t *challenge) {
    log_debug("Handling challenge");
    message_t msg;
    init_challenge_r(&msg);
    msg.body.challenge_r.error = ME_OTHER;
    msg.body.challenge_r.chid = challenge->chid;
    msg.body.challenge_r.id1 = challenge->id1;
    msg.body.challenge_r.id2 = challenge->id2;

    user_info_t *usr1, *usr2;
    {
        user_info_t tmp1 = {.id = challenge->id1},
                    tmp2 = {.id = challenge->id2};
        usr1 = deref_or_null(tfind(&tmp1, &user_by_id, cmp_by_id));
        usr2 = deref_or_null(tfind(&tmp2, &user_by_id, cmp_by_id));
    }

    if (usr1 == NULL || usr2 == NULL) {
        msg.body.challenge_r.error = NXID;
        msg_send(fd, &msg);
        log_debug("Non-existent user ID %d or %d", challenge->id1,
                  challenge->id2);
        return;
    }

    // Verify key / arg
    {
        uint32_t key;
        switch ((challenge_action_t)challenge->action) {
        case C_START:
        case C_CANCEL:
            key = usr1->key;
            break;
        case C_ACCEPT:
        case C_REJECT:
            key = usr2->key;
            break;
        default:
            msg.body.challenge_r.error = INVARG;
            msg_send(fd, &msg);
            return;
            break;
        }
        if (key != challenge->key) {
            msg.body.challenge_r.error = ICKEY;
            msg_send(fd, &msg);
            log_debug("Wrong key");
            return;
        }
    }

    if (challenge->action == C_START) {
        if (usr1->state != UONLINE || usr2->state != UONLINE) {
            msg.body.challenge_r.error = ENGAGED;
            msg_send(fd, &msg);
            return;
        }
        if (usr1->id == usr2->id) {
            msg.body.challenge_r.error = CHLSELF;
            msg_send(fd, &msg);
            return;
        }
        // Create challenge
        challenge_t *ch = add_challenge();
        ch->state = ASKING;
        ch->user1 = challenge->id1, ch->user2 = challenge->id2;
        ch->hp1 = ch->hp2 = ch->maxhp1 = ch->maxhp2 = MAXHP;
        usr1->chid = ch->id;
        // Relay challenge request to the other user
        message_t *ch_msg = make_challenge();
        ch_msg->body.challenge = *challenge;
        ch_msg->body.challenge.chid = ch->id;
        msg_send(usr2->fd, ch_msg);
        free(ch_msg);
        return;
    }

    if (challenge->action == C_CANCEL) {
        if (usr1->state == UONLINE) {
            challenge_t *ch;
            {
                challenge_t tmp = {.id = usr1->chid};
                ch = deref_or_null(tfind(&tmp, &ch_by_id, cmp_by_chid));
            }
            if (ch && ch->state == ASKING && ch->user1 == usr1->id) {
                usr1->chid = 0;
                tdelete(ch, &ch_by_id, cmp_by_chid);
                user_info_t *user2;
                {
                    user_info_t tmp = {.id = ch->user2};
                    user2 = deref_or_null(tfind(&tmp, &user_by_id, cmp_by_id));
                }
                if (user2) {
                    msg.body.challenge_r.error = CANCELLED;
                    msg_send(user2->fd, &msg);
                }
            }
        }
        return;
    }

    challenge_t *ch;
    {
        challenge_t tmp = {.id = challenge->chid};
        ch = deref_or_null(tfind(&tmp, &ch_by_id, cmp_by_chid));
    }
    if (ch == NULL) {
        msg.body.challenge_r.error = NXCHID;
        msg_send(fd, &msg);
        return;
    } else if (ch->state == STARTED) {
        // Ignore these requests; no reply needed
        return;
    }

    switch ((challenge_action_t)challenge->action) {
    case C_START: // handled earlier
        assert(0);
        break;
    case C_ACCEPT: {
        if (usr1->state == UONLINE && usr2->state == UONLINE) {
            // Change user states & broadcast changes
            ch->state = STARTED;
            usr1->state = usr2->state = UBATTLING;
            usr1->chid = usr2->chid = ch->id;
            broadcast_user_changes(usr1, usr2);
            // Send reply to both users
            msg.body.challenge_r.error = ME_OK;
            msg.body.challenge_r.is_id1 = true;
            msg_send(usr1->fd, &msg);
            msg.body.challenge_r.is_id1 = false;
            msg_send(usr2->fd, &msg);
            ch->turn_no = 0;
            judge_turn(ch, -1);
        } else {
            // Reply with error
            msg.body.challenge_r.error = ENGAGED;
            msg_send(fd, &msg);
        }
    } break;
    case C_REJECT: {
        // Reply only when success and only to usr1
        if (ch->state == ASKING && ch->user2 == usr2->id) {
            msg.body.challenge_r.error = REJECTED;
            msg_send(usr1->fd, &msg);
            tdelete(ch, &ch_by_id, cmp_by_chid);
            free(ch);
        }
    } break;
    case C_CANCEL: {
        // handled earlier
        assert(0);
    } break;
    default: // handled earlier
        assert(0);
        break;
    }
}

static void handle_turn(int fd, msg_turn_t *turn) {
    challenge_t *ch;
    {
        challenge_t tmp = {.id = turn->chid};
        ch = deref_or_null(tfind(&tmp, &ch_by_id, cmp_by_chid));
        if (ch == NULL)
            return;
    }
    user_info_t *user;
    {
        user_info_t tmp = {.id = turn->user};
        user = deref_or_null(tfind(&tmp, &user_by_id, cmp_by_id));
        if (user == NULL)
            return;
    }
    if (user->key != turn->key) {
        return;
    }
    if (turn->user == ch->user1 && ch->acted1 == false) {
        ch->act1 = turn->action;
        ch->acted1 = true;
    } else if (turn->user == ch->user2 && ch->acted2 == false) {
        ch->act2 = turn->action;
        ch->acted2 = true;
    }

    if (ch->acted1 && ch->acted2)
        judge_turn(ch, -1);
}

static void handle_sendmsg(message_t *msg) {
    msg_sendmsg_t *sm = &msg->body.sendmsg;
    user_info_t *user;
    {
        user_info_t tmp = {.id = sm->id};
        user = deref_or_null(tfind(&tmp, &user_by_id, cmp_by_id));
    }
    if (user && user->key == sm->key) {
        sm->key = 0;
        send_uinfo_wkst_t st = {
            .type = MSG_TO_ALL, .except_fd = -1, .msg = msg};
        twalk_r(user_by_id, send_user_info, &st);
    }
}

static void *pkt_handler(void *__reserved) {
    while (1) {
        void *p;
        queue_entry_t *entry;
        if (queue_take(incoming_queue, &p, true) == QOK) {
            entry = p;
            switch (entry->kind) {
            case EMSG:
                log_debug("Got message from %d", entry->fd);
                switch (entry->msg->head.kind) {
                case JOIN:
                    handle_join(entry->fd, &entry->msg->body.join);
                    break;
                case QUIT:
                    handle_quit(&entry->msg->body.quit);
                    break;
                case CHALLENGE:
                    handle_challenge(entry->fd, &entry->msg->body.challenge);
                    break;
                case TURN:
                    handle_turn(entry->fd, &entry->msg->body.turn);
                    break;
                case SENDMSG:
                    handle_sendmsg(entry->msg);
                    break;
                }
                free(entry->msg);
                free(entry);
                break;
            case EDISCONN:
                handle_disconnect(entry->fd);
                // TODO: handle close
                // close(entry->fd);
                break;
            }
        } else {
            assert(0);
        }
    }
    return 0;
}

static void pkt_handler_init(pthread_t *thread) {
    incoming_queue = queue_create(MAX_QUEUE_SIZE);
    assert(incoming_queue);
    int err = pthread_create(thread, NULL, pkt_handler, NULL);
    if (err != 0)
        ppanic("%s: pthread_create()", __func__);
}

void signal_handlers_init() {
    // ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    // TODO: handle C-c
}

int main(int argc, char **argv) {
    set_loglevel(LOGLV_MAX);

    char listen_addr[ADDR_MAX_LEN] = "0.0.0.0";
    uint32_t port = DEFAULT_PORT;
    parse_args(argc, argv, listen_addr, ADDR_MAX_LEN, &port,
               argc == 0 ? APPNAME : argv[0], "LISTEN_ADDR");
    signal_handlers_init();
    random_init();
    model_init();
    pthread_t pkg_handler_thread;
    pkt_handler_init(&pkg_handler_thread);

    int listen_fd;
    listen_fd = do_listen(listen_addr, port);
    log_info("Listening at %s:%u...", listen_addr, port);
    while (true) {
        int fd;
        struct sockaddr_in sin;
        socklen_t addrlen = sizeof(sin);

        if ((fd = accept(listen_fd, (struct sockaddr *)&sin, &addrlen)) != -1) {
            serve_arg_t *arg = xmalloc(sizeof(*arg));
            arg->fd = fd;
            arg->sin = sin;
            pthread_t p;
            int err = pthread_create(&p, NULL, serve, arg);
            if (err != 0) {
                ppanic("Accepting connection: pthread_create()");
            }
        }
    }
}
