#include "gallery.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int run_day(Ipc *ipc, const Config *cfg) {
    pid_t children[MAX_VISITORS + 1] = {0};
    int running = 0, result = 0;

    for (int id = 0; id <= cfg->visitors && !stopped; id++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            result = 1;
            break;
        }
        if (!pid) {
            int rc = id ? visitor_run(ipc, id) : guard_run(ipc, cfg);
            if (rc && !stopped && errno != ECANCELED) {
                perror(id ? "посетитель" : "вахтёр");
            }
            _exit(rc ? 1 : 0);
        }
        children[id] = pid;
        running++;
    }

    bool cancelling = false;
    while (running) {
        if ((stopped || result) && !cancelling) {
            cancelling = true;
            for (int i = 0; i <= cfg->visitors; i++) {
                if (children[i]) {
                    kill(children[i], SIGTERM);
                }
            }
        }

        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            for (int i = 0; i <= cfg->visitors; i++) {
                if (children[i] == pid) {
                    children[i] = 0;
                    break;
                }
            }
            running--;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                result = 1;
            }
        } else if (!pid) {
            struct timespec tick = {.tv_nsec = 10000000};
            nanosleep(&tick, NULL);
        } else if (errno != EINTR) {
            perror("waitpid");
            result = 1;
            break;
        }
    }
    return stopped ? 128 + stopped : result;
}

int main(int argc, char **argv) {
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage(name);
        return 0;
    }

    Config cfg;
    if (config_read(&cfg, argc - 1, argv + 1)) {
        usage(name);
        return 2;
    }
    if (install_signals()) {
        perror("sigaction");
        return 1;
    }

    Ipc ipc;
    ipc_init(&ipc);
    int rc = 1;
    if (ipc_create(&ipc, DAY_MODE, NULL, cfg.visitors)) {
        perror("создание сеанса");
    } else {
        rc = run_day(&ipc, &cfg);
    }
    ipc_close(&ipc);
    return stopped ? 128 + stopped : rc;
}
