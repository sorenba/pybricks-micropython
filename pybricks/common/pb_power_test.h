#ifndef _PYBRICKS_COMMON_PB_POWER_TEST_H_
#define _PYBRICKS_COMMON_PB_POWER_TEST_H_

#include <stdint.h>

#include "py/obj.h"

mp_obj_t pb_power_test_active(void);
mp_obj_t pb_power_test_standby(uint32_t cycles);
mp_obj_t pb_power_test_standby_result(void);
mp_obj_t pb_power_test_log(void);

#endif
