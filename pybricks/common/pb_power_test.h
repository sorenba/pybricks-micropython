#ifndef _PYBRICKS_COMMON_PB_POWER_TEST_H_
#define _PYBRICKS_COMMON_PB_POWER_TEST_H_

#include "py/obj.h"

void pb_power_test_idle_enter(void);
void pb_power_test_idle_exit(void);
mp_obj_t pb_power_test_active(void);
mp_obj_t pb_power_test_standby(void);

#endif
