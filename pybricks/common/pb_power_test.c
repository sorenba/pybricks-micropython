// SPDX-License-Identifier: MIT

#include "py/mpconfig.h"

#if PYBRICKS_PY_COMMON && PYBRICKS_PY_COMMON_SYSTEM

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include <pbdrv/adc.h>
#include <pbdrv/battery.h>
#include <pbdrv/clock.h>
#include <pbdrv/watchdog.h>
#include <pbsys/status.h>
#include <pbsys/light.h>
#include <pbsys/program_stop.h>
#include <pbsys/storage.h>
#include "../../lib/pbio/sys/program_stop.h"
#include "../../lib/pbio/sys/light.h"

#include <pbio/color.h>
#include <pbio/light.h>

#include "stm32l4xx.h"
#include STM32_HAL_H

#include "pb_power_test.h"

typedef struct {
    uint64_t sum;
    uint32_t count;
} pb_power_test_samples_t;

#define PB_POWER_TEST_SETTLE_MS 500U
#define PB_POWER_TEST_SAMPLE_MS 500U
#define PB_POWER_TEST_ACTIVE_MS 10000U
#define PB_POWER_TEST_ACTIVE_SAMPLE_MS 10U
#define PB_POWER_TEST_STANDBY_SECONDS 5U
#define PB_POWER_TEST_UPPER_BOUND_SAMPLES 5U
#define PB_POWER_TEST_UPPER_BOUND_CPU_CYCLES 400000U


#define PB_POWER_TEST_LOG_OFFSET 0U
#define PB_POWER_TEST_LOG_SIZE 120U
#define PB_POWER_TEST_LOG_CAPACITY 24U

typedef struct __attribute__((packed)) {
    uint8_t event;
    uint32_t data;
} pb_power_test_log_record_t;

static void pb_power_test_log_clear(void) {
    uint8_t clear[PB_POWER_TEST_LOG_SIZE] = { 0 };
    pbsys_storage_set_user_data(PB_POWER_TEST_LOG_OFFSET, clear, sizeof(clear));
}

void pb_power_test_log_event(uint16_t event, uint32_t data) {
    uint8_t *bytes;
    if (pbsys_storage_get_user_data(PB_POWER_TEST_LOG_OFFSET, &bytes, PB_POWER_TEST_LOG_SIZE) != PBIO_SUCCESS) {
        return;
    }

    const pb_power_test_log_record_t *records = (const pb_power_test_log_record_t *)bytes;
    uint32_t index = 0;
    while (index < PB_POWER_TEST_LOG_CAPACITY && records[index].event != 0) {
        index++;
    }
    if (index == PB_POWER_TEST_LOG_CAPACITY) {
        return;
    }

    pb_power_test_log_record_t record = {
        .event = event,
        .data = data,
    };
    pbsys_storage_set_user_data(PB_POWER_TEST_LOG_OFFSET + index * sizeof(record), (const uint8_t *)&record, sizeof(record));
}

mp_obj_t pb_power_test_log(void) {
    uint8_t *bytes;
    mp_obj_t list = mp_obj_new_list(0, NULL);
    if (pbsys_storage_get_user_data(PB_POWER_TEST_LOG_OFFSET, &bytes, PB_POWER_TEST_LOG_SIZE) != PBIO_SUCCESS) {
        return list;
    }

    const pb_power_test_log_record_t *records = (const pb_power_test_log_record_t *)bytes;
    for (uint32_t i = 0; i < PB_POWER_TEST_LOG_CAPACITY; i++) {
        if (records[i].event == 0) {
            break;
        }
        char data_hex[9];
        snprintf(data_hex, sizeof(data_hex), "%08lX", (unsigned long)records[i].data);
        mp_obj_t items[] = {
            mp_obj_new_int_from_uint(i + 1),
            mp_obj_new_int_from_uint(records[i].event),
            mp_obj_new_str(data_hex, 8),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(MP_ARRAY_SIZE(items), items));
    }
    return list;
}

