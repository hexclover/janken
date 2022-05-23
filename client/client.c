#include "lib/common.h"
#include <form.h>
#include <locale.h>
#include <menu.h>
#include <ncurses.h>
#include <panel.h>
#include <stdarg.h>
#include <stdbool.h>

#define QSIZE 4096
#define TICKS_PER_SEC 100
#define NANOSEC_PER_SEC 1000000000

/* INTERFACE SPECS */
const int ROOT_Y = 0, ROOT_X = 0;
const int POPUP_WIDTH = 60, POPUP_HEIGHT = 8;
// subwindows of root (popup is not)
const int MAIN_WIDTH = 80, MAIN_HEIGHT = 24, MAIN_REL_Y = 1, MAIN_REL_X = 1;
const int POPUP_Y = (MAIN_HEIGHT - POPUP_HEIGHT) / 2,
          POPUP_X = ((MAIN_WIDTH - POPUP_WIDTH) / 2);
const int USER_LIST_WIDTH = 20, USER_LIST_HEIGHT = 16, USER_LIST_REL_Y = 0,
          USER_LIST_REL_X = 0;
const int ARENA_WIDTH = MAIN_WIDTH - USER_LIST_WIDTH, ARENA_HEIGHT = 16,
          ARENA_REL_Y = USER_LIST_REL_Y,
          ARENA_REL_X = USER_LIST_REL_X + USER_LIST_WIDTH;
const int MAIN_MENU_WIDTH = 20, MAIN_MENU_HEIGHT = MAIN_HEIGHT - ARENA_HEIGHT,
          MAIN_MENU_REL_Y = ARENA_REL_Y + ARENA_HEIGHT,
          MAIN_MENU_REL_X = MAIN_WIDTH - MAIN_MENU_WIDTH;
const int CHAT_INPUT_HEIGHT = 1;
const int CHAT_WIDTH = MAIN_WIDTH - MAIN_MENU_WIDTH - 1,
          CHAT_HEIGHT = MAIN_HEIGHT - max_(USER_LIST_HEIGHT, ARENA_HEIGHT) -
                        CHAT_INPUT_HEIGHT,
          CHAT_REL_Y = max_(ARENA_REL_Y + ARENA_HEIGHT,
                            USER_LIST_REL_Y + USER_LIST_HEIGHT),
          CHAT_REL_X = 1;
const int CHAT_INPUT_WIDTH = MAIN_WIDTH - MAIN_MENU_WIDTH,
          CHAT_INPUT_REL_Y = CHAT_REL_Y + CHAT_HEIGHT, CHAT_INPUT_REL_X = 1;
// subwindows of arena
const int USER_STATE_WIDTH = 29, USER_STATE_HEIGHT = 6;
const int PLAYER_STATE_REL_Y = 1, PLAYER_STATE_REL_X = 1;
const int OPPONENT_STATE_REL_Y =
              /*ARENA_HEIGHT - USER_STATE_HEIGHT - 1*/ PLAYER_STATE_REL_Y,
          OPPONENT_STATE_REL_X = ARENA_WIDTH - USER_STATE_WIDTH - 1;
const int BATTLE_MSG_HEIGHT = 5, BATTLE_MSG_WIDTH = ARENA_WIDTH - 2,
          BATTLE_MSG_REL_Y = PLAYER_STATE_REL_Y + USER_STATE_HEIGHT,
          BATTLE_MSG_REL_X = 1;
const int BATTLE_ACTION_HEIGHT =
              ARENA_HEIGHT - USER_STATE_HEIGHT - BATTLE_MSG_HEIGHT - 2,
          BATTLE_ACTION_WIDTH = ARENA_WIDTH - 2,
          BATTLE_ACTION_REL_Y = BATTLE_MSG_REL_Y + BATTLE_MSG_HEIGHT,
          BATTLE_ACTION_REL_X = 1;
/* */

const char *title_string[] = {"J A N K E N"};
const char *please_login = "Enter nickname to login:";
const char *APPNAME = "game_client";
char initial_addr[ADDR_MAX_LEN] = "127.0.0.1";
uint32_t initial_port = DEFAULT_PORT;
typedef enum net_thread_kind_t { IN, OUT } net_thread_kind_t;

typedef struct io_arg_t {
    int fd;
    net_thread_kind_t kind;
    /* For IN: Receive packets and store into queue
       For OUT: Take packets from queue and send to server_fd
     */
    queue_t *queue;
} io_arg_t;

typedef struct ui_msg_t {
    enum { UM_DISCONN, UM_RECV } kind;
    union {
        struct {
            net_thread_kind_t kind;
            int err;
        } disconn;
        message_t message;
    };
} ui_msg_t;

typedef enum ui_state_t { UI_INIT, UI_LOGIN, UI_MAIN, UI_EXIT } ui_state_t;

typedef struct game_state_t {
    char nickname[NICKNAME_LEN], opponent_name[NICKNAME_LEN];
    uint16_t id, opponent_id, chid;
    bool is_user1;
    bool acted;
    uint32_t key;
    int32_t score;
    uint16_t turn_no;
    int32_t hp, max_hp, opponent_hp, opponent_max_hp;
    user_state_t state;
    void *user_by_id;
} game_state_t;

typedef enum uc_kind_t {
    UC_CHL_USER,
    UC_BATTLE_ACT,
    UC_SORT,
    UC_QUIT,
    UC_SENDMSG,
    UC_MAX
} uc_kind_t;

typedef enum sort_by_t { BY_NAME, BY_SCORE } sort_by_t;

typedef struct user_cmd_t {
    uc_kind_t kind;
    union {
        struct {
            uint16_t chl_user_id;
            // HACK: need this information for sorting
            int32_t chl_score;
            char chl_user_name[32];
        };
        uint16_t acc_or_rej_chid;
        char new_name[32];
        battle_act_t b_act;
        sort_by_t sort_by;
        struct {
            const char **names;
            struct user_cmd_t **actions;
            size_t size;
        } submenu;
    };
} user_cmd_t;

static void game_state_init(game_state_t *gs) {
    gs->nickname[0] = gs->opponent_name[0] = '\0';
    gs->score = 0;
    gs->id = gs->opponent_id = gs->chid = 0;
    gs->hp = gs->opponent_hp = gs->max_hp = gs->opponent_max_hp = 0;
    gs->user_by_id = NULL;
}

