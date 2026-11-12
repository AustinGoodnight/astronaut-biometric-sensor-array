#ifndef BLINKY_PERSP_H
#define BLINKY_PERSP_H

#include <stdint.h>

int persp_init(void);
int persp_read_resistance_kohms(uint16_t *resistance_kohms);

#endif
