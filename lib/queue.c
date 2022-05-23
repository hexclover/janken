#include "common.h"

struct queue_t {
    size_t cap, lo, hi, sz;
    pthread_mutex_t mutex;
    pthread_cond_t conde, condf;
    void **data;
};

queue_t *queue_create(size_t cap) {
    queue_t *res = xmalloc(sizeof(*res));
    pthread_mutex_init(&res->mutex, NULL);
    pthread_cond_init(&res->conde, NULL);
    pthread_cond_init(&res->condf, NULL);
    res->data = xmalloc(sizeof(*res->data) * cap);
    res->cap = cap;
    res->lo = res->hi = res->sz = 0;
    return res;
}

queue_err_t queue_add(queue_t *q, void *e, bool block) {
    pthread_mutex_lock(&q->mutex);
    queue_err_t res = QOK;
    while (q->cap == q->sz) {
        if (!block) {
            res = QFULL;
            goto fin;
        } else {
            pthread_cond_wait(&q->conde, &q->mutex);
        }
    }
    q->data[q->hi] = e;
    q->hi = (q->hi + 1) % q->cap;
    ++q->sz;
    res = QOK;
fin:
    pthread_mutex_unlock(&q->mutex);
    pthread_cond_signal(&q->condf);
    return res;
}

queue_err_t queue_take(queue_t *q, void **p, bool block) {
    pthread_mutex_lock(&q->mutex);
    queue_err_t res = QOK;
    while (q->sz == 0) {
        if (!block) {
            res = QEMPTY;
            goto fin;
        } else {
            pthread_cond_wait(&q->condf, &q->mutex);
        }
    }
    *p = q->data[q->lo];
    q->lo = (q->lo + 1) % q->cap;
    --q->sz;
fin:
    pthread_mutex_unlock(&q->mutex);
    pthread_cond_signal(&q->conde);
    return res;
}

void queue_destroy(queue_t *q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->conde);
    pthread_cond_destroy(&q->condf);
    free(q->data);
    free(q);
}