static user_info_t *user_create(const char *nickname, uint16_t id,
                                user_state_t state, int32_t score) {
    user_info_t *user = xmalloc(sizeof(*user));
    user->id = id;
    user->state = state;
    user->score = score;
    snprintf(user->nickname, NICKNAME_LEN, "%s", nickname);
    return user;
}

static void user_destroy(user_info_t *user) { free(user); }

static void create_thread(pthread_t *thread, void *(*entry)(void *),
                          const void *arg, size_t arg_size) {
    void *arg_copy;
    if (arg) {
        arg_copy = xmalloc(arg_size);
        memcpy(arg_copy, arg, arg_size);
    } else {
        arg_copy = NULL;
    }
    if (pthread_create(thread, NULL, entry, arg_copy) != 0) {
        ppanic("pthread_create()");
    }
}

static int do_connect(const char *addr, uint32_t port, char *err,
                      size_t errmax) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        snprintf(err, errmax, "socket(): %s", strerror(errno));
        return -1;
    }

    struct in_addr saddr;
    if (inet_aton(addr, &saddr) == 0) {
        snprintf(err, errmax, "Invalid server IP address: `%s'", addr);
        goto fail;
    }
    struct sockaddr_in sin;
    sin.sin_addr = saddr;
    sin.sin_port = htons(port);
    sin.sin_family = AF_INET;
    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        snprintf(err, errmax, "connect() to %s:%u: %s", addr, port,
                 strerror(errno));
        goto fail;
    }
    return fd;
fail:
    close(fd);
    return -1;
}

static void *io_worker(void *parg) {
    io_arg_t arg = *(io_arg_t *)parg;

    switch (arg.kind) {
    case IN:
        while (1) {
            message_t msg;
            if (msg_recv(arg.fd, &msg, true) == 0) {
                ui_msg_t *ui_msg = xmalloc(sizeof(*ui_msg));
                ui_msg->kind = UM_RECV;
                memcpy(&ui_msg->message, &msg, sizeof(msg));
                queue_add(arg.queue, ui_msg, true);
            } else {
                break;
            }
        }
        break;
    case OUT:
        while (1) {
            void *p;
            message_t *msg;
            if (queue_take(arg.queue, &p, true) == QOK) {
                msg = p;
                log_info("Sending: %d", arg.fd);
                int err = msg_send(arg.fd, msg);
                free(msg);
                if (err != 0) {
                    log_error("Send error: %d", err);
                    break;
                }
            }
        }
        break;
    default:
        assert(0);
    }

    ui_msg_t *um = xmalloc(sizeof(*um));
    um->kind = UM_DISCONN;
    um->disconn.kind = arg.kind;
    um->disconn.err = errno;
    queue_add(arg.queue, um, true);
    log_info("Disconnected: %d (%d)", arg.fd, arg.kind);
    free(parg);
    return 0;
}

static int centered_lpad(WINDOW *win, size_t object_width) {
    int maxy, maxx;
    getmaxyx(win, maxy, maxx);
    (void)maxy; // make compiler happy
    return (maxx - object_width) / 2;
}

inline static bool is_bksp(int c) {
    return c == KEY_BACKSPACE || c == '\b' || c == 127;
}

inline static int gety(WINDOW *win) {
    int y, x;
    (void)x;
    getyx(win, y, x);
    return y;
}

inline static void hide_cursor() { curs_set(0); }

inline static void show_cursor() { curs_set(1); }

__attribute__((format(printf, 4, 5))) static void
print_centered(WINDOW *win, int y, bool clear, const char *fmt, ...) {
    if (clear) {
        wmove(win, y, 0);
        wclrtoeol(win);
    }
    char buf[256];
    va_list args;
    va_start(args, fmt);
    size_t len = vsnprintf(buf, sizeof(buf), fmt, args);
    wmove(win, y, centered_lpad(win, len));
    wprintw(win, "%s", buf);
    va_end(args);
}

static void draw_login_scr(WINDOW *win) {
    size_t title_width = 0;
    const size_t title_height = ARRAY_SIZE(title_string);
    for (size_t i = 0; i < title_height; ++i) {
        size_t len = strlen(title_string[i]);
        if (len > title_width)
            title_width = len;
    }
    size_t lpad = centered_lpad(win, title_width);
    size_t tpad = 0.25 * MAIN_HEIGHT;
    for (size_t i = 0; i < title_height; ++i) {
        wmove(win, tpad, lpad);
        wprintw(win, "%s", title_string[i]);
    }
    print_centered(win, gety(win) + 2, true, "%s", please_login);
}

static ui_state_t ui_init(WINDOW **proot, queue_t **pum_queue,
                          queue_t **psend_queue) {
    if (*proot) {
        delwin(*proot);
    }
    *proot = newwin(MAIN_HEIGHT + 2, MAIN_WIDTH + 2, ROOT_Y, ROOT_X);
    if (*proot == NULL)
        ppanic("newwin()");
    box(*proot, 0, 0);
    refresh();
    wrefresh(*proot);

    // TODO: destroy the previous queues; free messages in them
    *pum_queue = queue_create(QSIZE);
    *psend_queue = queue_create(QSIZE);

    if (noecho() == ERR)
        ppanic("noecho()");
    hide_cursor();
    return UI_LOGIN;
}