static void pb_power_test_samples_reset(pb_power_test_samples_t *samples) {
    samples->sum = 0;
    samples->count = 0;
}

static void pb_power_test_samples_add(pb_power_test_samples_t *samples, uint16_t value) {
    samples->sum += value;
    samples->count++;
}

static uint32_t pb_power_test_samples_mean(const pb_power_test_samples_t *samples) {
    return samples->count ? samples->sum / samples->count : 0;
}

static void pb_power_test_wait_ms(uint32_t duration_ms) {
    uint32_t end = pbdrv_clock_get_ms() + duration_ms;
    while ((int32_t)(pbdrv_clock_get_ms() - end) < 0) {
        mp_event_wait_indefinite();
    }
}

static void pb_power_test_measure(uint32_t duration_ms, uint32_t interval_ms, pb_power_test_samples_t *voltage, pb_power_test_samples_t *current) {
    pb_power_test_samples_reset(voltage);
    pb_power_test_samples_reset(current);

    uint32_t start = pbdrv_clock_get_ms();
    uint32_t next = start;
    while (pbdrv_clock_get_ms() - start < duration_ms) {
        uint32_t now = pbdrv_clock_get_ms();
        if ((int32_t)(now - next) >= 0) {
            uint16_t voltage_raw;
            uint16_t current_raw;
            if (pbdrv_adc_get_ch(PBDRV_CONFIG_BATTERY_ADC_VOLTAGE_CH, &voltage_raw) == PBIO_SUCCESS &&
                pbdrv_adc_get_ch(PBDRV_CONFIG_BATTERY_ADC_CURRENT_CH, &current_raw) == PBIO_SUCCESS) {
                pb_power_test_samples_add(voltage, voltage_raw);
                pb_power_test_samples_add(current, current_raw);
            }
            next += interval_ms;
        }
        mp_event_wait_indefinite();
    }
}

static void pb_power_test_dict_store(mp_obj_t dict, qstr key, mp_obj_t value) {
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), value);
}

static uint32_t pb_power_test_current_ma(uint32_t current_raw) {
    return (current_raw + PBDRV_CONFIG_BATTERY_ADC_CURRENT_RAW_OFFSET) *
        PBDRV_CONFIG_BATTERY_ADC_CURRENT_SCALED_MAX /
        PBDRV_CONFIG_BATTERY_ADC_CURRENT_RAW_MAX;
}

static uint32_t pb_power_test_voltage_mv(uint32_t voltage_raw, uint32_t current_raw) {
    uint32_t current_ma = pb_power_test_current_ma(current_raw);
    return voltage_raw * PBDRV_CONFIG_BATTERY_ADC_VOLTAGE_SCALED_MAX /
        PBDRV_CONFIG_BATTERY_ADC_VOLTAGE_RAW_MAX + current_ma *
        PBDRV_CONFIG_BATTERY_ADC_CURRENT_CORRECTION / 16;
}

static mp_obj_t pb_power_test_sample_dict(const pb_power_test_samples_t *voltage, const pb_power_test_samples_t *current) {
    uint32_t voltage_raw_mean = pb_power_test_samples_mean(voltage);
    uint32_t current_raw_mean = pb_power_test_samples_mean(current);
    mp_obj_t dict = mp_obj_new_dict(0);
    pb_power_test_dict_store(dict, MP_QSTR_voltage_raw_mean, mp_obj_new_int_from_uint(voltage_raw_mean));
    pb_power_test_dict_store(dict, MP_QSTR_voltage_mv_mean, mp_obj_new_int_from_uint(pb_power_test_voltage_mv(voltage_raw_mean, current_raw_mean)));
    pb_power_test_dict_store(dict, MP_QSTR_current_raw_mean, mp_obj_new_int_from_uint(current_raw_mean));
    pb_power_test_dict_store(dict, MP_QSTR_current_ma_mean, mp_obj_new_int_from_uint(pb_power_test_current_ma(current_raw_mean)));
    pb_power_test_dict_store(dict, MP_QSTR_sample_count, mp_obj_new_int_from_uint(voltage->count));
    return dict;
}

