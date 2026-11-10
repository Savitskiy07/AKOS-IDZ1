#include "gallery.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int id;
    if (argc != 3 || number(argv[2], 1, MAX_VISITORS, &id)) {
        puts("Запуск: visitor КАТАЛОГ НОМЕР (от 1 до числа посетителей)");
        return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
    }
    if (install_signals()) {
        perror("sigaction");
        return 1;
    }

    Ipc ipc;
    ipc_init(&ipc);
    int rc = ipc_connect(&ipc, argv[1], id);
    if (rc) {
        perror("подключение посетителя");
    } else {
        rc = visitor_run(&ipc, id);
        if (rc) {
            if (!stopped && errno != ECANCELED) {
                perror("посетитель");
            }
            ipc_cancel_owner(&ipc);
        }
    }
    ipc_close(&ipc);
    return stopped ? 128 + stopped : (rc ? 1 : 0);
}