static ui_state_t ui_login(WINDOW *root, pthread_t *pprecv, pthread_t *ppsend,
                           queue_t *um_queue, queue_t *send_queue,
                           message_t **join_msg) {
    ui_state_t next_state = UI_MAIN;
    WINDOW *win = derwin(root, MAIN_HEIGHT, MAIN_WIDTH, 1, 1);
    assert(win);
    if (keypad(win, true) == ERR)
        ppanic("keypad()");

    char info[256] = {'\0'}, nick_buf[NICKNAME_LEN] = {'\0'},
         addr[ADDR_MAX_LEN];
    snprintf(addr, ADDR_MAX_LEN, "%s", initial_addr);
    uint32_t port = initial_port;
    size_t nick_len = 0;

    nodelay(win, FALSE);
    show_cursor();
    draw_login_scr(win); // Now cursor at "Please login" line
    int nick_y = gety(win) + 2;
    int info_y = nick_y + 2;

    enum {
        WAIT_INPUT,
        TO_CONNECT,
        SEND_JOIN,
        WAIT_MSG,
        SUCCESS
    } state = WAIT_INPUT;
    bool done = false, connected = false;
    int fd;
    while (!done) {
        print_centered(win, info_y, true, "%s", info);
        print_centered(win, nick_y, true, "%s", nick_buf);
        touchwin(root);
        wrefresh(win);
        switch (state) {
        case WAIT_INPUT: {
            info[0] = '\0';
            log_info("Waiting");
            int c = wgetch(win);
            log_info("Got character %c", c);
            if (c == '\n') {
                if (is_nickstr(nick_buf)) {
                    strcpy(info, "Connecting to server...");
                    state = TO_CONNECT;
                } else {
                    strcpy(info, "Illegal nickname!");
                }
            } else {
                if (is_bksp(c)) {
                    info[0] = '\0';
                    if (nick_len >= 1) {
                        nick_buf[--nick_len] = '\0';
                    }
                } else {
                    if (c == KEY_DOWN || c == KEY_UP || c == KEY_LEFT ||
                        c == KEY_RIGHT) {
                        ;
                    } else if (!is_nickchar(c)) {
                        sprintf(info,
                                "Character not allowed in nicknames: `%c'", c);
                    } else if (nick_len + 1 >= NICKNAME_LEN) {
                        strcpy(info, "Nickname too long!");
                    } else {
                        info[0] = '\0';
                        nick_buf[nick_len++] = c;
                        nick_buf[nick_len] = '\0';
                    }
                }
            }
        } break;
        case TO_CONNECT: {
            if (!connected) {
                fd = do_connect(addr, port, info, sizeof(info));
                if (fd == -1) {
                    state = WAIT_INPUT;
                    break;
                } else {
                    io_arg_t arg = {.fd = fd};
                    arg.kind = IN;
                    arg.queue = um_queue;
                    create_thread(pprecv, io_worker, &arg, sizeof(arg));
                    arg.kind = OUT;
                    arg.queue = send_queue;
                    create_thread(ppsend, io_worker, &arg, sizeof(arg));
                    connected = true;
                }
            }
            state = SEND_JOIN;
        } break;
        case SEND_JOIN: {
            assert(connected);
            message_t *msg = make_join(nick_buf);
            queue_add(send_queue, msg, true);
            strcpy(info, "Waiting for server reply...");
            state = WAIT_MSG;
        } break;
        case WAIT_MSG: {
            void *p;
            ui_msg_t *ui_msg;
            if (queue_take(um_queue, &p, true) == QOK) {
                ui_msg = p;
                switch (ui_msg->kind) {
                case UM_DISCONN:
                    snprintf(info, sizeof(info), "Disconnected: %s",
                             strerror(ui_msg->disconn.err));
                    if (fd != -1) {
                        close(fd);
                        fd = -1;
                    }
                    connected = false;
                    break;
                case UM_RECV: {
                    message_t *msg = &ui_msg->message;
                    if (msg->head.kind == JOIN_R) {
                        msg_join_r_t *r = &msg->body.join_r;
                        if (r->error == ME_OK) {
                            snprintf(info, sizeof(info), "OK!");
                            *join_msg = msg_dup(msg);
                            state = SUCCESS;
                        } else {
                            snprintf(info, sizeof(info), "Cannot join: %s",
                                     msg_strerror(msg->body.join_r.error));
                            state = WAIT_INPUT;
                        }
                    } else {
                        snprintf(info, sizeof(info), "What's this? %d",
                                 msg->head.kind);
                    }
                } break;
                }
                free(ui_msg);
            }
        } break;
        case SUCCESS: {
            done = true;
            break;
        }
        default:
            assert(0);
            break;
        }
    }

    refresh();
    touchwin(root);
    wclear(win);
    wrefresh(win);
    delwin(win);
    return next_state;
}

typedef struct populate_arg_t {
    size_t idx, sz;
    ITEM **items;
    uint16_t *selection_id;
    ITEM *selection;
    uint16_t self_id;
} populate_arg_t;

char user_state_char(user_state_t st) {
    switch (st) {
    case UOFFLINE:
        return '-';
    case UONLINE:
        return '+';
    case UBATTLING:
        return 'X';
    }
    return '?';
}

void populate_user_list(const void *pnode, VISIT vis, void *parg) {
    populate_arg_t *arg = parg;
    if (vis == postorder || vis == leaf) {
        const user_info_t *user = *(const user_info_t **)pnode;
        if (arg->idx < arg->sz) {
            char nb[128], db[128];
            snprintf(nb, sizeof(nb), "%s%s", user->nickname,
                     user->id == arg->self_id ? "*" : "");
            snprintf(db, sizeof(db), "%c|%d", user_state_char(user->state),
                     user->score);
            ITEM *item = new_item(strdup(nb), strdup(db));
            user_cmd_t *cmd = xcalloc(1, sizeof(*cmd));
            cmd->kind = UC_CHL_USER;
            cmd->chl_user_id = user->id;
            snprintf(cmd->chl_user_name, NICKNAME_LEN, "%s", user->nickname);
            cmd->chl_score = user->score;
            set_item_userptr(item, cmd);
            arg->items[arg->idx] = item;
            if (arg->selection_id && user->id == *arg->selection_id) {
                arg->selection = item;
            }
            ++arg->idx;
        }
    }
}

void draw_user_state_subwin(WINDOW *subwin, void *const *rootp, uint16_t id,
                            int32_t hp, int32_t maxhp) {
    user_info_t tmp = {.id = id};
    const user_info_t *user = deref_or_null(tfind(&tmp, rootp, cmp_by_id));
    const char *nickname = user ? user->nickname : NULL;
    print_centered(subwin, 1, true, "%s",
                   nickname && nickname[0] ? nickname : "-");
    print_centered(subwin, 3, true, "HP: %4d /%4d", hp, maxhp);
    touchwin(wgetparent(subwin));
    wrefresh(subwin);
}

typedef struct focusable_t {
    enum { F_MENU, F_FORM } kind;
    bool hidden;
    union {
        struct {
            MENU *menu;
            bool keep_selection;
        };
        FORM *form;
    };
} focusable_t;