void pb_power_test_idle_enter(void) {
}

void pb_power_test_idle_exit(void) {
}

mp_obj_t pb_power_test_active(void) {
    pb_power_test_samples_t voltage;
    pb_power_test_samples_t current;

    pb_power_test_wait_ms(PB_POWER_TEST_SETTLE_MS);
    uint32_t start_us = pbdrv_clock_get_us();
    pb_power_test_measure(PB_POWER_TEST_ACTIVE_MS, PB_POWER_TEST_ACTIVE_SAMPLE_MS, &voltage, &current);
    uint32_t elapsed_us = pbdrv_clock_get_us() - start_us;

    mp_obj_t report = mp_obj_new_dict(0);
    pb_power_test_dict_store(report, MP_QSTR_test, mp_obj_new_str("active_idle", 11));
    pb_power_test_dict_store(report, MP_QSTR_duration_us, mp_obj_new_int_from_uint(elapsed_us));
    pb_power_test_dict_store(report, MP_QSTR_electrical, pb_power_test_sample_dict(&voltage, &current));
    return report;
}

#define PB_POWER_TEST_AUTOSTART_MAGIC 0x5057414BU
#define PB_POWER_TEST_AUTOSTART_OFFSET (PBSYS_CONFIG_STORAGE_USER_DATA_SIZE - 8U)

typedef struct {
    uint32_t magic;
    uint32_t magic_inverse;
} pb_power_test_autostart_marker_t;

static bool pb_power_test_autostarted;
static bool pb_power_test_supervisor_sleep_pending;
static volatile bool pb_power_test_rtc_wake_armed;
static volatile bool pb_power_test_rtc_wake_detected;
static volatile uint32_t pb_power_test_rtc_wake_source;
static uint32_t pb_power_test_standby_cycles;
static uint32_t pb_power_test_completed_cycles;
static uint32_t pb_power_test_start_voltage_mv;
static uint32_t pb_power_test_start_current_ma;
static uint32_t pb_power_test_start_voltage_raw;
static uint32_t pb_power_test_start_current_raw;
static uint32_t pb_power_test_end_voltage_mv;
static uint32_t pb_power_test_end_current_ma;
static uint32_t pb_power_test_end_voltage_raw;
static uint32_t pb_power_test_end_current_raw;
static uint32_t pb_power_test_upper_voltage_mv;
static uint32_t pb_power_test_upper_current_ma;
static uint32_t pb_power_test_upper_voltage_raw;
static uint32_t pb_power_test_upper_current_raw;

static void pb_power_test_rtc_disable_wakeup(void) {
    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;
    RTC->CR &= ~(RTC_CR_WUTE | RTC_CR_WUTIE);
    while (!(RTC->ISR & RTC_ISR_WUTWF)) {
    }
    do {
        RTC->ISR &= ~RTC_ISR_WUTF;
        __DSB();
        __ISB();
    } while (RTC->ISR & RTC_ISR_WUTF);
    RTC->WPR = 0xFF;

    EXTI->IMR1 &= ~EXTI_IMR1_IM20;
    EXTI->RTSR1 &= ~EXTI_RTSR1_RT20;
    EXTI->PR1 = EXTI_PR1_PIF20;
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);
}

static void pb_power_test_rtc_arm_wakeup(void) {
    NVIC_DisableIRQ(RTC_WKUP_IRQn);
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);

    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;
    RTC->CR &= ~(RTC_CR_WUTE | RTC_CR_WUTIE);
    while (!(RTC->ISR & RTC_ISR_WUTWF)) {
    }
    do {
        RTC->ISR &= ~RTC_ISR_WUTF;
        __DSB();
        __ISB();
    } while (RTC->ISR & RTC_ISR_WUTF);
    RTC->WUTR = PB_POWER_TEST_STANDBY_SECONDS - 1U;
    RTC->CR = (RTC->CR & ~RTC_CR_WUCKSEL) | RTC_CR_WUCKSEL_2;
    RTC->CR |= RTC_CR_WUTIE | RTC_CR_WUTE;
    while (RTC->ISR & RTC_ISR_WUTWF) {
    }
    RTC->WPR = 0xFF;

    EXTI->EMR1 &= ~EXTI_EMR1_EM20;
    EXTI->RTSR1 |= EXTI_RTSR1_RT20;
    EXTI->PR1 = EXTI_PR1_PIF20;
    EXTI->IMR1 |= EXTI_IMR1_IM20;
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);
    NVIC_SetPriority(RTC_WKUP_IRQn, 0);
    pb_power_test_rtc_wake_detected = false;
    pb_power_test_rtc_wake_source = 1;
    pb_power_test_rtc_wake_armed = true;
    NVIC_EnableIRQ(RTC_WKUP_IRQn);
}

