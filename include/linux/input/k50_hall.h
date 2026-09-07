/*
 * Narrow built-in Hall/FT8057 contract for k50sv1_64_bsp.
 *
 * No symbol declared here is exported to loadable modules.
 */
#ifndef _LINUX_INPUT_K50_HALL_H
#define _LINUX_INPUT_K50_HALL_H

#include <linux/types.h>

int get_hall_status(void);
int k50_ft8057_set_hall_state(bool state);

#endif /* _LINUX_INPUT_K50_HALL_H */