void focus(focusable_t *foc) {
    if (foc == NULL)
        return;
    if (foc->hidden)
        return;
    switch (foc->kind) {
    case F_MENU:
        set_menu_back(foc->menu, A_NORMAL);
        set_menu_fore(foc->menu, A_REVERSE);
        break;
    case F_FORM:
        show_cursor();
        form_driver(foc->form, REQ_FIRST_FIELD);
        break;
    }
}

void unfocus(focusable_t *foc) {
    if (foc == NULL)
        return;
    if (foc->hidden)
        return;
    switch (foc->kind) {
    case F_MENU:
        if (foc->keep_selection) {
            set_menu_back(foc->menu, A_NORMAL);
            set_menu_fore(foc->menu, A_BOLD);
        } else {
            set_menu_back(foc->menu, A_NORMAL);
            set_menu_fore(foc->menu, A_NORMAL);
        }
        break;
    case F_FORM:
        hide_cursor();
        break;
    }
}

user_cmd_t *focus_handle_key(focusable_t *foc, int key) {
    if (foc == NULL)
        return NULL;
    switch (foc->kind) {
    case F_MENU:
        switch (key) {
        case KEY_DOWN:
            menu_driver(foc->menu, REQ_DOWN_ITEM);
            break;
        case KEY_UP:
            menu_driver(foc->menu, REQ_UP_ITEM);
            break;
        case KEY_LEFT:
            menu_driver(foc->menu, REQ_LEFT_ITEM);
            break;
        case KEY_RIGHT:
            menu_driver(foc->menu, REQ_RIGHT_ITEM);
            break;
        case '\n':
            return item_userptr(current_item(foc->menu));
            break;
        }
        break;
    case F_FORM:
        switch (key) {
        case KEY_DOWN:
            form_driver(foc->form, REQ_BEG_LINE);
            break;
        case KEY_UP:
            form_driver(foc->form, REQ_END_LINE);
            break;
        case KEY_LEFT:
            form_driver(foc->form, REQ_LEFT_CHAR);
            break;
        case KEY_RIGHT:
            form_driver(foc->form, REQ_RIGHT_CHAR);
            break;
        case KEY_DC:
            form_driver(foc->form, REQ_DEL_CHAR);
            break;
        case '\n':
            return form_userptr(foc->form);
            break;
        default:
            if (is_bksp(key))
                form_driver(foc->form, REQ_DEL_PREV);
            else if (ispunct(key) || isalnum(key) || isspace(key))
                form_driver(foc->form, key);
            break;
        }
        break;
    }
    return NULL;
}

void derwin_with_box(WINDOW *parent, WINDOW **boxwin, WINDOW **subwin,
                     int box_lines, int box_cols, int box_rel_y, int box_rel_x,
                     const char *title) {
    WINDOW *_box, *_sub;
    _box = derwin(parent, box_lines, box_cols, box_rel_y, box_rel_x);
    assert(_box);
    box(_box, 0, 0);
    if (title) {
        print_centered(_box, 0, false, "%s", title);
        touchwin(parent);
        wrefresh(_box);
    }
    if (boxwin)
        *boxwin = _box;
    _sub = derwin(_box, box_lines - 2, box_cols - 2, 1, 1);
    assert(_sub);
    if (subwin)
        *subwin = _sub;
}

int cmp_item_for_sort(const void *p, const void *q, void *psb) {
    const ITEM *i = *(ITEM **)p, *j = *(ITEM **)q;
    const user_cmd_t *ci = item_userptr(i), *cj = item_userptr(j);
    const int by_name = strncasecmp(ci->chl_user_name, cj->chl_user_name,
                                    NICKNAME_LEN),
              by_score = cj->chl_score - ci->chl_score; // descending
    const sort_by_t sb = *(const sort_by_t *)psb;
    switch (sb) {
    case BY_NAME:
        return by_name == 0 ? by_score : by_name;
    case BY_SCORE:
        return by_score == 0 ? by_name : by_score;
    default:
        return 0;
    }
}

