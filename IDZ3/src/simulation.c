#include "simulation.h"
#include "io.h"
#include "random.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum {
    NEW, ARRIVED, WAITING, RESERVED, INSIDE, FINISHED, CANCELLED, INTERRUPTED
} VisitorState;

typedef struct {
    int viewers;
    int peak;
    int completed_views;
    int waiting[MAX_VISITORS];
    int waiting_count;
} Painting;

typedef struct {
    int id;
    int order[MAX_PAINTINGS];
    int duration_ms[MAX_PAINTINGS];
    int next_step;
    VisitorState state;
    int current_painting; /* -1: не осматривает картину */
    int waiting_painting; /* -1: не ждёт места у картины */
    uint64_t view_end_ms;
    uint64_t seen; /* бит i: осмотр картины i завершён */
} Visitor;

typedef struct {
    const Config *config;
    Logger logger;
    Visitor *visitors;
    Painting paintings[MAX_PAINTINGS];
    int *entry_queue;
    int queue_head;
    int queue_count;

    int created;
    int inside;
    int reserved;
    int completed;
    int interrupted;
    int cancelled;
    int total_views;
    int peak_inside;
    int peak_occupied;
    int peak_viewers;
    int peak_queue;
    int entry_waits;
    int painting_waits;

    uint64_t now_ms;
    bool stopping;
    bool failed;
    int stop_signal;
    const char *reason;

    struct sigaction old_sigint;
    struct sigaction old_sigterm;
    bool signal_handlers_installed;
} Gallery;

static volatile sig_atomic_t pending_signal;

static void require(bool condition, const char *expression, int line) {
    if (!condition) {
        dprintf(STDERR_FILENO, "Нарушен внутренний инвариант, строка %d: %s\n",
                line, expression);
        abort();
    }
}
#define REQUIRE(condition) require((condition), #condition, __LINE__)

static uint64_t complete_mask(const Gallery *gallery) {
    int count = gallery->config->paintings;
    return count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1;
}

/* короткая внутренняя проверка */
static void audit(const Gallery *gallery) {
    const Config *config = gallery->config;
    int at_painting[MAX_PAINTINGS] = {0};
    REQUIRE(gallery->created <= config->visitors && gallery->inside >= 0 &&
            gallery->reserved >= 0 &&
            gallery->inside + gallery->reserved <= config->capacity);
    for (int i = 0; i < gallery->created; i++) {
        const Visitor *visitor = &gallery->visitors[i];
        REQUIRE((visitor->seen & ~complete_mask(gallery)) == 0);
        if (visitor->current_painting >= 0) {
            int p = visitor->current_painting;
            REQUIRE(p < config->paintings);
            REQUIRE(visitor->state == INSIDE && visitor->waiting_painting == -1);
            REQUIRE((visitor->seen & (UINT64_C(1) << p)) == 0);
            at_painting[p]++;
        }
        if (visitor->waiting_painting >= 0) {
            REQUIRE(visitor->state == INSIDE && visitor->current_painting == -1);
            REQUIRE(visitor->waiting_painting < config->paintings);
        }
    }
    for (int p = 0; p < config->paintings; p++) {
        const Painting *painting = &gallery->paintings[p];
        REQUIRE(painting->viewers == at_painting[p]);
        REQUIRE(painting->viewers >= 0 && painting->viewers <= config->picture_capacity);
    }
}

static void signal_handler(int signal_number) {
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        pending_signal = signal_number;
    }
}

static int install_signal_handlers(Gallery *gallery) {
    struct sigaction action = {0};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    pending_signal = 0;
    if (sigaction(SIGINT, &action, &gallery->old_sigint) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &action, &gallery->old_sigterm) != 0) {
        return -1;
    }
    struct sigaction ignore_pipe = {0};
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, NULL) != 0) {
        return -1;
    }
    gallery->signal_handlers_installed = true;
    return 0;
}

static void restore_signal_handlers(Gallery *gallery) {
    if (!gallery->signal_handlers_installed) {
        return;
    }
    (void)sigaction(SIGINT, &gallery->old_sigint, NULL);
    (void)sigaction(SIGTERM, &gallery->old_sigterm, NULL);
    gallery->signal_handlers_installed = false;
}