static void pb_power_test_wait_cpu_cycles(uint32_t cycles) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    while (DWT->CYCCNT < cycles) {
    }
}

static void pb_power_test_measure_upper_bound(void) {
    uint32_t voltage_sum = 0;
    uint32_t current_sum = 0;
    uint32_t count = 0;

    for (uint32_t i = 0; i < PB_POWER_TEST_UPPER_BOUND_SAMPLES; i++) {
        pb_power_test_wait_cpu_cycles(PB_POWER_TEST_UPPER_BOUND_CPU_CYCLES);
        uint16_t voltage_raw;
        uint16_t current_raw;
        if (pbdrv_adc_get_ch(PBDRV_CONFIG_BATTERY_ADC_VOLTAGE_CH, &voltage_raw) == PBIO_SUCCESS &&
            pbdrv_adc_get_ch(PBDRV_CONFIG_BATTERY_ADC_CURRENT_CH, &current_raw) == PBIO_SUCCESS) {
            voltage_sum += voltage_raw;
            current_sum += current_raw;
            count++;
        }
    }

    if (count) {
        pb_power_test_upper_voltage_raw = voltage_sum / count;
        pb_power_test_upper_current_raw = current_sum / count;
        pb_power_test_upper_current_ma = pb_power_test_current_ma(pb_power_test_upper_current_raw);
        pb_power_test_upper_voltage_mv = pb_power_test_voltage_mv(pb_power_test_upper_voltage_raw, pb_power_test_upper_current_raw);
        pb_power_test_log_event(32, (pb_power_test_upper_voltage_mv << 16) | pb_power_test_upper_current_ma);
        pb_power_test_log_event(35, (pb_power_test_upper_voltage_raw << 16) | pb_power_test_upper_current_raw);
    }
}

bool pb_power_test_boot_autostart_check(void) {
    pb_power_test_log_event(17, 0);
    uint8_t *data;
    if (pbsys_storage_get_user_data(PB_POWER_TEST_AUTOSTART_OFFSET, &data, sizeof(pb_power_test_autostart_marker_t)) != PBIO_SUCCESS) {
        pb_power_test_log_event(18, 1);
        return false;
    }

    pb_power_test_autostart_marker_t marker;
    memcpy(&marker, data, sizeof(marker));
    if (marker.magic != PB_POWER_TEST_AUTOSTART_MAGIC || marker.magic_inverse != ~PB_POWER_TEST_AUTOSTART_MAGIC) {
        pb_power_test_log_event(18, 2);
        return false;
    }

    pb_power_test_log_event(19, 0);
    pb_power_test_autostarted = true;
    return true;
}

void pb_power_test_boot_autostart_request(void) {
    pb_power_test_autostart_marker_t marker = {
        .magic = PB_POWER_TEST_AUTOSTART_MAGIC,
        .magic_inverse = ~PB_POWER_TEST_AUTOSTART_MAGIC,
    };
    pbsys_storage_set_user_data(PB_POWER_TEST_AUTOSTART_OFFSET, (const uint8_t *)&marker, sizeof(marker));
}

void pb_power_test_boot_autostart_clear(void) {
    pb_power_test_autostart_marker_t marker = { 0 };
    pbsys_storage_set_user_data(PB_POWER_TEST_AUTOSTART_OFFSET, (const uint8_t *)&marker, sizeof(marker));
}

