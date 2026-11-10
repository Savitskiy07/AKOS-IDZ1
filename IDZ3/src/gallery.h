#ifndef GALLERY_H
#define GALLERY_H

#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>
#include <semaphore.h>
#include <limits.h>

enum {
    MAX_VISITORS = 300,
    MAX_PAINTINGS = 64,
    EMPTY = 0,
    FULL = 1,
    FIRST_GATE = 2
};

typedef enum { UNNAMED, NAMED, FIFO } Mode;
typedef enum { JOIN, ARRIVE, ENTER, SELECT_PICTURE, FINISH, LEAVE } Action;

typedef struct {
    int capacity, paintings, visitors;
    int limits[MAX_PAINTINGS];
    int arrival_min, arrival_max, view_min, view_max;
    bool random_order;
    uint32_t seed;
    const char *log_path;
} Config;

typedef struct {
    int64_t epoch;
    int paintings, arrival_min, arrival_max, view_min, view_max, random_order;
    uint32_t seed;
} Plan;

typedef struct {
    int id, action, picture;
} Request;

typedef struct {
    Request slot;
    Plan plan;
    sem_t sems[FIRST_GATE + MAX_VISITORS];
} Shared;

typedef struct {
    Mode mode;
    pid_t owner;
    int count, dirfd, readyfd, requests, plans, created_sems;
    bool owns_dir, owns_shm;
    char path[PATH_MAX];
    Shared *mem;
    sem_t *sems[FIRST_GATE + MAX_VISITORS];
} Ipc;

extern volatile sig_atomic_t stopped;

int install_signals(void);
int64_t now_ms(void);
int write_all(int fd, const void *buf, size_t len);
int number(const char *s, int min, int max, int *out);
void usage(const char *name);
int config_read(Config *cfg, int argc, char **argv);
uint32_t random_next(uint32_t *s);
int sample(uint32_t *s, int lo, int hi);

void ipc_init(Ipc *ipc);
int ipc_create(Ipc *ipc, Mode mode, const char *path, int count);
int ipc_connect(Ipc *ipc, const char *path, int id);
int ipc_alive(const Ipc *ipc);
void ipc_cancel_owner(const Ipc *ipc);
void ipc_close(Ipc *ipc);
int ipc_send(Ipc *ipc, Request req);
int ipc_receive(Ipc *ipc, Request *req);
int ipc_give_plan(Ipc *ipc, const Plan *plan, int id);
int ipc_get_plan(Ipc *ipc, Plan *plan, int id);
int ipc_grant(Ipc *ipc, int id);
int ipc_await(Ipc *ipc, int id);
int sleep_until(Ipc *ipc, int64_t deadline);

int guard_run(Ipc *ipc, const Config *cfg);
int visitor_run(Ipc *ipc, int id);
#endif