/* все изменения модели и записи журнала происходят в одном потоке */
static void event(Gallery *gallery, bool always, const char *name,
                  const Visitor *visitor, int painting, const char *format, ...) {
    audit(gallery);
    char details[2048];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(details, sizeof(details), format, arguments);
    va_end(arguments);
    REQUIRE(length >= 0 && (size_t)length < sizeof(details));

    if (!logger_event(&gallery->logger, always, name, visitor == NULL ? 0 : visitor->id, painting + 1,
                      gallery->inside, gallery->reserved, gallery->queue_count,
                      painting < 0 ? 0 : gallery->paintings[painting].viewers,
                      details)) {
        gallery->failed = true;
        if (!gallery->stopping) {
            gallery->stopping = true;
            gallery->reason = "io_error";
        }
    }
}

static int pop_entry(Gallery *gallery) {
    REQUIRE(gallery->queue_count > 0);
    int index = gallery->entry_queue[gallery->queue_head];
    gallery->queue_head = (gallery->queue_head + 1) % gallery->config->visitors;
    gallery->queue_count--;
    REQUIRE(gallery->visitors[index].state == WAITING);
    return index;
}

static int pop_picture(Painting *painting) {
    REQUIRE(painting->waiting_count > 0);
    int index = painting->waiting[0];
    memmove(&painting->waiting[0], &painting->waiting[1],
            (size_t)(painting->waiting_count - 1) * sizeof(painting->waiting[0]));
    painting->waiting_count--;
    return index;
}

static void leave_gallery(Gallery *gallery, Visitor *visitor) {
    REQUIRE(visitor->state == INSIDE);
    REQUIRE(visitor->current_painting == -1);
    REQUIRE(visitor->waiting_painting == -1);
    bool complete = visitor->seen == complete_mask(gallery);
    visitor->state = complete ? FINISHED : INTERRUPTED;
    gallery->inside--;
    if (complete) {
        gallery->completed++;
    } else {
        gallery->interrupted++;
    }
    event(gallery, false, "EXIT", visitor, -1,
          "Посетитель вышел и освободил место. completed=%d", complete ? 1 : 0);
}

static void start_picture_waiters(Gallery *gallery, int painting_index) {
    Painting *painting = &gallery->paintings[painting_index];
    while (!gallery->stopping && painting->viewers < gallery->config->picture_capacity &&
           painting->waiting_count > 0) {
        int index = pop_picture(painting);
        Visitor *visitor = &gallery->visitors[index];
        REQUIRE(visitor->state == INSIDE);
        REQUIRE(visitor->waiting_painting == painting_index);
        visitor->waiting_painting = -1;
        ++painting->viewers;
        visitor->current_painting = painting_index;
        visitor->view_end_ms = gallery->now_ms + (uint64_t)visitor->duration_ms[painting_index];
        if (painting->viewers > painting->peak) {
            painting->peak = painting->viewers;
        }
        int active = 0;
        for (int p = 0; p < gallery->config->paintings; ++p) {
            active += gallery->paintings[p].viewers;
        }
        if (active > gallery->peak_viewers) {
            gallery->peak_viewers = active;
        }
        event(gallery, false, "VIEW_START", visitor, painting_index,
              "Начало осмотра. view_ms=%d", visitor->duration_ms[painting_index]);
    }
}

static void choose_next_picture(Gallery *gallery, Visitor *visitor) {
    REQUIRE(visitor->state == INSIDE);
    REQUIRE(visitor->current_painting == -1 && visitor->waiting_painting == -1);
    REQUIRE(visitor->next_step < gallery->config->paintings);

    int painting_index = visitor->order[visitor->next_step++];
    Painting *painting = &gallery->paintings[painting_index];
    REQUIRE((visitor->seen & (UINT64_C(1) << painting_index)) == 0);
    int ticket = painting->waiting_count;
    visitor->waiting_painting = painting_index;
    REQUIRE(painting->waiting_count < MAX_VISITORS);
    painting->waiting[painting->waiting_count++] = (int)(visitor - gallery->visitors);
    event(gallery, false, "CHOOSE", visitor, painting_index,
          "Выбрана картина. step=%d ticket=%d view_ms=%d",
          visitor->next_step, ticket, visitor->duration_ms[painting_index]);

    bool must_wait = painting->viewers >= gallery->config->picture_capacity;
    if (must_wait) {
        gallery->painting_waits++;
        event(gallery, false, "WAIT_PAINTING", visitor, painting_index,
              "Ожидание свободного места у картины. ticket=%d", ticket);
    }
    start_picture_waiters(gallery, painting_index);
}

static void enter_gallery(Gallery *gallery, Visitor *visitor) {
    REQUIRE(visitor->state == RESERVED);
    gallery->reserved--;
    gallery->inside++;
    visitor->state = INSIDE;
    if (gallery->inside > gallery->peak_inside) {
        gallery->peak_inside = gallery->inside;
    }
    event(gallery, false, "ENTER", visitor, -1, "Посетитель вошёл в галерею.");
    if (!gallery->stopping) {
        choose_next_picture(gallery, visitor);
    }
}