static void pb_power_test_set_status_light(pbio_color_t color) {
    if (pbsys_status_light_main) {
        pbio_color_light_on(pbsys_status_light_main, color);
    }
}

void pb_power_test_boot_autostart_confirm(void) {
    pb_power_test_log_event(22, 0);
    pb_power_test_set_status_light(PBIO_COLOR_ORANGE);
}

void pb_power_test_boot_autostart_confirm_silent(void) {
    pb_power_test_log_event(22, 0);
}

void pb_power_test_boot_autostart_failed(void) {
    pb_power_test_log_event(23, 0);
    pb_power_test_set_status_light(PBIO_COLOR_RED);
}

bool pb_power_test_supervisor_sleep_requested(void) {
    return pb_power_test_supervisor_sleep_pending;
}

void RTC_WKUP_IRQHandler(void) {
    bool wake_timer_elapsed = pb_power_test_rtc_wake_armed && (RTC->ISR & RTC_ISR_WUTF);

    pb_power_test_rtc_disable_wakeup();

    if (wake_timer_elapsed) {
        pb_power_test_rtc_wake_armed = false;
        pb_power_test_rtc_wake_source = 0;
        pb_power_test_rtc_wake_detected = true;
    }
}

void pb_power_test_supervisor_prepare_sleep(void) {
    pb_power_test_supervisor_sleep_pending = false;

    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;
    PWR->CR1 |= PWR_CR1_DBP;
    while (!(PWR->CR1 & PWR_CR1_DBP)) {
    }

    RCC->CSR |= RCC_CSR_LSION;
    while (!(RCC->CSR & RCC_CSR_LSIRDY)) {
    }

    // Start every test from a newly reset RTC backup domain. The RTC and its
    // wake timer survive a system reset, so reusing the previous run's state
    // made repeated Stop 2 tests intermittently fail.
    RCC->BDCR |= RCC_BDCR_BDRST;
    RCC->BDCR &= ~RCC_BDCR_BDRST;
    RCC->BDCR = (RCC->BDCR & ~RCC_BDCR_RTCSEL) | RCC_BDCR_RTCSEL_1;
    RCC->BDCR |= RCC_BDCR_RTCEN;

    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;
    RTC->ISR |= RTC_ISR_INIT;
    while (!(RTC->ISR & RTC_ISR_INITF)) {
    }
    RTC->PRER = (127U << RTC_PRER_PREDIV_A_Pos) | (249U << RTC_PRER_PREDIV_S_Pos);
    RTC->ISR &= ~RTC_ISR_INIT;
    RTC->WPR = 0xFF;

    pb_power_test_rtc_arm_wakeup();

    pb_power_test_log_clear();
    pb_power_test_log_event(1, pb_power_test_standby_cycles * PB_POWER_TEST_STANDBY_SECONDS);
    pb_power_test_log_event(20, RCC->BDCR);
    pb_power_test_log_event(21, RTC->ISR);
    pb_power_test_log_event(30, (pb_power_test_start_voltage_mv << 16) | pb_power_test_start_current_ma);
    pb_power_test_log_event(38, (pb_power_test_start_voltage_raw << 16) | pb_power_test_start_current_raw);
    pb_power_test_log_event(13, RTC->ISR);
}

static bool pb_power_test_restore_system_clock(void) {
    MODIFY_REG(FLASH->ACR, FLASH_ACR_LATENCY, FLASH_ACR_LATENCY_4WS);
    if ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_4WS) {
        return false;
    }

    RCC->CR |= RCC_CR_MSION;
    while (!(RCC->CR & RCC_CR_MSIRDY)) {
    }

    RCC->PLLCFGR |= RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
    }

    MODIFY_REG(RCC->CFGR, RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 | RCC_CFGR_SW, RCC_CFGR_PPRE1_DIV16 | RCC_CFGR_SW_PLL);
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }

    SystemCoreClock = 80000000U;
    return true;
}

