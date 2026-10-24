#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "sync.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct Gallery Gallery;

typedef enum {
    NEW,
    QUEUED,
    RESERVED,
    INSIDE,
    DONE
} State;

typedef struct {
    Gallery *gallery;
    unsigned id;
    unsigned order[MAX_PAINTINGS];
    unsigned duration[MAX_PAINTINGS];
    unsigned viewed;
    bool seen[MAX_PAINTINGS];
    int current;
    int next;
    State state;
    Gate admission;
    pthread_t thread;
} Visitor;

struct Gallery {
    Config config;
    pthread_mutex_t mutex;
    Gate changed;
    Gate picture_slots[MAX_PAINTINGS];
    Visitor *visitors;
    int head;
    int tail;
    unsigned queued;
    unsigned inside;
    unsigned reserved;
    unsigned created;
    unsigned registered;
    unsigned arrived;
    unsigned entered;
    unsigned exited;
    unsigned completed;
    unsigned cancelled;
    unsigned inspections;
    unsigned peak_inside;
    unsigned peak_committed;
    unsigned peak_queue;
    unsigned viewers[MAX_PAINTINGS];
    unsigned peak_viewers[MAX_PAINTINGS];
    unsigned picture_views[MAX_PAINTINGS];
    bool generation_done;
    bool io_failed;
    bool runtime_failed;
    atomic_bool stop;
    int stop_signal;
    int log_fd;
    bool stdout_failed;
    uint64_t sequence;
    struct timespec start;
    sigset_t signals;
};

static void check(int rc) {
    if (rc != 0) {
        abort();
    }
}

static void lock(Gallery *g) {
    check(pthread_mutex_lock(&g->mutex));
}

static void unlock(Gallery *g) {
    check(pthread_mutex_unlock(&g->mutex));
}

static bool write_all(int fd, const char *buf, size_t size) {
    while (size) {
        ssize_t n = write(fd, buf, size);
        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return false;
        }

        buf += n;
        size -= (size_t)n;
    }

    return true;
}

static uint64_t elapsed_ms(const Gallery *g) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        abort();
    }

    int64_t ns = (int64_t)(now.tv_sec-g->start.tv_sec)*INT64_C(1000000000)
        + now.tv_nsec-g->start.tv_nsec;
    return (uint64_t)(ns/1000000);
}

static void invariants(const Gallery *g) {
    assert(g->inside + g->reserved <= g->config.capacity);
    unsigned sum = 0;
    for (unsigned p = 0; p < g->config.paintings; p++) {
        assert(g->viewers[p] <= g->config.picture_capacity[p]);
        sum += g->viewers[p];
    }

    assert(sum <= g->inside);
    assert(g->entered >= g->exited && g->entered-g->exited == g->inside);
}

static void event(Gallery *g, bool always, const char *type, unsigned id,
    int picture, const char *format, ...) {
    invariants(g);
    char detail[4096];
    char line[4608];
    va_list args;
    va_start(args, format);
    int dn = vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    if (dn < 0 || (size_t)dn >= sizeof(detail)) {
        abort();
    }

    g->sequence++;
    int n = snprintf(line, sizeof(line),
        "seq=%" PRIu64 " ms=%" PRIu64 " event=%s visitor=%u picture=%d "
        "inside=%u reserved=%u queue=%u %s\n",
        g->sequence, elapsed_ms(g), type, id, picture+1,
        g->inside, g->reserved, g->queued, detail);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        abort();
    }

    if ((!g->config.quiet || always) && !g->stdout_failed &&
        !write_all(STDOUT_FILENO, line, (size_t)n)) {
        g->stdout_failed = true;
        g->io_failed = true;
        dprintf(STDERR_FILENO, "Ошибка записи stdout: %s\n", strerror(errno));
    }

    if (g->log_fd >= 0 && !write_all(g->log_fd, line, (size_t)n)) {
        g->io_failed = true;
        dprintf(STDERR_FILENO, "Ошибка записи журнала: %s\n", strerror(errno));
        close(g->log_fd);
        g->log_fd = -1;
    }
}

