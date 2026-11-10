#ifndef GALLERY_SIMULATION_H
#define GALLERY_SIMULATION_H

#include "config.h"

/* 0: успех; 1: ошибка выполнения; 130/143: SIGINT/SIGTERM */
int simulation_run(const Config *config);

#endif