void pb_power_test_supervisor_sleep(void) {
    uint32_t irq_enable[8];
    uint32_t primask = __get_PRIMASK();
    uint32_t sleep_control = SCB->SCR;
    uint32_t systick_load = SysTick->LOAD;
    uint32_t systick_ctrl = SysTick->CTRL;
    uint32_t systick_priority = NVIC_GetPriority(SysTick_IRQn);

    pbdrv_watchdog_prepare_for_stop();
    pbdrv_adc_prepare_for_stop();
    pbsys_status_light_force_off();
    SysTick->CTRL = 0;

    for (size_t i = 0; i < 8; i++) {
        irq_enable[i] = NVIC->ISER[i];
        NVIC->ICER[i] = UINT32_MAX;
        NVIC->ICPR[i] = UINT32_MAX;
    }

    PWR->CR1 = (PWR->CR1 & ~PWR_CR1_LPMS) | PWR_CR1_LPMS_STOP2;
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    pb_power_test_completed_cycles = 0;

    for (uint32_t cycle = 0; cycle < pb_power_test_standby_cycles; cycle++) {
        NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);
        NVIC_SetPriority(RTC_WKUP_IRQn, 0);
        __enable_irq();
        NVIC_EnableIRQ(RTC_WKUP_IRQn);
        PWR->SCR = PWR_SCR_CWUF;

        while (!pb_power_test_rtc_wake_detected) {
            __DSB();
            __ISB();
            __WFI();

            if (RTC->ISR & RTC_ISR_WUTF) {
                pb_power_test_rtc_disable_wakeup();
                pb_power_test_rtc_wake_armed = false;
                pb_power_test_rtc_wake_detected = true;
            }
        }

        __disable_irq();
        pbdrv_watchdog_update();
        pb_power_test_completed_cycles = cycle + 1;
        if (pb_power_test_standby_cycles <= 12 || pb_power_test_completed_cycles == 1 ||
            pb_power_test_completed_cycles == pb_power_test_standby_cycles || pb_power_test_completed_cycles % 180 == 0) {
            pb_power_test_log_event(31, pb_power_test_completed_cycles);
        }

        if (cycle == 0) {
            SystemCoreClock = 4000000U;
            pbdrv_adc_restore_after_stop();
            pb_power_test_log_event(34, SystemCoreClock);
            pb_power_test_measure_upper_bound();
            pbdrv_adc_prepare_for_stop();
        }

        if (pb_power_test_completed_cycles < pb_power_test_standby_cycles) {
            pb_power_test_rtc_arm_wakeup();
        }
    }

    SCB->SCR = sleep_control;
    PWR->CR1 &= ~PWR_CR1_LPMS;

    // Stop 2 wakes on MSI. Restore the retained Technic Hub PLL directly,
    // then restore the exact SysTick configuration that was active before sleep.
    bool clock_restored = pb_power_test_restore_system_clock();
    pbdrv_adc_restore_after_stop();
    pb_power_test_log_event(36, SystemCoreClock);
    SysTick->LOAD = systick_load;
    SysTick->VAL = 0;
    NVIC_SetPriority(SysTick_IRQn, systick_priority);
    SysTick->CTRL = systick_ctrl;

    // Keep the watchdog on the extended timeout during reset-free recovery.
    // The normal watchdog process continues refreshing it after wake.
    pbdrv_watchdog_update();

    NVIC_DisableIRQ(RTC_WKUP_IRQn);
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);
    irq_enable[(uint32_t)RTC_WKUP_IRQn >> 5] &= ~(1UL << ((uint32_t)RTC_WKUP_IRQn & 31U));
    for (size_t i = 0; i < 8; i++) {
        NVIC->ISER[i] = irq_enable[i];
    }

    pb_power_test_autostarted = true;
    pb_power_test_log_event(26, pb_power_test_rtc_wake_source);
    pb_power_test_log_event(27, clock_restored ? SystemCoreClock : 0);
    pb_power_test_log_event(28, SysTick->LOAD);
    __set_PRIMASK(primask);
}