static void admit_waiting_visitors(Gallery *gallery) {
    while (!gallery->stopping && gallery->queue_count > 0 &&
           gallery->inside + gallery->reserved < gallery->config->capacity) {
        Visitor *visitor = &gallery->visitors[pop_entry(gallery)];
        visitor->state = RESERVED;
        gallery->reserved++;
        int occupied = gallery->inside + gallery->reserved;
        if (occupied > gallery->peak_occupied) {
            gallery->peak_occupied = occupied;
        }
        event(gallery, false, "ALLOW", visitor, -1,
              "Вахтёр разрешил вход; место зарезервировано.");
        if (!gallery->stopping) {
            enter_gallery(gallery, visitor);
        }
    }
}

static void process_arrival(Gallery *gallery, int index, int arrival_delay) {
    Visitor *visitor = &gallery->visitors[index];
    visitor->id = index + 1;
    visitor->next_step = 0;
    visitor->current_painting = -1;
    visitor->waiting_painting = -1;
    visitor->seen = 0;
    uint64_t state = (uint64_t)gallery->config->seed ^
                     (UINT64_C(0xd1b54a32d192ed03) * (uint64_t)visitor->id);
    for (int p = 0; p < gallery->config->paintings; ++p) {
        visitor->order[p] = p;
        visitor->duration_ms[p] = random_between(&state, gallery->config->view_min_ms,
                                                  gallery->config->view_max_ms);
    }
    if (gallery->config->strategy == STRATEGY_RANDOM) {
        for (int p = gallery->config->paintings - 1; p > 0; --p) {
            int other = random_between(&state, 0, p);
            int value = visitor->order[p];
            visitor->order[p] = visitor->order[other];
            visitor->order[other] = value;
        }
    }
    visitor->state = ARRIVED;
    gallery->created++;
    event(gallery, false, "ARRIVE", visitor, -1,
          "Прибыл посетитель. arrival_delay_ms=%d", arrival_delay);
    if (gallery->stopping) {
        return;
    }

    visitor->state = WAITING;
    int tail = (gallery->queue_head + gallery->queue_count) % gallery->config->visitors;
    gallery->entry_queue[tail] = index;
    ++gallery->queue_count;
    if (gallery->queue_count > gallery->peak_queue) {
        gallery->peak_queue = gallery->queue_count;
    }
    bool full = gallery->inside + gallery->reserved >= gallery->config->capacity;
    gallery->entry_waits += full;
    event(gallery, false, "WAIT_ENTRY", visitor, -1,
          "Ожидание разрешения вахтёра в FIFO-очереди. full=%d", full ? 1 : 0);
    admit_waiting_visitors(gallery);
}

static int find_next_view(const Gallery *gallery, uint64_t *end_time) {
    int selected = -1;
    uint64_t selected_time = UINT64_MAX;
    for (int i = 0; i < gallery->created; ++i) {
        const Visitor *visitor = &gallery->visitors[i];
        if (visitor->state == INSIDE && visitor->current_painting >= 0 &&
            (selected < 0 || visitor->view_end_ms < selected_time ||
             (visitor->view_end_ms == selected_time && visitor->id <
              gallery->visitors[selected].id))) {
            selected = i;
            selected_time = visitor->view_end_ms;
        }
    }
    if (selected >= 0 && end_time != NULL) {
        *end_time = selected_time;
    }
    return selected;
}

static void process_view_end(Gallery *gallery, int index) {
    Visitor *visitor = &gallery->visitors[index];
    int painting_index = visitor->current_painting;
    REQUIRE(visitor->state == INSIDE && painting_index >= 0);
    Painting *painting = &gallery->paintings[painting_index];

    visitor->current_painting = -1;
    painting->viewers--;
    visitor->seen |= UINT64_C(1) << painting_index;
    painting->completed_views++;
    gallery->total_views++;
    event(gallery, false, "VIEW_END", visitor, painting_index,
          "Осмотр завершён; картина засчитана один раз.");
    if (gallery->stopping) {
        return;
    }

    /* освободившееся место получает самый ранний билет этой картины */
    start_picture_waiters(gallery, painting_index);
    if (visitor->next_step >= gallery->config->paintings) {
        leave_gallery(gallery, visitor);
        admit_waiting_visitors(gallery);
    } else {
        choose_next_picture(gallery, visitor);
    }
}

