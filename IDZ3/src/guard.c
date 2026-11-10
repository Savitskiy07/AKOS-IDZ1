#include "gallery.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage("guard shm|fifo КАТАЛОГ");
        return 0;
    }
    if (argc < 3 || (strcmp(argv[1], "shm") && strcmp(argv[1], "fifo"))) {
        usage("guard shm|fifo КАТАЛОГ");
        return 2;
    }

    Mode mode = !strcmp(argv[1], "shm") ? NAMED : FIFO;
    Config cfg;
    if (config_read(&cfg, argc - 3, argv + 3)) {
        return 2;
    }
    if (install_signals()) {
        perror("sigaction");
        return 1;
    }

    Ipc ipc;
    ipc_init(&ipc);
    int rc = ipc_create(&ipc, mode, argv[2], cfg.visitors);
    if (rc) {
        perror("создание сеанса");
    } else {
        rc = guard_run(&ipc, &cfg);
        if (rc && !stopped) {
            perror("вахтёр");
        }
    }
    ipc_close(&ipc);
    return stopped ? 128 + stopped : (rc ? 1 : 0);
}