static ui_state_t ui_main(WINDOW *root, pthread_t *pprecv, pthread_t *ppsend,
                          queue_t *um_queue, queue_t *send_queue,
                          message_t *join_msg) {
    game_state_t gs;
    game_state_init(&gs);
    assert(join_msg->head.kind == JOIN_R &&
           join_msg->body.join_r.error == ME_OK);
    gs.id = join_msg->body.join_r.id;
    snprintf(gs.nickname, NICKNAME_LEN, "%s", join_msg->body.join_r.nickname);
    gs.key = join_msg->body.join_r.key;

    void *user_by_id = NULL;
    size_t user_cnt = 0;

    struct timespec rqtp, rmtp;
    memset(&rqtp, 0, sizeof(rqtp));
    rqtp.tv_nsec = NANOSEC_PER_SEC / TICKS_PER_SEC;

    hide_cursor();

    WINDOW *main_win =
        derwin(root, MAIN_HEIGHT, MAIN_WIDTH, MAIN_REL_Y, MAIN_REL_X);
    assert(main_win);
    if (keypad(main_win, TRUE) == ERR)
        ppanic("keypad()");
    if (nodelay(main_win, TRUE) == ERR)
        ppanic("nodelay()");

    WINDOW *popup_win = newwin(POPUP_HEIGHT, POPUP_WIDTH, POPUP_Y, POPUP_X);
    WINDOW *popup_sub =
        derwin(popup_win, POPUP_HEIGHT - 2, POPUP_WIDTH - 2, 1, 1);
    assert(popup_win && popup_sub);
    box(popup_win, 0, 0);
    print_centered(popup_win, 0, false, "Message");

    PANEL *popup_panel, *main_panel;
    main_panel = new_panel(root);
    assert(main_panel);
    popup_panel = new_panel(popup_win);
    assert(popup_panel);
    hide_panel(popup_panel);
    update_panels();
    doupdate();

    WINDOW *arena =
        derwin(main_win, ARENA_HEIGHT, ARENA_WIDTH, ARENA_REL_Y, ARENA_REL_X);
    assert(arena);
    box(arena, 0, 0);
    print_centered(arena, 0, false, "Battle");
    touchwin(main_win);
    wrefresh(arena);

    static const char *main_act_names[] = {"Sort By Name", "Sort By Score",
                                           "Quit", 0};
    const static user_cmd_t main_menu_actions[] = {
        {.kind = UC_SORT, .sort_by = BY_NAME},
        {.kind = UC_SORT, .sort_by = BY_SCORE},
        {.kind = UC_QUIT},
        {.kind = UC_MAX}};

    static_assert(ARRAY_SIZE(main_act_names) == ARRAY_SIZE(main_menu_actions),
                  "");

    WINDOW *user_list_win, *user_list_sub;
    derwin_with_box(main_win, &user_list_win, &user_list_sub, USER_LIST_HEIGHT,
                    USER_LIST_WIDTH, USER_LIST_REL_Y, USER_LIST_REL_X, "Users");

    MENU *user_list_menu = new_menu(xcalloc(1, sizeof(ITEM *)));
    assert(user_list_menu);
    set_menu_mark(user_list_menu, ">");
    menu_opts_off(user_list_menu, O_NONCYCLIC);
    set_menu_win(user_list_menu, user_list_win);
    set_menu_sub(user_list_menu, user_list_sub);
    set_menu_format(user_list_menu, USER_LIST_HEIGHT - 2, USER_LIST_WIDTH - 2);

    ITEM **main_act_items = xcalloc(ARRAY_SIZE(main_act_names), sizeof(ITEM *));
    for (size_t i = 0; i < ARRAY_SIZE(main_act_names); ++i) {
        main_act_items[i] = new_item(main_act_names[i], NULL);
        set_item_userptr(main_act_items[i], (void *)&main_menu_actions[i]);
    }
    MENU *main_menu = new_menu(main_act_items);
    menu_opts_off(main_menu, O_NONCYCLIC);
    assert(main_menu);
    WINDOW *main_menu_win, *main_menu_sub;
    derwin_with_box(main_win, &main_menu_win, &main_menu_sub, MAIN_MENU_HEIGHT,
                    MAIN_MENU_WIDTH, MAIN_MENU_REL_Y, MAIN_MENU_REL_X, NULL);
    set_menu_mark(main_menu, "*");
    set_menu_win(main_menu, main_menu_win);
    set_menu_sub(main_menu, main_menu_sub);
    post_menu(main_menu);
    touchwin(main_win);
    wrefresh(main_menu_win);

    WINDOW *player_state_sub, *player_state_win, *opponent_state_sub,
        *opponent_state_win, *battle_msg_win, *battle_msg_sub;
    derwin_with_box(arena, &player_state_win, &player_state_sub,
                    USER_STATE_HEIGHT, USER_STATE_WIDTH, PLAYER_STATE_REL_Y,
                    PLAYER_STATE_REL_X, "Player");
    derwin_with_box(arena, &opponent_state_win, &opponent_state_sub,
                    USER_STATE_HEIGHT, USER_STATE_WIDTH, OPPONENT_STATE_REL_Y,
                    OPPONENT_STATE_REL_X, "Opponent");
    derwin_with_box(arena, &battle_msg_win, &battle_msg_sub, BATTLE_MSG_HEIGHT,
                    BATTLE_MSG_WIDTH, BATTLE_MSG_REL_Y, BATTLE_MSG_REL_X, NULL);
    scrollok(battle_msg_sub, true);
    idlok(battle_msg_sub, true);
    WINDOW *battle_act_win, *battle_act_sub;
    derwin_with_box(arena, &battle_act_win, &battle_act_sub,
                    BATTLE_ACTION_HEIGHT, BATTLE_ACTION_WIDTH,
                    BATTLE_ACTION_REL_Y, BATTLE_ACTION_REL_X, "Actions");
    static const char *battle_act_names[] = {"Rock", "Paper", "Scissors", 0};
    const static user_cmd_t battle_act_actions[] = {
        {.kind = UC_BATTLE_ACT, .b_act = B_ROCK},
        {.kind = UC_BATTLE_ACT, .b_act = B_PAPER},
        {.kind = UC_BATTLE_ACT, .b_act = B_SCISSORS},
        {.kind = UC_MAX}};
    static_assert(
        ARRAY_SIZE(battle_act_names) == ARRAY_SIZE(battle_act_actions), "");
    ITEM **battle_act_items =
        xcalloc(ARRAY_SIZE(battle_act_names), sizeof(ITEM *));
    for (size_t i = 0; i < ARRAY_SIZE(battle_act_names); ++i) {
        battle_act_items[i] = new_item(battle_act_names[i], NULL);
        set_item_userptr(battle_act_items[i], (void *)&battle_act_actions[i]);
    }
    MENU *battle_act_menu = new_menu(battle_act_items);
    set_menu_win(battle_act_menu, battle_act_win);
    set_menu_sub(battle_act_menu, battle_act_sub);
    set_menu_mark(battle_act_menu, NULL);
    menu_opts_off(battle_act_menu, O_NONCYCLIC);
    menu_opts_off(battle_act_menu, O_ROWMAJOR);
    set_menu_format(battle_act_menu, 2, 5);
    post_menu(battle_act_menu);
    touchwin(battle_act_win);
    wrefresh(battle_act_sub);

    WINDOW *chat_sub;
    derwin_with_box(main_win, NULL, &chat_sub, CHAT_HEIGHT, CHAT_WIDTH,
                    CHAT_REL_Y, CHAT_REL_X, "Chat");
    scrollok(chat_sub, true);
    idlok(chat_sub, true);

    FIELD *fields[2] = {0, 0};
    fields[0] = new_field(1, CHAT_INPUT_WIDTH - 1, 0, 0, 0, 0);
    FORM *chat_input = new_form(fields);
    WINDOW *chat_input_sub =
        derwin(main_win, CHAT_INPUT_HEIGHT, CHAT_INPUT_WIDTH - 1,
               CHAT_INPUT_REL_Y, CHAT_INPUT_REL_X);
    /* derwin_with_box(main_win, &chat_input_win, &chat_input_sub, */
    /*                 CHAT_INPUT_HEIGHT, CHAT_INPUT_WIDTH, CHAT_INPUT_REL_Y, */
    /*                 CHAT_REL_X, NULL); */
    /* assert(chat_input_sub && chat_input_win); */
    assert(chat_input);
    set_field_back(fields[0], A_UNDERLINE);
    field_opts_off(fields[0], O_AUTOSKIP);
    /* set_form_win(chat_input, chat_input_win); */
    set_form_sub(chat_input, chat_input_sub);
    user_cmd_t *send_cmd = xcalloc(1, sizeof(*send_cmd));
    send_cmd->kind = UC_SENDMSG;
    set_form_userptr(chat_input, send_cmd);
    post_form(chat_input);
    touchwin(main_win);
    wrefresh(chat_input_sub);

    bool user_list_updated = false, user_state_updated = true;
    struct focusable_t foci[4] = {
        {.kind = F_MENU,
         .menu = user_list_menu,
         .keep_selection = true,
         .hidden = false},
        {.kind = F_FORM, .form = chat_input},
        {.kind = F_MENU,
         .menu = battle_act_menu,
         .keep_selection = false,
         .hidden = false},
        {.kind = F_MENU,
         .menu = main_menu,
         .keep_selection = false,
         .hidden = false},
    };
    const size_t focus_cnt = ARRAY_SIZE(foci);
    size_t focus_idx = 0;
    for (size_t i = 0; i < focus_cnt; ++i) {
        if (i == focus_idx)
            focus(foci + i);
        else
            unfocus(foci + i);
    }

    enum { W_NONE, W_CANCEL, W_ACCEPT } waiting_for = W_NONE;
    sort_by_t sort_by = BY_SCORE;

    while (1) {
        void *p;
        ui_msg_t *um;
        while (queue_take(um_queue, &p, false) == QOK) {
            um = p;
            switch (um->kind) {
            case UM_DISCONN:
                goto done;
                break;
            case UM_RECV: {
                message_t *msg = &um->message;
                switch (msg->head.kind) {
                case SENDMSG: {
                    user_info_t *user;
                    {
                        user_info_t tmp = {.id = msg->body.sendmsg.id};
                        user =
                            deref_or_null(tfind(&tmp, &user_by_id, cmp_by_id));
                    }
                    const char *sender = user ? user->nickname : "<unknown>";
                    wprintw(chat_sub, "%s%s said: %s\n", sender,
                            msg->body.sendmsg.id == gs.id ? " (You)" : "",
                            msg->body.sendmsg.text);
                    touchwin(main_win);
                    wrefresh(chat_sub);
                } break;
                case TURN_R: {
                    msg_turn_r_t *r = &msg->body.turn_r;
                    wmove(battle_msg_sub, 0, 0);
                    if (gs.is_user1) {
                        gs.hp = r->hp1;
                        gs.max_hp = r->maxhp1;
                        gs.opponent_hp = r->hp2;
                        gs.opponent_max_hp = r->maxhp2;
                    } else {
                        gs.hp = r->hp2;
                        gs.max_hp = r->maxhp2;
                        gs.opponent_hp = r->hp1;
                        gs.opponent_max_hp = r->maxhp1;
                    }
                    if (r->fin) {
                        wprintw(battle_msg_sub, "Battle ended -- your %s\n",
                                gs.id == r->winner ? "victory!" : "defeat...");
                        gs.acted = true;
                    } else {
                        if (r->turn_no > 1) {
                            // "Last action" is available
                            const char *s1, *s2;
                            battle_act_t a, b;
                            if (gs.is_user1) {
                                a = r->action1, b = r->action2;
                            } else {
                                a = r->action2, b = r->action1;
                            }
                            s1 = act_name(a);
                            s2 = act_name(b);
                            turn_result_t res = get_turn_result(a, b);
                            wprintw(battle_msg_sub, "Last turn: %s - %s (%s)\n",
                                    s1, s2,
                                    res == TR_EVEN  ? "even"
                                    : res == TR_WIN ? "you won"
                                                    : "you lost");
                        }
                        wprintw(battle_msg_sub,
                                "Turn %d -- Please choose your action.\n",
                                r->turn_no);
                        touchwin(battle_msg_win);
                        wrefresh(battle_msg_sub);
                        gs.turn_no = r->turn_no;
                        gs.acted = false;
                    }
                    user_state_updated = true;
                } break;
                case CHALLENGE: {
                    msg_challenge_t *pch = &msg->body.challenge;
                    switch (pch->action) {
                    case C_START:
                        if (waiting_for == W_NONE) {
                            waiting_for = W_ACCEPT;
                            wclear(popup_sub);
                            user_info_t *usr1;
                            {
                                user_info_t tmp = {.id = pch->id1};
                                usr1 = deref_or_null(
                                    tfind(&tmp, &user_by_id, cmp_by_id));
                            }
                            print_centered(popup_sub, 3, false,
                                           "You are being challenged by `%s'. "
                                           "Join battle? (Y/N)",
                                           usr1 ? usr1->nickname : "<unknown>");
                            gs.chid = pch->chid;
                            gs.opponent_id = pch->id1;
                            touchwin(popup_win);
                            wrefresh(popup_sub);
                            show_panel(popup_panel);
                            update_panels();
                            doupdate();
                        } else {
                            // reject immediately
                            msg_challenge_t ch = {.action = C_REJECT,
                                                  .chid = pch->chid,
                                                  .id1 = pch->id1,
                                                  .id2 = pch->id2,
                                                  .key = gs.key};
                            message_t *msg = make_challenge();
                            msg->body.challenge = ch;
                            queue_add(send_queue, msg, true);
                        }
                        break;
                    default:
                        log_info("FIXME: should not receive %d", pch->action);
                        break;
                    }
                } break;
                case CHALLENGE_R: {
                    msg_challenge_r_t *chr = &msg->body.challenge_r;
                    if (chr->error == ME_OK) {
                        // Must start a battle
                        waiting_for = W_NONE;
                        hide_panel(popup_panel);
                        update_panels();
                        int32_t op;
                        if (chr->is_id1) {
                            op = chr->id2;
                            gs.is_user1 = true;
                        } else {
                            op = chr->id1;
                            gs.is_user1 = false;
                        }
                        gs.acted = true;
                        gs.opponent_id = op;
                        gs.chid = chr->chid;
                        user_info_t *opponent;
                        {
                            user_info_t tmp = {.id = op};
                            opponent = deref_or_null(
                                tfind(&tmp, &user_by_id, cmp_by_id));
                        }
                        gs.state = UBATTLING;
                        snprintf(gs.opponent_name, NICKNAME_LEN, "%s",
                                 opponent ? opponent->nickname : "-");
                        wprintw(battle_msg_sub, "Starting battle with %s\n",
                                gs.opponent_name);
                        touchwin(battle_msg_win);
                        wrefresh(battle_msg_sub);
                    } else {
                        // Current challenge is cancelled
                        wprintw(battle_msg_sub, "Battle didn't start: %s\n",
                                msg_strerror(chr->error));
                        touchwin(battle_msg_win);
                        wrefresh(battle_msg_sub);
                        hide_panel(popup_panel);
                        update_panels();
                        doupdate();
                        waiting_for = W_NONE;
                    }
                    user_state_updated = true;
                } break;
                case UCHANGE: {
                    msg_uchange_t *changes = &msg->body.uchange;
                    for (size_t i = 0; i < changes->count; ++i) {
                        log_debug("Handling change %zu/%u, id = %d, nick = %s",
                                  i, changes->count, changes->users[i].id,
                                  changes->users[i].nickname);
                        if (changes->users[i].id == gs.id) {
                            // Changing info about myself
                            if (changes->users[i].state == UOFFLINE) {
                                goto done;
                            } else {
                                snprintf(gs.nickname, NICKNAME_LEN, "%s",
                                         changes->users[i].nickname);
                                gs.state = changes->users[i].state;
                            }
                            user_state_updated = true;
                        }
                        if (changes->users[i].id == gs.opponent_id) {
                            snprintf(gs.opponent_name, NICKNAME_LEN, "%s",
                                     changes->users[i].nickname);
                            user_state_updated = true;
                        }
                        // Update user list (containing yourself)
                        {
                            user_info_t *user = user_create(
                                changes->users[i].nickname,
                                changes->users[i].id, changes->users[i].state,
                                changes->users[i].score);
                            user_info_t **node =
                                tfind(user, &user_by_id, cmp_by_id);
                            if (changes->users[i].state == UOFFLINE) {
                                // User goes offline
                                log_info("User %u becomes offline", user->id);
                                wprintw(chat_sub, "User `%s' goes offline\n",
                                        changes->users[i].nickname);
                                touchwin(main_win);
                                wrefresh(chat_sub);
                                if (node) {
                                    user_info_t *ptr = *node;
                                    tdelete(ptr, &user_by_id, cmp_by_id);
                                    user_destroy(ptr);
                                    assert(user_cnt > 0);
                                    --user_cnt;
                                }
                                user_destroy(user);
                            } else {
                                if (node) {
                                    log_info("User %u exists", user->id);
                                    // User exists
                                    snprintf((*node)->nickname, NICKNAME_LEN,
                                             "%s", user->nickname);
                                    (*node)->score = user->score;
                                    (*node)->state = user->state;
                                    user_destroy(user);
                                } else {
                                    // New user
                                    log_info("User %u is new", user->id);
                                    wprintw(chat_sub, "User `%s' joined\n",
                                            changes->users[i].nickname);
                                    touchwin(main_win);
                                    wrefresh(chat_sub);
                                    user_info_t **node =
                                        tsearch(user, &user_by_id, cmp_by_id);
                                    assert(node);
                                    assert(*node == user);
                                    ++user_cnt;
                                }
                            }
                            user_list_updated = true;
                        }
                        log_info("User cnt becomes %zu", user_cnt);
                    }
                } break;
                }
                free(um);
            } break;
            }
        }

        if (user_list_updated) {
            ITEM **old_items = menu_items(user_list_menu),
                 **items = xcalloc(user_cnt + 1, sizeof(ITEM *));
            ITEM *selection = current_item(user_list_menu);
            uint16_t old_id, *maybe_old_id;
            if (selection) {
                old_id = *(uint16_t *)item_userptr(selection);
                maybe_old_id = &old_id;
            } else {
                maybe_old_id = NULL;
            }
            // populate new user list
            populate_arg_t arg = {.idx = 0,
                                  .sz = user_cnt,
                                  .selection_id = maybe_old_id,
                                  .items = items,
                                  .selection = NULL,
                                  .self_id = gs.id};
            twalk_r(user_by_id, populate_user_list, &arg);
            items[user_cnt] = NULL;
            // sort the new items
            qsort_r(items, user_cnt, sizeof(ITEM *), cmp_item_for_sort,
                    &sort_by);
            unpost_menu(user_list_menu);
            set_menu_items(user_list_menu, items);
            if (arg.selection) {
                set_current_item(user_list_menu, arg.selection);
            }
            if (old_items) {
                for (size_t i = 0; old_items[i]; ++i) {
                    free((void *)item_name(old_items[i]));
                    free((void *)item_description(old_items[i]));
                    free(item_userptr(old_items[i]));
                    free_item(old_items[i]);
                }
                free(old_items);
            }
            box(user_list_win, 0, 0);
            print_centered(user_list_win, 0, false, "Users (%zu)", user_cnt);
            post_menu(user_list_menu);
            touchwin(main_win);
            wrefresh(user_list_win);
            user_list_updated = false;
        }

        if (user_state_updated) {
            draw_user_state_subwin(player_state_sub, &user_by_id, gs.id, gs.hp,
                                   gs.max_hp);
            draw_user_state_subwin(opponent_state_sub, &user_by_id,
                                   gs.opponent_id, gs.opponent_hp,
                                   gs.opponent_max_hp);
            user_state_updated = false;
        }

        int c;
        while ((c = wgetch(main_win)) != ERR) {
            if (waiting_for == W_CANCEL) {
                message_t *msg = make_challenge();
                msg->body.challenge.action = C_CANCEL;
                msg->body.challenge.id1 = gs.id;
                msg->body.challenge.id2 = gs.opponent_id;
                msg->body.challenge.chid = 0;
                msg->body.challenge.key = gs.key;
                queue_add(send_queue, msg, true);
                hide_panel(popup_panel);
                update_panels();
                doupdate();
                waiting_for = W_NONE;
            } else if (waiting_for == W_ACCEPT) {
                msg_challenge_t ch = {.chid = gs.chid,
                                      .id1 = gs.opponent_id,
                                      .id2 = gs.id,
                                      .key = gs.key};
                if (c == 'y' || c == 'Y') {
                    ch.action = C_ACCEPT;
                } else if (c == 'n' || c == 'N') {
                    ch.action = C_REJECT;
                } else {
                    continue;
                }
                message_t *msg = make_challenge();
                msg->body.challenge = ch;
                queue_add(send_queue, msg, true);
                hide_panel(popup_panel);
                update_panels();
                doupdate();
                waiting_for = W_NONE;
            } else if (waiting_for == W_NONE) {
                switch (c) {
                case '\t':
                    unfocus(foci + focus_idx);
                    focus(foci + (focus_idx = (focus_idx + 1) % focus_cnt));
                    break;
                case KEY_BTAB:
                    unfocus(foci + focus_idx);
                    focus(foci + (focus_idx =
                                      (focus_idx + focus_cnt - 1) % focus_cnt));
                    break;
                default: {
                    user_cmd_t *cmd = focus_handle_key(foci + focus_idx, c);
                    if (cmd == NULL)
                        break;
                    switch (cmd->kind) {
                    case UC_CHL_USER:
                        if (cmd->chl_user_id == gs.id) {
                            wprintw(battle_msg_sub,
                                    "Cannot start battle with yourself.\n");
                            touchwin(battle_msg_win);
                            wrefresh(battle_msg_sub);
                            break;
                        }
                        msg_challenge_t ch = {.action = C_START,
                                              .chid = 0,
                                              .id1 = gs.id,
                                              .id2 = cmd->chl_user_id,
                                              .key = gs.key};
                        message_t *msg = make_challenge();
                        msg->body.challenge = ch;
                        queue_add(send_queue, msg, true);
                        waiting_for = W_CANCEL;
                        wclear(popup_sub);
                        print_centered(popup_sub, 3, false,
                                       "Inviting `%s' to battle (press any key "
                                       "to cancel)...",
                                       cmd->chl_user_name);
                        touchwin(popup_win);
                        wrefresh(popup_sub);
                        show_panel(popup_panel);
                        update_panels();
                        doupdate();
                        break;
                    case UC_BATTLE_ACT:
                        if (gs.state != UBATTLING) {
                            wprintw(battle_msg_sub, "Not in a battle.\n");
                            touchwin(battle_msg_win);
                            wrefresh(battle_msg_sub);
                        } else if (gs.acted) {
                            wprintw(battle_msg_sub, "You've already acted.\n");
                            touchwin(battle_msg_win);
                            wrefresh(battle_msg_sub);
                        } else {
                            message_t *msg = make_msg_buf(TURN);
                            msg_turn_t turn = {.turn_no = gs.turn_no,
                                               .action = cmd->b_act,
                                               .chid = gs.chid,
                                               .key = gs.key,
                                               .user = gs.id};
                            gs.acted = true;
                            msg->body.turn = turn;
                            queue_add(send_queue, msg, true);
                            wprintw(battle_msg_sub,
                                    "You've chosen %s. Waiting for the other "
                                    "user.\n",
                                    act_name(cmd->b_act));
                            touchwin(battle_msg_win);
                            wrefresh(battle_msg_sub);
                        }
                        break;
                    case UC_SORT:
                        sort_by = cmd->sort_by;
                        user_list_updated = true;
                        break;
                    case UC_SENDMSG: {
                        message_t *msg = make_msg_buf(SENDMSG);
                        msg->body.sendmsg.id = gs.id;
                        msg->body.sendmsg.key = gs.key;
                        form_driver(chat_input, REQ_VALIDATION);
                        size_t sz = snprintf(msg->body.sendmsg.text,
                                             sizeof(msg->body.sendmsg.text),
                                             "%s", field_buffer(fields[0], 0));
                        while (sz > 0 &&
                               isspace(msg->body.sendmsg.text[sz - 1])) {
                            msg->body.sendmsg.text[--sz] = '\0';
                        }
                        queue_add(send_queue, msg, true);
                        form_driver(chat_input, REQ_CLR_FIELD);
                    } break;
                    case UC_QUIT:
                        goto done;
                    case UC_MAX:
                        // fall through
                    default:
                        assert(0);
                        break;
                    }
                } break;
                }
            }
        }
        nanosleep(&rqtp, &rmtp);
    }

done:
    free_menu(main_menu);
    free_menu(user_list_menu);
    free_menu(battle_act_menu);
    del_panel(popup_panel);
    del_panel(main_panel);
    free_form(chat_input);

    delwin(battle_act_sub);
    delwin(battle_act_win);
    delwin(player_state_sub);
    delwin(player_state_win);
    delwin(battle_msg_sub);
    delwin(battle_msg_win);
    delwin(opponent_state_sub);
    delwin(opponent_state_win);
    delwin(battle_act_sub);
    delwin(battle_act_win);
    delwin(arena);

    delwin(chat_sub);
    delwin(chat_input_sub);
    delwin(main_menu_sub);
    delwin(main_menu_win);
    delwin(user_list_sub);
    delwin(user_list_win);
    delwin(popup_sub);
    delwin(popup_win);
    delwin(main_win);

    clear();
    refresh();
    {
        message_t *quit = make_msg_buf(QUIT);
        quit->body.quit.id = gs.id;
        quit->body.quit.key = gs.key;
        queue_add(send_queue, quit, true);
    }
    return UI_INIT;
}