static uint64_t random_next(uint64_t *state) {
    *state += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t z = *state;
    z = (z ^ (z >> 30))*UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27))*UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static unsigned random_range(uint64_t *state, unsigned lo, unsigned hi) {
    uint64_t bound = (uint64_t)hi-lo+1;
    uint64_t threshold = (uint64_t)(-bound)%bound;
    uint64_t value;
    do {
        value = random_next(state);
    } while (value < threshold);
    return lo+(unsigned)(value%bound);
}

static bool delay_ms(Gallery *g, unsigned ms) {
    while (ms && !atomic_load(&g->stop)) {
        unsigned step = ms > 10 ? 10 : ms;
        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = (long)step*1000000L
        };
        while (nanosleep(&delay, &delay) != 0) {
            if (errno != EINTR) {
                abort();
            }

            if (atomic_load(&g->stop)) {
                return false;
            }
        }

        ms -= step;
    }

    return !atomic_load(&g->stop);
}

static void enqueue(Gallery *g, Visitor *v) {
    int index = (int)(v-g->visitors);
    v->next = -1;
    v->state = QUEUED;
    if (g->tail < 0) {
        g->head = index;
    } else {
        g->visitors[g->tail].next = index;
    }

    g->tail = index;
    g->queued++;
    if (g->queued > g->peak_queue) {
        g->peak_queue = g->queued;
    }
}

static void remove_from_queue(Gallery *g, Visitor *v) {
    int index = (int)(v-g->visitors);
    int prev = -1;
    int cur = g->head;
    while (cur != index) {
        assert(cur >= 0);
        prev = cur;
        cur = g->visitors[cur].next;
    }

    if (prev < 0) {
        g->head = v->next;
    } else {
        g->visitors[prev].next = v->next;
    }

    if (g->tail == index) {
        g->tail = prev;
    }

    v->next = -1;
    g->queued--;
}

static void *warden_main(void *arg) {
    Gallery *g = arg;
    for (; ; ) {
        lock(g);
        if (atomic_load(&g->stop)) {
            event(g, false, "ENTRY_CLOSED", 0, -1, "Вахтёр закрывает вход: остановка.");
            unlock(g);
            break;
        }
        while (g->head >= 0 && g->inside+g->reserved < g->config.capacity) {
            Visitor *v = &g->visitors[g->head];
            remove_from_queue(g, v);
            v->state = RESERVED;
            g->reserved++;
            if (g->inside+g->reserved > g->peak_committed) {
                g->peak_committed = g->inside+g->reserved;
            }

            event(g, false, "PERMIT", v->id, -1, "Вахтёр разрешил вход и зарезервировал место.");
            gate_post(&v->admission);
        }

        if (g->generation_done && g->registered == g->created && g->queued == 0) {
            event(g, false, "ENTRY_CLOSED", 0, -1, "Вахтёр выдал все разрешения; новых посетителей не будет.");
            unlock(g);
            break;
        }

        unlock(g);
        (void)gate_wait(&g->changed, &g->stop);
    }

    return NULL;
}

