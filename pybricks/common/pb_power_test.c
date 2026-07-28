// SPDX-License-Identifier: MIT

#include "py/mpconfig.h"

#if PYBRICKS_PY_COMMON && PYBRICKS_PY_COMMON_SYSTEM

#include <stdbool.h>
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

#include <pbio/color.h>
#include <pbio/light.h>

#include "stm32l4xx.h"
#include STM32_HAL_H

#include "pb_power_test.h"

typedef struct {
    uint64_t sum;
    uint64_t sum_square;
    uint16_t minimum;
    uint16_t maximum;
    uint32_t count;
} pb_power_test_samples_t;

#define PB_POWER_TEST_SETTLE_MS 500U
#define PB_POWER_TEST_SAMPLE_MS 500U
#define PB_POWER_TEST_ACTIVE_MS 10000U
#define PB_POWER_TEST_ACTIVE_SAMPLE_MS 10U
#define PB_POWER_TEST_STANDBY_SECONDS 5U


#define PB_POWER_TEST_LOG_MAGIC 0xA55AU
#define PB_POWER_TEST_LOG_ERASED 0xFFFFU

typedef struct {
    uint16_t magic;
    uint16_t sequence;
    uint16_t event;
    uint16_t data;
} pb_power_test_log_record_t;

#define PB_POWER_TEST_RESET_CHECKPOINT_MAGIC 0x52544357U

typedef struct {
    uint32_t magic;
    uint16_t event;
    uint16_t data;
} pb_power_test_reset_checkpoint_t;

static pb_power_test_reset_checkpoint_t pb_power_test_reset_checkpoint __attribute__((section(".noinit"), used));
static uint16_t pb_power_test_log_next_index;
static bool pb_power_test_log_index_initialized;

extern uint8_t _pb_power_test_log_start[];

#define PB_POWER_TEST_LOG_ADDRESS ((uint32_t)_pb_power_test_log_start)
#define PB_POWER_TEST_LOG_CAPACITY (2048U / sizeof(pb_power_test_log_record_t))

static void pb_power_test_log_clear(void) {
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    FLASH_EraseInitTypeDef erase_init = {
        .Banks = FLASH_BANK_1,
        .Page = (PB_POWER_TEST_LOG_ADDRESS - FLASH_BASE) / FLASH_PAGE_SIZE,
        .NbPages = 1,
        .TypeErase = FLASH_TYPEERASE_PAGES,
    };

    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    uint32_t page_error;
    HAL_FLASHEx_Erase(&erase_init, &page_error);
    __set_PRIMASK(irq);
    HAL_FLASH_Lock();
    pb_power_test_log_next_index = 0;
    pb_power_test_log_index_initialized = true;
}

void pb_power_test_log_event(uint16_t event, uint16_t data) {
    const pb_power_test_log_record_t *records = (const pb_power_test_log_record_t *)PB_POWER_TEST_LOG_ADDRESS;
    uint32_t capacity = PB_POWER_TEST_LOG_CAPACITY;

    if (!pb_power_test_log_index_initialized) {
        while (pb_power_test_log_next_index < capacity && records[pb_power_test_log_next_index].magic != PB_POWER_TEST_LOG_ERASED) {
            pb_power_test_log_next_index++;
        }
        pb_power_test_log_index_initialized = true;
    }

    uint32_t index = pb_power_test_log_next_index;
    if (index == capacity) {
        return;
    }

    pb_power_test_log_record_t record = {
        .magic = PB_POWER_TEST_LOG_MAGIC,
        .sequence = index + 1,
        .event = event,
        .data = data,
    };
    uint64_t value;
    memcpy(&value, &record, sizeof(value));

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    HAL_StatusTypeDef status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, PB_POWER_TEST_LOG_ADDRESS + index * sizeof(record), value);
    __set_PRIMASK(irq);
    HAL_FLASH_Lock();
    if (status == HAL_OK) {
        pb_power_test_log_next_index++;
    }
}

mp_obj_t pb_power_test_log(void) {
    const pb_power_test_log_record_t *records = (const pb_power_test_log_record_t *)PB_POWER_TEST_LOG_ADDRESS;
    uint32_t capacity = PB_POWER_TEST_LOG_CAPACITY;
    mp_obj_t list = mp_obj_new_list(0, NULL);

    for (uint32_t i = 0; i < capacity; i++) {
        if (records[i].magic == PB_POWER_TEST_LOG_ERASED) {
            break;
        }
        if (records[i].magic != PB_POWER_TEST_LOG_MAGIC) {
            continue;
        }
        mp_obj_t items[] = {
            mp_obj_new_int_from_uint(records[i].sequence),
            mp_obj_new_int_from_uint(records[i].event),
            mp_obj_new_int_from_uint(records[i].data),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(MP_ARRAY_SIZE(items), items));
    }
    return list;
}

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
    uint64_t count = samples->count;
    uint64_t numerator = count * samples->sum_square - samples->sum * samples->sum;
    return numerator / (count * count);
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
    pb_power_test_dict_store(report, MP_QSTR_electronics, electronics);

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

