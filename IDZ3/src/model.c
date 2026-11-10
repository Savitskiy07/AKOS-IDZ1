#include "gallery.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Каждый посетитель владеет своим маршрутом и сам посылает запросы. */
int visitor_run(Ipc *ipc, int id) {
    Plan p;
    if (ipc_send(ipc, (Request){id, JOIN, 0}) || ipc_get_plan(ipc, &p, id)) {
        return -1;
    }

    uint32_t schedule = p.seed;
    int64_t arrival = p.epoch;
    for (int i = 1; i < id; i++) {
        arrival += sample(&schedule, p.arrival_min, p.arrival_max);
    }
    if (sleep_until(ipc, arrival) ||
        ipc_send(ipc, (Request){id, ARRIVE, 0}) ||
        ipc_await(ipc, id) ||
        ipc_send(ipc, (Request){id, ENTER, 0})) {
        return -1;
    }

    int route[MAX_PAINTINGS];
    for (int i = 0; i < p.paintings; i++) {
        route[i] = i + 1;
    }
    uint32_t rng = p.seed ^ ((uint32_t)id * UINT32_C(0x9e3779b9));
    if (p.random_order) {
        /* Перестановка Фишера - Йетса сохраняет один экземпляр картины. */
        for (int i = p.paintings - 1; i > 0; i--) {
            int j = sample(&rng, 0, i), temp = route[i];
            route[i] = route[j];
            route[j] = temp;
        }
    }

    for (int i = 0; i < p.paintings; i++) {
        int picture = route[i];
        if (ipc_send(ipc, (Request){id, SELECT_PICTURE, picture}) || ipc_await(ipc, id)) {
            return -1;
        }
        int duration = sample(&rng, p.view_min, p.view_max);
        if (sleep_until(ipc, now_ms() + duration) ||
            ipc_send(ipc, (Request){id, FINISH, picture})) {
            return -1;
        }
    }
    return ipc_send(ipc, (Request){id, LEAVE, 0});
}

typedef enum { NEW, REGISTERED, OUTSIDE, ADMITTED, INSIDE, WAITING, VIEWING, GONE } Phase;

typedef struct {
    Phase phase;
    int picture, finished;
    uint64_t seen;
} Person;

typedef struct {
    int ids[MAX_VISITORS], head, length;
} Queue;

typedef struct {
    Queue queue;
    int viewers;
} Painting;

typedef struct {
    const Config *cfg;
    Ipc *ipc;
    Person people[1 + MAX_VISITORS];
    Painting art[1 + MAX_PAINTINGS];
    Queue entrance;
    int joined, entered, exited, inside, reserved, visits, max_inside, max_near;
    int logfd, seq;
    int64_t start;
} Gallery;

static void push(Queue *q, int id) {
    q->ids[(q->head + q->length) % MAX_VISITORS] = id;
    q->length++;
}

static int pop(Queue *q) {
    int id = q->ids[q->head];
    q->head = (q->head + 1) % MAX_VISITORS;
    q->length--;
    return id;
}