static void *visitor_main(void *arg) {
    Visitor *v = arg;
    Gallery *g = v->gallery;
    lock(g);
    g->registered++;
    if (atomic_load(&g->stop)) {
        v->state = DONE;
        gate_post(&g->changed);
        unlock(g);
        return NULL;
    }

    g->arrived++;
    event(g, false, "ARRIVE", v->id, -1, "Посетитель прибыл к галерее.");
    enqueue(g, v);
    event(g, false, "WAIT_ENTRY", v->id, -1, "Ожидание разрешения вахтёра в очереди FIFO.");
    gate_post(&g->changed);
    unlock(g);
    bool permitted = gate_wait(&v->admission, &g->stop);
    lock(g);
    if (!permitted || atomic_load(&g->stop)) {
        if (v->state == QUEUED) {
            remove_from_queue(g, v);
        } else {
            assert(v->state == RESERVED);
            g->reserved--;
        }

        v->state = DONE;
        g->cancelled++;
        event(g, false, "CANCEL_ENTRY", v->id, -1, "Остановка: посетитель ушёл, не войдя в галерею.");
        gate_post(&g->changed);
        unlock(g);
        return NULL;
    }

    assert(v->state == RESERVED);
    g->reserved--;
    g->inside++;
    g->entered++;
    v->state = INSIDE;
    if (g->inside > g->peak_inside) {
        g->peak_inside = g->inside;
    }

    event(g, false, "ENTER", v->id, -1, "Посетитель вошёл в галерею.");
    unlock(g);
    for (unsigned step = 0; step < g->config.paintings && !atomic_load(&g->stop); step++) {
        unsigned p = v->order[step];
        lock(g);
        assert(v->current == -1 && !v->seen[p]);
        event(g, false, "CHOOSE", v->id, (int)p, "view_ms=%u Выбрана следующая картина.", v->duration[p]);
        bool got = gate_try_wait(&g->picture_slots[p]);
        if (!got) {
            event(g, false, "WAIT_PICTURE", v->id, (int)p, "Ожидание свободного места возле картины.");
        }

        unlock(g);
        if (!got) {
            got = gate_wait(&g->picture_slots[p], &g->stop);
        }

        if (!got) {
            break;
        }

        lock(g);
        if (atomic_load(&g->stop)) {
            gate_post(&g->picture_slots[p]);
            unlock(g);
            break;
        }

        v->current = (int)p;
        g->viewers[p]++;
        if (g->viewers[p] > g->peak_viewers[p]) {
            g->peak_viewers[p] = g->viewers[p];
        }

        event(g, false, "VIEW_BEGIN", v->id, (int)p, "viewers=%u limit=%u Начало осмотра.", g->viewers[p], g->config.picture_capacity[p]);
        unlock(g);
        bool finished = delay_ms(g, v->duration[p]);
        lock(g);
        assert(v->current == (int)p && !v->seen[p]);
        v->current = -1;
        g->viewers[p]--;
        if (finished && !atomic_load(&g->stop)) {
            v->seen[p] = true;
            v->viewed++;
            g->inspections++;
            g->picture_views[p]++;
            event(g, false, "VIEW_END", v->id, (int)p, "viewers=%u viewed=%u Осмотр завершён; место освобождено.", g->viewers[p], v->viewed);
        } else {
            event(g, false, "VIEW_ABORT", v->id, (int)p, "viewers=%u Осмотр прерван; место освобождено.", g->viewers[p]);
        }

        gate_post(&g->picture_slots[p]);
        unlock(g);
    }

    lock(g);
    assert(v->current == -1 && v->state == INSIDE);
    g->inside--;
    g->exited++;
    v->state = DONE;
    bool complete = v->viewed == g->config.paintings;
    if (complete) {
        g->completed++;
    } else {
        g->cancelled++;
    }

    event(g, false, "EXIT", v->id, -1, "complete=%u viewed=%u Посетитель вышел; место в галерее освобождено.", complete ? 1U:0U, v->viewed);
    gate_post(&g->changed);
    unlock(g);
    return NULL;
}

/* SIGINT/SIGTERM заблокированы во всех потоках; только этот поток ждёт sigwait.
 * Здесь можно безопасно брать mutex и писать журнал: это не обработчик сигнала. */
static void *signal_main(void *arg) {
    Gallery *g = arg;
    for (; ; ) {
        int sig;
        check(sigwait(&g->signals, &sig));
        /* Служебное завершение после join. */
        if (sig == SIGUSR1) {
            return NULL;
        }

        lock(g);
        if (!atomic_load(&g->stop)) {
            g->stop_signal = sig;
            atomic_store(&g->stop, true);
            event(g, false, "STOP", 0, -1, "signal=%d Получен сигнал; начинается освобождение ресурсов.", sig);
        }

        unlock(g);
    }
}

