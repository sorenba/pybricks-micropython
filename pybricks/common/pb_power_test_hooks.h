#ifndef _PYBRICKS_COMMON_PB_POWER_TEST_HOOKS_H_
#define _PYBRICKS_COMMON_PB_POWER_TEST_HOOKS_H_

#include <stdbool.h>
#include <stdint.h>

void pb_power_test_idle_enter(void);
void pb_power_test_idle_exit(void);
void pb_power_test_log_event(uint16_t event, uint32_t data);
void pb_power_test_supervisor_prepare_sleep(void);
bool pb_power_test_boot_autostart_check(void);
void pb_power_test_boot_autostart_request(void);
void pb_power_test_boot_autostart_clear(void);
void pb_power_test_boot_autostart_confirm(void);
void pb_power_test_boot_autostart_failed(void);
bool pb_power_test_supervisor_sleep_requested(void);
void pb_power_test_supervisor_sleep(void);

#endif