static int emit(Gallery *g, const char *code, int id, int p, const char *description) {
    char line[768];
    g->seq++;
    int len = snprintf(
        line, sizeof(line),
        "%06d +%06" PRId64 "ms %-13s v=%d p=%d hall=%d reserved=%d near=%d | %s\n",
        g->seq, now_ms() - g->start, code, id, p, g->inside, g->reserved,
        p ? g->art[p].viewers : 0, description
    );
    if (len < 0 || (size_t)len >= sizeof(line)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (g->logfd >= 0 && write_all(g->logfd, line, (size_t)len)) {
        return -1;
    }
    return write_all(STDOUT_FILENO, line, (size_t)len);
}

static int open_day(Gallery *g) {
    const Config *c = g->cfg;
    Plan p = {
        .epoch = now_ms(),
        .paintings = c->paintings,
        .arrival_min = c->arrival_min,
        .arrival_max = c->arrival_max,
        .view_min = c->view_min,
        .view_max = c->view_max,
        .random_order = c->random_order,
        .seed = c->seed
    };

    if (emit(g, "OPEN", 0, 0, "Регистрация завершена. Галерея открылась.")) {
        return -1;
    }
    for (int id = 1; id <= c->visitors; id++) {
        if (ipc_give_plan(g->ipc, &p, id)) {
            return -1;
        }
    }
    return 0;
}

/* Резерв защищает свободное место
   между выдачей персонального разрешения и подтверждением входа. */
static int admit(Gallery *g) {
    if (!g->entrance.length || g->inside + g->reserved >= g->cfg->capacity) {
        return 0;
    }

    int id = pop(&g->entrance);
    g->people[id].phase = ADMITTED;
    g->reserved++;
    if (emit(g, "PERMIT", id, 0, "Вахтёр разрешил вход.")) {
        return -1;
    }
    return ipc_grant(g->ipc, id);
}

static int start_view(Gallery *g, int p) {
    Painting *a = &g->art[p];
    if (!a->queue.length || a->viewers >= g->cfg->limits[p - 1]) {
        return 0;
    }

    int id = pop(&a->queue);
    a->viewers++;
    if (a->viewers > g->max_near) {
        g->max_near = a->viewers;
    }
    g->people[id].phase = VIEWING;
    if (emit(g, "BEGIN", id, p, "Посетитель начал осмотр картины.")) {
        return -1;
    }
    return ipc_grant(g->ipc, id);
}

static int dispatch(Gallery *g, Request r) {
    int id = r.id, p = r.picture;
    if (id < 1 || id > g->cfg->visitors) {
        goto invalid;
    }

    Person *v = &g->people[id];
    switch (r.action) {
    case JOIN:
        if (v->phase != NEW) {
            goto invalid;
        }
        v->phase = REGISTERED;
        g->joined++;
        return g->joined == g->cfg->visitors ? open_day(g) : 0;

    case ARRIVE:
        if (v->phase != REGISTERED) {
            goto invalid;
        }
        v->phase = OUTSIDE;
        push(&g->entrance, id);
        if (emit(g, "ARRIVE", id, 0, "Посетитель прибыл к галерее.")) {
            return -1;
        }
        if (g->inside + g->reserved == g->cfg->capacity &&
            emit(g, "WAIT_ENTRY", id, 0, "Мест в галерее нет. Посетитель ждёт у входа.")) {
            return -1;
        }
        return admit(g);

    case ENTER:
        if (v->phase != ADMITTED) {
            goto invalid;
        }
        v->phase = INSIDE;
        g->reserved--;
        g->inside++;
        if (g->inside > g->max_inside) {
            g->max_inside = g->inside;
        }
        if (emit(g, "ENTER", id, 0, "Посетитель вошёл в галерею.")) {
            return -1;
        }
        g->entered++;
        if (g->entered == g->cfg->visitors) {
            return emit(g, "CLOSE", 0, 0, "Последний посетитель вошёл. Вахтёр закрыл вход.");
        }
        return 0;

    case SELECT_PICTURE:
        if (v->phase != INSIDE || p < 1 || p > g->cfg->paintings ||
            (v->seen & (UINT64_C(1) << (p - 1)))) {
            goto invalid;
        }
        v->phase = WAITING;
        v->picture = p;
        push(&g->art[p].queue, id);
        if (emit(g, "CHOOSE", id, p, "Посетитель выбрал картину.")) {
            return -1;
        }
        if (g->art[p].viewers == g->cfg->limits[p - 1] &&
            emit(g, "WAIT_PICTURE", id, p, "Все места у картины заняты. Посетитель ждёт.")) {
            return -1;
        }
        return start_view(g, p);

    case FINISH:
        if (v->phase != VIEWING || p != v->picture) {
            goto invalid;
        }
        v->phase = INSIDE;
        v->picture = 0;
        v->seen |= UINT64_C(1) << (p - 1);
        v->finished++;
        g->visits++;
        g->art[p].viewers--;
        if (emit(g, "END", id, p, "Посетитель закончил осмотр и освободил место.")) {
            return -1;
        }
        return start_view(g, p);

    case LEAVE:
        if (v->phase != INSIDE || v->finished != g->cfg->paintings) {
            goto invalid;
        }
        v->phase = GONE;
        g->inside--;
        g->exited++;
        if (emit(g, "EXIT", id, 0, "Посетитель осмотрел все картины и вышел.")) {
            return -1;
        }
        return admit(g);
    }

invalid:
    errno = EPROTO;
    return -1;
}

int guard_run(Ipc *ipc, const Config *cfg) {
    Gallery g = {.cfg = cfg, .ipc = ipc, .logfd = -1, .start = now_ms()};
    if (cfg->log_path) {
        g.logfd = open(cfg->log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g.logfd < 0) {
            return -1;
        }
    }

    char limits[4 * MAX_PAINTINGS + 1];
    size_t used = 0;
    for (int i = 0; i < cfg->paintings; i++) {
        used += (size_t)snprintf(limits + used, sizeof(limits) - used, "%s%d",
                                 i ? "," : "", cfg->limits[i]);
    }
    char note[512];
    snprintf(
        note, sizeof(note),
        "Вахтёр ожидает участников. visitors=%d capacity=%d paintings=%d limits=%s seed=%u",
        cfg->visitors, cfg->capacity, cfg->paintings, limits, cfg->seed
    );

    int result = emit(&g, "READY", 0, 0, note);
    while (!result && g.exited < cfg->visitors) {
        Request r;
        result = ipc_receive(ipc, &r);
        if (!result) {
            result = dispatch(&g, r);
        }
    }

    int saved = errno;
    snprintf(
        note, sizeof(note),
        "%s Вышли %d из %d. Осмотров %d. max_hall=%d max_near=%d",
        result ? "День прерван." : "Рабочий день завершён.",
        g.exited, cfg->visitors, g.visits, g.max_inside, g.max_near
    );
    if (emit(&g, result ? "STOP" : "SUMMARY", 0, 0, note) && !result) {
        result = -1;
        saved = errno;
    }
    if (g.logfd >= 0 && close(g.logfd) < 0 && !result) {
        result = -1;
        saved = errno;
    }
    errno = saved;
    return result;
}