static void make_plan(Gallery *g, Visitor *v, unsigned arrival) {
    uint64_t rng = (uint64_t)g->config.seed ^ (UINT64_C(0xd1b54a32d192ed03)*v->id);
    unsigned n = g->config.paintings;
    for (unsigned p = 0; p < n; p++) {
        v->order[p] = p;
        v->duration[p] = random_range(&rng, g->config.view_min, g->config.view_max);
    }

    if (g->config.random_order) {
        for (unsigned i = n; i > 1; i--) {
            unsigned j = random_range(&rng, 0, i-1);
            unsigned tmp = v->order[i-1];
            v->order[i-1] = v->order[j];
            v->order[j] = tmp;
        }
    }

    char route[512] = "";
    char durations[1024] = "";
    size_t a = 0;
    size_t b = 0;
    for (unsigned i = 0; i < n; i++) {
        a += (size_t)snprintf(route+a, sizeof(route)-a, "%s%u", i ? ",":"", v->order[i]+1);
        b += (size_t)snprintf(durations+b, sizeof(durations)-b, "%s%u", i ? ",":"", v->duration[i]);
    }

    event(g, false, "PLAN", v->id, -1, "arrival_ms=%u order=%s durations=%s План осмотра (времена по номерам картин).", arrival, route, durations);
}