bool pb_power_test_boot_autostart_check(void) {
    if (pb_power_test_reset_checkpoint.magic == PB_POWER_TEST_RESET_CHECKPOINT_MAGIC) {
        pb_power_test_log_event(24, pb_power_test_reset_checkpoint.event);
        pb_power_test_reset_checkpoint.magic = 0;
    }
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
    pb_power_test_log_event(20, 0);
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

void pb_power_test_boot_autostart_failed(void) {
    pb_power_test_log_event(23, 0);
    pb_power_test_set_status_light(PBIO_COLOR_RED);
}

bool pb_power_test_supervisor_sleep_requested(void) {
    return pb_power_test_supervisor_sleep_pending;
}

void RTC_WKUP_IRQHandler(void) {
    bool wake_timer_elapsed = pb_power_test_rtc_wake_armed && (RTC->ISR & RTC_ISR_WUTF);

    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;
    RTC->ISR &= ~RTC_ISR_WUTF;
    RTC->WPR = 0xFF;
    EXTI->PR1 = EXTI_PR1_PIF20;
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);

    if (wake_timer_elapsed) {
        pb_power_test_rtc_wake_armed = false;
        pb_power_test_reset_checkpoint.magic = PB_POWER_TEST_RESET_CHECKPOINT_MAGIC;
        pb_power_test_reset_checkpoint.event = 14;
        pb_power_test_reset_checkpoint.data = 0;
        __DSB();
        NVIC_SystemReset();
    }
}

void pb_power_test_supervisor_sleep(void) {
    pb_power_test_log_event(7, 0);
    pb_power_test_supervisor_sleep_pending = false;
    pb_power_test_log_event(8, 0);
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;
    PWR->CR1 |= PWR_CR1_DBP;
    while (!(PWR->CR1 & PWR_CR1_DBP)) {
    }

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

    pb_power_test_rtc_wake_armed = false;
    NVIC_DisableIRQ(RTC_WKUP_IRQn);
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);
    EXTI->IMR1 &= ~EXTI_IMR1_IM20;
    EXTI->PR1 = EXTI_PR1_PIF20;

    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;
    RTC->CR &= ~(RTC_CR_WUTE | RTC_CR_WUTIE);
    while (!(RTC->ISR & RTC_ISR_WUTWF)) {
    }
    RTC->ISR &= ~RTC_ISR_WUTF;
    RTC->WUTR = PB_POWER_TEST_STANDBY_SECONDS - 1U;
    RTC->CR = (RTC->CR & ~RTC_CR_WUCKSEL) | RTC_CR_WUCKSEL_2;
    RTC->CR |= RTC_CR_WUTIE | RTC_CR_WUTE;
    RTC->WPR = 0xFF;
    pb_power_test_log_event(9, 0);

    EXTI->EMR1 &= ~EXTI_EMR1_EM20;
    EXTI->RTSR1 |= EXTI_RTSR1_RT20;
    EXTI->PR1 = EXTI_PR1_PIF20;
    EXTI->IMR1 |= EXTI_IMR1_IM20;

    pbdrv_watchdog_prepare_for_stop();
    pb_power_test_log_event(10, 0);
    pb_power_test_set_status_light(PBIO_COLOR_BLACK);
    pb_power_test_log_event(11, 0);
    SysTick->CTRL = 0;

    for (size_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = UINT32_MAX;
        NVIC->ICPR[i] = UINT32_MAX;
    }
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);
    NVIC_SetPriority(RTC_WKUP_IRQn, 0);
    pb_power_test_rtc_wake_armed = true;
    __enable_irq();
    NVIC_EnableIRQ(RTC_WKUP_IRQn);

    PWR->SCR = PWR_SCR_CWUF;
    PWR->CR1 = (PWR->CR1 & ~PWR_CR1_LPMS) | PWR_CR1_LPMS_STOP2;
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;

    pb_power_test_log_event(12, 0);

    for (;;) {
        __DSB();
        __ISB();
        __WFI();

        if (RTC->ISR & RTC_ISR_WUTF) {
            pb_power_test_reset_checkpoint.magic = PB_POWER_TEST_RESET_CHECKPOINT_MAGIC;
            pb_power_test_reset_checkpoint.event = 14;
            pb_power_test_reset_checkpoint.data = 1;
            __DSB();
            NVIC_SystemReset();
        }
    }
}

mp_obj_t pb_power_test_standby_result(void) {
    if (!pb_power_test_autostarted) {
        return mp_const_none;
    }

    pb_power_test_autostarted = false;
    pb_power_test_set_status_light(PBIO_COLOR_YELLOW);

    mp_obj_t report = mp_obj_new_dict(0);
    pb_power_test_dict_store(report, MP_QSTR_test, mp_obj_new_str("stop2_reset_autostart", 21));
    pb_power_test_dict_store(report, MP_QSTR_completed, mp_const_true);
    pb_power_test_dict_store(report, MP_QSTR_requested_seconds, mp_obj_new_int_from_uint(PB_POWER_TEST_STANDBY_SECONDS));
    return report;
}

void pb_power_test_log_begin_sleep_sequence(void) {
    pb_power_test_log_clear();
    pb_power_test_log_event(1, 0);
    pb_power_test_log_event(2, 0);
    pb_power_test_log_event(3, 0);
    pb_power_test_log_event(4, 0);
    pb_power_test_log_event(5, 0);
    pb_power_test_log_event(6, 0);
}

mp_obj_t pb_power_test_standby(void) {
    pb_power_test_wait_ms(PB_POWER_TEST_SETTLE_MS);
    pb_power_test_supervisor_sleep_pending = true;
    pbsys_program_stop(false);
    return mp_const_none;
}

#endif