static void request_stop(Gallery *gallery, const char *reason, int signal_number) {
    if (gallery->stopping) {
        return;
    }
    gallery->stopping = true;
    gallery->reason = reason;
    gallery->stop_signal = signal_number;
    event(gallery, true, "STOP", NULL, -1,
          "Остановка: reason=%s signal=%d. Последовательная модель завершается.", reason, signal_number);
}

static void cancel_all(Gallery *gallery) {
    while (gallery->queue_count > 0) {
        Visitor *visitor = &gallery->visitors[pop_entry(gallery)];
        visitor->state = CANCELLED;
        ++gallery->cancelled;
        event(gallery, false, "CANCEL", visitor, -1,
              "Посетитель покидает очередь: галерея прекращает работу.");
    }

    for (int i = 0; i < gallery->created; i++) {
        Visitor *visitor = &gallery->visitors[i];
        if (visitor->state == RESERVED) {
            gallery->reserved--;
            visitor->state = CANCELLED;
            gallery->cancelled++;
            event(gallery, false, "CANCEL", visitor, -1,
                  "Посетитель уходит без входа; резерв места освобождён.");
            continue;
        }
        if (visitor->state != INSIDE) {
            continue;
        }
        if (visitor->current_painting >= 0) {
            int painting_index = visitor->current_painting;
            visitor->current_painting = -1;
            --gallery->paintings[painting_index].viewers;
            event(gallery, false, "VIEW_ABORT", visitor, painting_index,
                  "Осмотр прерван; место освобождено, картина не засчитана.");
        }
        if (visitor->waiting_painting >= 0) {
            visitor->waiting_painting = -1;
        }
        leave_gallery(gallery, visitor);
    }
    for (int p = 0; p < gallery->config->paintings; p++) {
        gallery->paintings[p].waiting_count = 0;
    }
}

static bool advance_time(Gallery *gallery, uint64_t target) {
    while (gallery->now_ms < target) {
        if (pending_signal != 0) {
            return false;
        }
        uint64_t delta = target - gallery->now_ms;
        struct timespec request = {
            .tv_sec = (time_t)(delta / 1000),
            .tv_nsec = (long)((delta % 1000) * 1000000U)
        };
        if (nanosleep(&request, NULL) == 0) {
            gallery->now_ms = target;
            return true;
        }
        if (errno == EINTR) {
            if (pending_signal != 0) {
                return false;
            }
            continue;
        }
        gallery->failed = true;
        request_stop(gallery, "clock_error", 0);
        return false;
    }
    return true;
}

static void handle_pending_signal(Gallery *gallery) {
    int signal_number = pending_signal;
    pending_signal = 0;
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        request_stop(gallery, signal_number == SIGINT ? "sigint" : "sigterm",
                     signal_number);
    }
}