int main(int argc, char **argv) {
    Gallery g = {
        .head = -1,
        .tail = -1,
        .log_fd = -1
    };
    atomic_init(&g.stop, false);
    int parsed = parse_config(argc, argv, &g.config);
    if (parsed) {
        return parsed > 0 ? 0 : 2;
    }

    /* SIGPIPE не должен прервать освобождение ресурсов при закрытии stdout. */
    struct sigaction ignore = {
        .sa_handler = SIG_IGN
    };
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, NULL)) {
        dprintf(2, "Ошибка sigaction\n");
        return 1;
    }

    sigemptyset(&g.signals);
    sigaddset(&g.signals, SIGINT);
    sigaddset(&g.signals, SIGTERM);
    sigaddset(&g.signals, SIGUSR1);
    int rc = pthread_sigmask(SIG_BLOCK, &g.signals, NULL);
    if (rc) {
        dprintf(2, "pthread_sigmask: %s\n", strerror(rc));
        return 1;
    }

    if (strcmp(g.config.log_path, "none")) {
        g.log_fd = open(g.config.log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g.log_fd < 0) {
            dprintf(2, "Не открыть журнал: %s\n", strerror(errno));
            return 1;
        }
    }

    g.visitors = calloc(g.config.visitors ? g.config.visitors:1, sizeof(*g.visitors));
    if (!g.visitors) {
        dprintf(2, "Недостаточно памяти\n");
        if (g.log_fd >= 0) {
            close(g.log_fd);
        }

        return 1;
    }

    unsigned init_pictures = 0;
    unsigned init_visitors = 0;
    bool mutex_ready = false;
    bool changed_ready = false;
    rc = pthread_mutex_init(&g.mutex, NULL);
    if (rc) {
        goto init_failed;
    }

    mutex_ready = true;
    rc = gate_init(&g.changed, 0);
    if (rc) {
        goto init_failed;
    }

    changed_ready = true;
    for (; init_pictures < g.config.paintings; init_pictures++) {
        rc = gate_init(&g.picture_slots[init_pictures], g.config.picture_capacity[init_pictures]);
        if (rc) {
            goto init_failed;
        }
    }

    for (; init_visitors < g.config.visitors; init_visitors++) {
        Visitor *v = &g.visitors[init_visitors];
        v->gallery = &g;
        v->id = init_visitors+1;
        v->next = -1;
        v->current = -1;
        rc = gate_init(&v->admission, 0);
        if (rc) {
            goto init_failed;
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &g.start)) {
        rc = errno;
        goto init_failed;
    }

    event(&g, false, "START", 0, -1, "backend=%s capacity=%u paintings=%u visitors=%u seed=%" PRIu32 " strategy=%s Открытие галереи.",
        sync_backend(), g.config.capacity, g.config.paintings, g.config.visitors, g.config.seed,
        g.config.random_order ? "random":"sequential");
    for (unsigned p = 0; p < g.config.paintings; p++) {
        event(&g, false, "PICTURE_CONFIG", 0, (int)p, "limit=%u Вместимость зоны картины.", g.config.picture_capacity[p]);
    }

    pthread_t signal_thread;
    pthread_t warden;
    rc = pthread_create(&signal_thread, NULL, signal_main, &g);
    if (rc) {
        goto init_failed;
    }

    rc = pthread_create(&warden, NULL, warden_main, &g);
    if (rc) {
        check(pthread_kill(signal_thread, SIGUSR1));
        check(pthread_join(signal_thread, NULL));
        goto init_failed;
    }

    uint64_t arrivals_rng = (uint64_t)g.config.seed ^ UINT64_C(0xa0761d6478bd642f);
    for (unsigned i = 0; i < g.config.visitors; i++) {
        unsigned interval = random_range(&arrivals_rng, g.config.arrival_min, g.config.arrival_max);
        if (!delay_ms(&g, interval)) {
            break;
        }

        lock(&g);
        if (atomic_load(&g.stop)) {
            unlock(&g);
            break;
        }

        make_plan(&g, &g.visitors[i], interval);
        rc = pthread_create(&g.visitors[i].thread, NULL, visitor_main, &g.visitors[i]);
        if (rc) {
            g.runtime_failed = true;
            atomic_store(&g.stop, true);
            event(&g, false, "ERROR", 0, -1, "pthread_create: %s", strerror(rc));
            unlock(&g);
            break;
        }

        g.created++;
        unlock(&g);
    }

    lock(&g);
    g.generation_done = true;
    event(&g, false, "ARRIVALS_CLOSED", 0, -1, "created=%u Поток новых прибытий завершён.", g.created);
    gate_post(&g.changed);
    unlock(&g);
    for (unsigned i = 0; i < g.created; i++) {
        check(pthread_join(g.visitors[i].thread, NULL));
    }

    check(pthread_join(warden, NULL));
    check(pthread_kill(signal_thread, SIGUSR1));
    check(pthread_join(signal_thread, NULL));
    invariants(&g);
    assert(g.inside == 0 && g.reserved == 0 && g.queued == 0);
    assert(g.arrived == g.completed+g.cancelled);
    if (!atomic_load(&g.stop)) {
        assert(g.completed == g.config.visitors);
    }

    for (unsigned p = 0; p < g.config.paintings; p++) {
        assert(g.viewers[p] == 0);
        event(&g, true, "PICTURE_STATS", 0, (int)p, "views=%u peak=%u limit=%u Итог по картине.", g.picture_views[p], g.peak_viewers[p], g.config.picture_capacity[p]);
    }

    event(&g, true, "SUMMARY", 0, -1,
        "status=%s planned=%u created=%u arrived=%u entered=%u exited=%u completed=%u cancelled=%u not_arrived=%u inspections=%u peak_inside=%u peak_committed=%u peak_queue=%u Все потоки завершены.",
        g.runtime_failed ? "error":(g.stop_signal ? "interrupted":(g.io_failed ? "io_error":"completed")),
        g.config.visitors, g.created, g.arrived, g.entered, g.exited, g.completed, g.cancelled,
        g.config.visitors-g.arrived, g.inspections, g.peak_inside, g.peak_committed, g.peak_queue);
    rc = g.runtime_failed || g.io_failed ? 1:(g.stop_signal ? 128+g.stop_signal:0);
    goto cleanup;
init_failed:
    dprintf(2, "Ошибка инициализации: %s\n", strerror(rc));
    rc = 1;
cleanup:
    for (unsigned i = 0; i < init_visitors; i++) {
        gate_destroy(&g.visitors[i].admission);
    }

    for (unsigned p = 0; p < init_pictures; p++) {
        gate_destroy(&g.picture_slots[p]);
    }

    if (changed_ready) {
        gate_destroy(&g.changed);
    }

    if (mutex_ready) {
        check(pthread_mutex_destroy(&g.mutex));
    }

    free(g.visitors);
    if (g.log_fd >= 0 && close(g.log_fd) != 0) {
        dprintf(2, "Ошибка закрытия журнала\n");
        rc = 1;
    }

    return rc;
}