mp_obj_t pb_power_test_standby_result(void) {
    if (!pb_power_test_autostarted) {
        return mp_const_none;
    }

    pb_power_test_autostarted = false;
    pbsys_status_light_force_off();

    pb_power_test_samples_t voltage;
    pb_power_test_samples_t current;
    pb_power_test_wait_ms(PB_POWER_TEST_SETTLE_MS);
    pb_power_test_measure(PB_POWER_TEST_SAMPLE_MS, PB_POWER_TEST_ACTIVE_SAMPLE_MS, &voltage, &current);
    pb_power_test_end_voltage_raw = pb_power_test_samples_mean(&voltage);
    pb_power_test_end_current_raw = pb_power_test_samples_mean(&current);
    pb_power_test_end_current_ma = pb_power_test_current_ma(pb_power_test_end_current_raw);
    pb_power_test_end_voltage_mv = pb_power_test_voltage_mv(pb_power_test_end_voltage_raw, pb_power_test_end_current_raw);
    pb_power_test_log_event(33, (pb_power_test_end_voltage_mv << 16) | pb_power_test_end_current_ma);
    pb_power_test_log_event(37, (pb_power_test_end_voltage_raw << 16) | pb_power_test_end_current_raw);
    pb_power_test_log_event(25, 0);
    pb_power_test_set_status_light(PBIO_COLOR_YELLOW);

    mp_obj_t items[] = {
        mp_obj_new_int_from_uint(pb_power_test_completed_cycles),
        mp_obj_new_int_from_uint(PB_POWER_TEST_STANDBY_SECONDS),
        mp_obj_new_int_from_uint(pb_power_test_completed_cycles * PB_POWER_TEST_STANDBY_SECONDS),
        mp_obj_new_int_from_uint(pb_power_test_start_voltage_mv),
        mp_obj_new_int_from_uint(pb_power_test_start_current_ma),
        mp_obj_new_int_from_uint(pb_power_test_end_voltage_mv),
        mp_obj_new_int_from_uint(pb_power_test_end_current_ma),
        mp_obj_new_int_from_uint(pb_power_test_upper_voltage_mv),
        mp_obj_new_int_from_uint(pb_power_test_upper_current_ma),
        mp_obj_new_int_from_uint(pb_power_test_start_voltage_raw),
        mp_obj_new_int_from_uint(pb_power_test_start_current_raw),
        mp_obj_new_int_from_uint(pb_power_test_end_voltage_raw),
        mp_obj_new_int_from_uint(pb_power_test_end_current_raw),
        mp_obj_new_int_from_uint(pb_power_test_upper_voltage_raw),
        mp_obj_new_int_from_uint(pb_power_test_upper_current_raw),
    };
    return mp_obj_new_tuple(MP_ARRAY_SIZE(items), items);
}

mp_obj_t pb_power_test_standby(uint32_t cycles) {
    pb_power_test_samples_t voltage;
    pb_power_test_samples_t current;

    pb_power_test_wait_ms(PB_POWER_TEST_SETTLE_MS);
    pb_power_test_measure(PB_POWER_TEST_SAMPLE_MS, PB_POWER_TEST_ACTIVE_SAMPLE_MS, &voltage, &current);
    pb_power_test_start_voltage_raw = pb_power_test_samples_mean(&voltage);
    pb_power_test_start_current_raw = pb_power_test_samples_mean(&current);
    pb_power_test_start_current_ma = pb_power_test_current_ma(pb_power_test_start_current_raw);
    pb_power_test_start_voltage_mv = pb_power_test_voltage_mv(pb_power_test_start_voltage_raw, pb_power_test_start_current_raw);
    pb_power_test_end_voltage_raw = 0;
    pb_power_test_end_current_raw = 0;
    pb_power_test_upper_voltage_mv = 0;
    pb_power_test_upper_current_ma = 0;
    pb_power_test_upper_voltage_raw = 0;
    pb_power_test_upper_current_raw = 0;
    pb_power_test_standby_cycles = cycles ? cycles : 1;
    pb_power_test_supervisor_sleep_pending = true;
    pbsys_program_stop(false);
    return mp_const_none;
}

#endif