int simulation_run(const Config *config) {
    Gallery gallery = {.config = config, .reason = "complete"};
    size_t allocation = (size_t)(config->visitors > 0 ? config->visitors : 1);
    gallery.visitors = calloc(allocation, sizeof(*gallery.visitors));
    gallery.entry_queue = calloc(allocation, sizeof(*gallery.entry_queue));
    if (gallery.visitors == NULL || gallery.entry_queue == NULL) {
        dprintf(STDERR_FILENO, "Недостаточно памяти для модели.\n");
        free(gallery.visitors);
        free(gallery.entry_queue);
        return 1;
    }
    if (logger_open(&gallery.logger, config->log_path, config->config_path,
                    config->quiet) != 0) {
        dprintf(STDERR_FILENO, "Не удалось открыть журнал: %s\n", strerror(errno));
        free(gallery.visitors);
        free(gallery.entry_queue);
        return 1;
    }
    if (install_signal_handlers(&gallery) != 0) {
        dprintf(STDERR_FILENO, "Не удалось установить обработчики сигналов: %s\n",
                strerror(errno));
        (void)logger_close(&gallery.logger);
        free(gallery.visitors);
        free(gallery.entry_queue);
        return 1;
    }

    event(&gallery, true, "CONFIG", NULL, -1,
          "Параметры: capacity=%d paintings=%d picture_capacity=%d visitors=%d "
          "arrival_min_ms=%d arrival_max_ms=%d view_min_ms=%d view_max_ms=%d "
          "strategy=%s seed=%" PRIu32,
          config->capacity, config->paintings, config->picture_capacity, config->visitors,
          config->arrival_min_ms, config->arrival_max_ms,
          config->view_min_ms, config->view_max_ms, strategy_name(config->strategy), config->seed);

    uint64_t arrival_state = (uint64_t)config->seed ^ UINT64_C(0xa0761d6478bd642f);
    uint64_t next_arrival_ms = 0;
    int scheduled_arrival_delay = 0;
    bool arrival_scheduled = false;
    int next_index = 0;

    while (!gallery.stopping && (next_index < config->visitors || find_next_view(&gallery, NULL) >= 0)) {
        if (pending_signal != 0) {
            handle_pending_signal(&gallery);
            break;
        }

        if (next_index < config->visitors && !arrival_scheduled) {
            scheduled_arrival_delay = random_between(&arrival_state, config->arrival_min_ms, config->arrival_max_ms);
            next_arrival_ms += (uint64_t)scheduled_arrival_delay;
            arrival_scheduled = true;
        }

        uint64_t next_view_ms = UINT64_MAX;
        int next_view = find_next_view(&gallery, &next_view_ms);
        bool arrival_is_next = arrival_scheduled &&
                               (next_view < 0 || next_arrival_ms <= next_view_ms);
        if (arrival_is_next) {
            if (!advance_time(&gallery, next_arrival_ms)) {
                handle_pending_signal(&gallery);
                break;
            }
            if (pending_signal != 0) {
                handle_pending_signal(&gallery);
                break;
            }
            process_arrival(&gallery, next_index, scheduled_arrival_delay);
            ++next_index;
            arrival_scheduled = false;
            if (next_index == config->visitors && !gallery.stopping) {
                event(&gallery, false, "ARRIVALS_DONE", NULL, -1,
                      "Создание посетителей завершено. created=%d requested=%d",
                      gallery.created, config->visitors);
                admit_waiting_visitors(&gallery);
            }
        } else if (next_view >= 0) {
            if (!advance_time(&gallery, next_view_ms)) {
                handle_pending_signal(&gallery);
                break;
            }
            if (pending_signal != 0) {
                handle_pending_signal(&gallery);
                break;
            }
            process_view_end(&gallery, next_view);
        }
    }

    if (!gallery.stopping && config->visitors == 0) {
        event(&gallery, false, "ARRIVALS_DONE", NULL, -1,
              "Создание посетителей завершено. created=%d requested=%d",
              gallery.created, config->visitors);
    }
    if (pending_signal != 0 && !gallery.stopping) {
        handle_pending_signal(&gallery);
    }
    if (gallery.stopping) {
        cancel_all(&gallery);
    } else {
        admit_waiting_visitors(&gallery);
    }

    REQUIRE(gallery.inside == 0 && gallery.reserved == 0 && gallery.queue_count == 0);
    REQUIRE(gallery.completed + gallery.interrupted + gallery.cancelled == gallery.created);
    if (!gallery.stopping) {
        REQUIRE(gallery.completed == config->visitors);
        REQUIRE(gallery.total_views == config->visitors * config->paintings);
    }
    event(&gallery, true, "ENTRY_CLOSED", NULL, -1,
          "Вход закрыт. Новые посетители больше не принимаются.");
    event(&gallery, false, "GUARD_DONE", NULL, -1,
          "Последовательный диспетчер галереи завершил работу: галерея пуста.");

    event(&gallery, true, "SUMMARY", NULL, -1,
          "Итоги: reason=%s requested=%d created=%d arrived=%d completed=%d "
          "interrupted=%d cancelled=%d unstarted=%d views=%d "
          "peak_inside=%d peak_occupied=%d peak_viewers=%d peak_queue=%d "
          "entry_waits=%d painting_waits=%d",
          gallery.reason, config->visitors, gallery.created, gallery.created, gallery.completed,
          gallery.interrupted, gallery.cancelled, config->visitors - gallery.created,
          gallery.total_views, gallery.peak_inside, gallery.peak_occupied,
          gallery.peak_viewers, gallery.peak_queue, gallery.entry_waits, gallery.painting_waits);
    for (int p = 0; p < config->paintings; p++) {
        event(&gallery, true, "PICTURE_SUMMARY", NULL, p,
              "Итоги по картине: completed_views=%d peak=%d limit=%d",
              gallery.paintings[p].completed_views, gallery.paintings[p].peak,
              config->picture_capacity);
    }

    int exit_code = gallery.failed ? 1 :
                    (gallery.stop_signal != 0 ? 128 + gallery.stop_signal : 0);
    if (logger_close(&gallery.logger) != 0) {
        dprintf(STDERR_FILENO, "Ошибка закрытия журнала: %s\n", strerror(errno));
        exit_code = 1;
    }
    restore_signal_handlers(&gallery);
    free(gallery.entry_queue);
    free(gallery.visitors);
    return exit_code;
}
