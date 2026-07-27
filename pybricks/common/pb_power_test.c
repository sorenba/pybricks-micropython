// SPDX-License-Identifier: MIT

#include "py/mpconfig.h"

#if PYBRICKS_PY_COMMON && PYBRICKS_PY_COMMON_SYSTEM

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include <pbdrv/adc.h>
#include <pbdrv/battery.h>
#include <pbdrv/clock.h>
#include <pbsys/status.h>

#include "stm32l4xx.h"

#include "pb_power_test.h"

typedef struct {
    uint64_t sum;
    uint64_t sum_square;
    uint16_t minimum;
    uint16_t maximum;
    uint32_t count;
} pb_power_test_samples_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t before_voltage_sum;
    uint32_t before_current_sum;
    uint32_t before_voltage_min_max;
    uint32_t before_current_min_max;
    uint32_t before_sample_count;
    uint32_t requested_seconds;
    uint32_t checksum;
} pb_power_test_backup_t;

#define PB_POWER_TEST_BACKUP_MAGIC 0x50575431U
#define PB_POWER_TEST_BACKUP_VERSION 1U
#define PB_POWER_TEST_SETTLE_MS 500U
#define PB_POWER_TEST_SAMPLE_MS 500U
#define PB_POWER_TEST_ACTIVE_MS 10000U
#define PB_POWER_TEST_ACTIVE_SAMPLE_MS 10U
#define PB_POWER_TEST_STANDBY_SECONDS 60U

static volatile bool pb_power_test_idle_monitoring;
static volatile uint32_t pb_power_test_idle_start_us;
static volatile uint64_t pb_power_test_idle_us;
static volatile uint32_t pb_power_test_idle_entries;

static void pb_power_test_samples_reset(pb_power_test_samples_t *samples) {
    samples->sum = 0;
    samples->sum_square = 0;
    samples->minimum = UINT16_MAX;
    samples->maximum = 0;
    samples->count = 0;
}

static void pb_power_test_samples_add(pb_power_test_samples_t *samples, uint16_t value) {
    samples->sum += value;
    samples->sum_square += (uint64_t)value * value;
    if (value < samples->minimum) {
        samples->minimum = value;
    }
    if (value > samples->maximum) {
        samples->maximum = value;
    }
    samples->count++;
}

static uint32_t pb_power_test_samples_mean(const pb_power_test_samples_t *samples) {
    return samples->count ? samples->sum / samples->count : 0;
}

