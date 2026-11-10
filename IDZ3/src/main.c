#include "config.h"
#include "simulation.h"

int main(int argc, char **argv) {
    Config config;
    int result = config_parse(&config, argc, argv);
    if (result != 0) {
        return result > 0 ? 0 : 2;
    }
    return simulation_run(&config);
}