static void *ui(void *_arg) {
    if (initscr() == NULL)
        ppanic("initscr()");
    if (cbreak() == ERR)
        ppanic("cbreak()");

    WINDOW *root = NULL;
    queue_t *um_queue = NULL, *send_queue = NULL;
    ui_state_t state = UI_INIT;
    pthread_t precv, psend;
    message_t *join_msg;

    while (1) {
        switch (state) {
        case UI_INIT:
            log_info("Initing");
            state = ui_init(&root, &um_queue, &send_queue);
            break;
        case UI_LOGIN:
            log_info("Loginning");
            state =
                ui_login(root, &precv, &psend, um_queue, send_queue, &join_msg);
            break;
        case UI_MAIN:
            state =
                ui_main(root, &precv, &psend, um_queue, send_queue, join_msg);
            break;
        case UI_EXIT:
            goto fin;
            break;
        }
    }

fin:
    endwin();
    return 0;
}

void signal_handlers_init() { signal(SIGPIPE, SIG_IGN); }

int main(int argc, char **argv) {
    set_loglevel(LOGLV_MIN); // disable logging
    setlocale(LC_ALL, "");

    parse_args(argc, argv, initial_addr, ADDR_MAX_LEN, &initial_port,
               argc == 0 ? APPNAME : argv[0], "SERVER_ADDR");
    signal_handlers_init();

    pthread_t pui;
    create_thread(&pui, ui, NULL, 0);

    pthread_join(pui, NULL);
}