static uint64_t pb_power_test_samples_variance(const pb_power_test_samples_t *samples) {
    if (!samples->count) {
        return 0;
    }
    uint64_t mean = samples->sum / samples->count;
    return samples->sum_square / samples->count - mean * mean;
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

static mp_obj_t pb_power_test_uint64_string(uint64_t value) {
    char buffer[20];
    size_t length = 0;

    do {
        buffer[length++] = '0' + value % 10;
        value /= 10;
    } while (value);

    for (size_t i = 0; i < length / 2; i++) {
        char temporary = buffer[i];
        buffer[i] = buffer[length - i - 1];
        buffer[length - i - 1] = temporary;
    }

    return mp_obj_new_str(buffer, length);
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
    pb_power_test_dict_store(dict, MP_QSTR_voltage_raw_min, mp_obj_new_int_from_uint(voltage->count ? voltage->minimum : 0));
    pb_power_test_dict_store(dict, MP_QSTR_voltage_raw_max, mp_obj_new_int_from_uint(voltage->maximum));
    pb_power_test_dict_store(dict, MP_QSTR_voltage_raw_variance, pb_power_test_uint64_string(pb_power_test_samples_variance(voltage)));
    pb_power_test_dict_store(dict, MP_QSTR_voltage_mv_mean, mp_obj_new_int_from_uint(pb_power_test_voltage_mv(voltage_raw_mean, current_raw_mean)));
    pb_power_test_dict_store(dict, MP_QSTR_current_raw_mean, mp_obj_new_int_from_uint(current_raw_mean));
    pb_power_test_dict_store(dict, MP_QSTR_current_raw_min, mp_obj_new_int_from_uint(current->count ? current->minimum : 0));
    pb_power_test_dict_store(dict, MP_QSTR_current_raw_max, mp_obj_new_int_from_uint(current->maximum));
    pb_power_test_dict_store(dict, MP_QSTR_current_raw_variance, pb_power_test_uint64_string(pb_power_test_samples_variance(current)));
    pb_power_test_dict_store(dict, MP_QSTR_current_ma_mean, mp_obj_new_int_from_uint(pb_power_test_current_ma(current_raw_mean)));
    pb_power_test_dict_store(dict, MP_QSTR_sample_count, mp_obj_new_int_from_uint(voltage->count));
    return dict;
}

void pb_power_test_idle_enter(void) {
    if (pb_power_test_idle_monitoring) {
        pb_power_test_idle_start_us = pbdrv_clock_get_us();
    }
}

void pb_power_test_idle_exit(void) {
    if (pb_power_test_idle_monitoring) {
        pb_power_test_idle_us += pbdrv_clock_get_us() - pb_power_test_idle_start_us;
        pb_power_test_idle_entries++;
    }
}

mp_obj_t pb_power_test_active(void) {
    pb_power_test_samples_t voltage;
    pb_power_test_samples_t current;

    pb_power_test_wait_ms(PB_POWER_TEST_SETTLE_MS);

    pb_power_test_idle_us = 0;
    pb_power_test_idle_entries = 0;
    pb_power_test_idle_monitoring = true;
    uint32_t start_us = pbdrv_clock_get_us();
    pb_power_test_measure(PB_POWER_TEST_ACTIVE_MS, PB_POWER_TEST_ACTIVE_SAMPLE_MS, &voltage, &current);
    uint32_t elapsed_us = pbdrv_clock_get_us() - start_us;
    pb_power_test_idle_monitoring = false;

    uint64_t idle_us = pb_power_test_idle_us;
    if (idle_us > elapsed_us) {
        idle_us = elapsed_us;
    }

    mp_obj_t report = mp_obj_new_dict(0);
    pb_power_test_dict_store(report, MP_QSTR_test, mp_obj_new_str("active_idle", 11));
    pb_power_test_dict_store(report, MP_QSTR_duration_us, mp_obj_new_int_from_uint(elapsed_us));
    pb_power_test_dict_store(report, MP_QSTR_electrical, pb_power_test_sample_dict(&voltage, &current));

    mp_obj_t cpu = mp_obj_new_dict(0);
    pb_power_test_dict_store(cpu, MP_QSTR_idle_us, mp_obj_new_int_from_uint((uint32_t)idle_us));
    pb_power_test_dict_store(cpu, MP_QSTR_active_us, mp_obj_new_int_from_uint((uint32_t)(elapsed_us - idle_us)));
    pb_power_test_dict_store(cpu, MP_QSTR_idle_entries, mp_obj_new_int_from_uint(pb_power_test_idle_entries));
    pb_power_test_dict_store(cpu, MP_QSTR_idle_percent_x1000, mp_obj_new_int_from_uint((uint32_t)(elapsed_us ? idle_us * 100000ULL / elapsed_us : 0)));
    pb_power_test_dict_store(report, MP_QSTR_cpu, cpu);

    mp_obj_t electronics = mp_obj_new_dict(0);
    pb_power_test_dict_store(electronics, MP_QSTR_ble_host_connected, mp_obj_new_bool(pbsys_status_test(PBIO_PYBRICKS_STATUS_BLE_HOST_CONNECTED)));
    pb_power_test_dict_store(electronics, MP_QSTR_system_clock_hz, mp_obj_new_int_from_uint(SystemCoreClock));
    pb_power_test_dict_store(electronics, MP_QSTR_gpioa_clock_enabled, mp_obj_new_bool((RCC->AHB2ENR & RCC_AHB2ENR_GPIOAEN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_gpiob_clock_enabled, mp_obj_new_bool((RCC->AHB2ENR & RCC_AHB2ENR_GPIOBEN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_gpioc_clock_enabled, mp_obj_new_bool((RCC->AHB2ENR & RCC_AHB2ENR_GPIOCEN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_adc_clock_enabled, mp_obj_new_bool((RCC->AHB2ENR & RCC_AHB2ENR_ADCEN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_dma1_clock_enabled, mp_obj_new_bool((RCC->AHB1ENR & RCC_AHB1ENR_DMA1EN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_dma2_clock_enabled, mp_obj_new_bool((RCC->AHB1ENR & RCC_AHB1ENR_DMA2EN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_spi1_clock_enabled, mp_obj_new_bool((RCC->APB2ENR & RCC_APB2ENR_SPI1EN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_spi3_clock_enabled, mp_obj_new_bool((RCC->APB1ENR1 & RCC_APB1ENR1_SPI3EN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_usart1_clock_enabled, mp_obj_new_bool((RCC->APB2ENR & RCC_APB2ENR_USART1EN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_usart2_clock_enabled, mp_obj_new_bool((RCC->APB1ENR1 & RCC_APB1ENR1_USART2EN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_usart3_clock_enabled, mp_obj_new_bool((RCC->APB1ENR1 & RCC_APB1ENR1_USART3EN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_tim1_clock_enabled, mp_obj_new_bool((RCC->APB2ENR & RCC_APB2ENR_TIM1EN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_tim2_clock_enabled, mp_obj_new_bool((RCC->APB1ENR1 & RCC_APB1ENR1_TIM2EN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_rtc_enabled, mp_obj_new_bool((RCC->BDCR & RCC_BDCR_RTCEN) != 0));
    pb_power_test_dict_store(electronics, MP_QSTR_debug_sleep_enabled, mp_obj_new_bool((DBGMCU->CR & DBGMCU_CR_DBG_SLEEP) != 0));
    pb_power_test_dict_store(report, MP_QSTR_electronics, electronics);

    return report;
}

static uint32_t pb_power_test_backup_checksum(const pb_power_test_backup_t *backup) {
    const uint32_t *values = (const uint32_t *)backup;
    uint32_t checksum = 0x6D2B79F5U;
    for (size_t i = 0; i < sizeof(*backup) / sizeof(uint32_t) - 1; i++) {
        checksum = (checksum << 5) | (checksum >> 27);
        checksum ^= values[i];
    }
    return checksum;
}

static void pb_power_test_backup_write(const pb_power_test_backup_t *backup) {
    const uint32_t *values = (const uint32_t *)backup;
    for (size_t i = 0; i < sizeof(*backup) / sizeof(uint32_t); i++) {
        (&RTC->BKP0R)[i] = values[i];
    }
}

static bool pb_power_test_backup_read(pb_power_test_backup_t *backup) {
    uint32_t *values = (uint32_t *)backup;
    for (size_t i = 0; i < sizeof(*backup) / sizeof(uint32_t); i++) {
        values[i] = (&RTC->BKP0R)[i];
    }
    return backup->magic == PB_POWER_TEST_BACKUP_MAGIC && backup->version == PB_POWER_TEST_BACKUP_VERSION && backup->checksum == pb_power_test_backup_checksum(backup);
}

static void pb_power_test_backup_clear(void) {
    RTC->BKP0R = 0;
}

static void pb_power_test_rtc_enable_backup_access(void) {
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;
    PWR->CR1 |= PWR_CR1_DBP;
    while (!(PWR->CR1 & PWR_CR1_DBP)) {
    }
}

static void pb_power_test_enter_standby_60_seconds(void) {
    pb_power_test_rtc_enable_backup_access();

    RCC->CSR |= RCC_CSR_LSION;
    while (!(RCC->CSR & RCC_CSR_LSIRDY)) {
    }

    if (!(RCC->BDCR & RCC_BDCR_RTCEN)) {
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
    }

    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;
    RTC->CR &= ~RTC_CR_WUTE;
    while (!(RTC->ISR & RTC_ISR_WUTWF)) {
    }
    RTC->ISR &= ~RTC_ISR_WUTF;
    RTC->WUTR = PB_POWER_TEST_STANDBY_SECONDS - 1U;
    RTC->CR = (RTC->CR & ~RTC_CR_WUCKSEL) | RTC_CR_WUCKSEL_2;
    RTC->CR |= RTC_CR_WUTIE | RTC_CR_WUTE;
    RTC->WPR = 0xFF;

    EXTI->PR1 = EXTI_PR1_PIF20;
    EXTI->IMR1 |= EXTI_IMR1_IM20;
    EXTI->RTSR1 |= EXTI_RTSR1_RT20;

    PWR->SCR = PWR_SCR_CSBF | PWR_SCR_CWUF;
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    PWR->CR1 = (PWR->CR1 & ~PWR_CR1_LPMS) | PWR_CR1_LPMS_STANDBY;
    __DSB();
    __WFI();
    for (;;) {
    }
}

mp_obj_t pb_power_test_standby_result(void) {
    pb_power_test_rtc_enable_backup_access();

    pb_power_test_backup_t backup;
    bool standby_wake = (PWR->SR1 & PWR_SR1_SBF) != 0;
    if (!standby_wake || !pb_power_test_backup_read(&backup)) {
        return mp_const_none;
    }

    pb_power_test_samples_t voltage;
    pb_power_test_samples_t current;
    pb_power_test_wait_ms(PB_POWER_TEST_SETTLE_MS);
    pb_power_test_measure(PB_POWER_TEST_SAMPLE_MS, 1, &voltage, &current);

    mp_obj_t report = mp_obj_new_dict(0);
    pb_power_test_dict_store(report, MP_QSTR_test, mp_obj_new_str("standby_rtc", 11));
    pb_power_test_dict_store(report, MP_QSTR_completed, mp_const_true);
    pb_power_test_dict_store(report, MP_QSTR_standby_wake, mp_const_true);
    pb_power_test_dict_store(report, MP_QSTR_requested_seconds, mp_obj_new_int_from_uint(backup.requested_seconds));

    mp_obj_t before = mp_obj_new_dict(0);
    uint32_t before_voltage_raw_mean = backup.before_voltage_sum / backup.before_sample_count;
    uint32_t before_current_raw_mean = backup.before_current_sum / backup.before_sample_count;
    pb_power_test_dict_store(before, MP_QSTR_voltage_raw_mean, mp_obj_new_int_from_uint(before_voltage_raw_mean));
    pb_power_test_dict_store(before, MP_QSTR_voltage_raw_min, mp_obj_new_int_from_uint(backup.before_voltage_min_max & 0xFFFF));
    pb_power_test_dict_store(before, MP_QSTR_voltage_raw_max, mp_obj_new_int_from_uint(backup.before_voltage_min_max >> 16));
    pb_power_test_dict_store(before, MP_QSTR_voltage_mv_mean, mp_obj_new_int_from_uint(pb_power_test_voltage_mv(before_voltage_raw_mean, before_current_raw_mean)));
    pb_power_test_dict_store(before, MP_QSTR_current_raw_mean, mp_obj_new_int_from_uint(before_current_raw_mean));
    pb_power_test_dict_store(before, MP_QSTR_current_raw_min, mp_obj_new_int_from_uint(backup.before_current_min_max & 0xFFFF));
    pb_power_test_dict_store(before, MP_QSTR_current_raw_max, mp_obj_new_int_from_uint(backup.before_current_min_max >> 16));
    pb_power_test_dict_store(before, MP_QSTR_current_ma_mean, mp_obj_new_int_from_uint(pb_power_test_current_ma(before_current_raw_mean)));
    pb_power_test_dict_store(before, MP_QSTR_sample_count, mp_obj_new_int_from_uint(backup.before_sample_count));
    pb_power_test_dict_store(report, MP_QSTR_before, before);
    pb_power_test_dict_store(report, MP_QSTR_after, pb_power_test_sample_dict(&voltage, &current));
    pb_power_test_backup_clear();
    PWR->SCR = PWR_SCR_CSBF | PWR_SCR_CWUF;
    return report;
}

mp_obj_t pb_power_test_standby(void) {
    pb_power_test_rtc_enable_backup_access();

    pb_power_test_samples_t voltage;
    pb_power_test_samples_t current;
    pb_power_test_wait_ms(PB_POWER_TEST_SETTLE_MS);
    pb_power_test_measure(PB_POWER_TEST_SAMPLE_MS, 1, &voltage, &current);

    backup.magic = PB_POWER_TEST_BACKUP_MAGIC;
    backup.version = PB_POWER_TEST_BACKUP_VERSION;
    backup.before_voltage_sum = voltage.sum;
    backup.before_current_sum = current.sum;
    backup.before_voltage_min_max = voltage.minimum | ((uint32_t)voltage.maximum << 16);
    backup.before_current_min_max = current.minimum | ((uint32_t)current.maximum << 16);
    backup.before_sample_count = voltage.count;
    backup.requested_seconds = PB_POWER_TEST_STANDBY_SECONDS;
    backup.checksum = pb_power_test_backup_checksum(&backup);
    pb_power_test_backup_write(&backup);
    pb_power_test_enter_standby_60_seconds();
    return mp_const_none;
}

#endif
