from pybricks.hubs import TechnicHub

hub = TechnicHub()

event_names = {
    1: "RTC_PREPARE_BEGIN",
    2: "RCC_CSR",
    3: "RCC_BDCR",
    4: "RTC_CR",
    5: "RTC_ISR",
    6: "RTC_PRER",
    7: "RTC_WUTR",
    8: "EXTI_IMR1",
    9: "EXTI_RTSR1",
    10: "RTC_NVIC_STATE",
    11: "PWR_CR1",
    12: "RTC_ARMED_SNAPSHOT_SAVED",
    13: "RTC_ENABLE_CONFIRMED",
    14: "RTC_WAKE_FLAG_VALID",
    16: "BOOT_SYSTEM_INITIALIZED",
    17: "AUTOSTART_CHECK_BEGIN",
    18: "AUTOSTART_MARKER_MISSING",
    19: "AUTOSTART_MARKER_FOUND",
    20: "RTC_DOMAIN_REINITIALIZED",
    21: "RTC_WAKE_FLAG_CLEARED",
    22: "SLOT0_START_ACCEPTED",
    23: "SLOT0_START_FAILED",
    24: "RESET_CHECKPOINT_RECOVERED",
    25: "STANDBY_RESULT_CONSUMED",
    26: "STOP2_WAKE_RETURNED",
    27: "SYSTEM_CLOCK_RESTORED",
    28: "SYSTICK_RESTORED",
    29: "AUTOSTART_MARKER_CLEARED",
    30: "BATTERY_START_SAMPLE",
    31: "AUTONOMOUS_SLEEP_CYCLE_WAKE",
    32: "MINIMAL_WAKE_UPPER_BOUND",
    33: "BATTERY_END_SAMPLE",
    34: "ADC_MINIMAL_WAKE_RESTORED",
    35: "MINIMAL_WAKE_RAW_SAMPLE",
    36: "ADC_ACTIVE_RESTORED",
    37: "BATTERY_END_RAW_SAMPLE",
    38: "BATTERY_START_RAW_SAMPLE",
}

records = hub.system.power_test_log()

if not records:
    print("Power log is empty.")
else:
    print("Persistent power log:")
    for sequence, event, data_hex in records:
        name = event_names.get(event, "UNKNOWN_EVENT")
        data_small = int(data_hex, 16) if data_hex[:1] == "0" else None
        if event in (30, 32, 33):
            voltage_mv = int(data_hex[:4], 16)
            current_ma = int(data_hex[4:], 16)
            print(sequence, name, "voltage", voltage_mv, "mV current", current_ma, "mA")
        elif event in (35, 37, 38):
            voltage_raw = int(data_hex[:4], 16)
            current_raw = int(data_hex[4:], 16)
            print(sequence, name, "voltage raw", voltage_raw, "current raw", current_raw)
        elif event in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 20, 21, 27, 28, 34, 36):
            print(sequence, name, "0x" + data_hex)
        elif event == 18 and data_small is not None:
            detail = {1: "user-data read failed", 2: "marker value invalid"}.get(data_small, data_hex)
            print(sequence, name, detail)
        elif event == 26 and data_small is not None:
            source = {0: "RTC IRQ", 1: "post-WFI flag"}.get(data_small, data_hex)
            print(sequence, name, source)
        elif event == 24 and data_small is not None:
            recovered = event_names.get(data_small, "event " + str(data_small))
            print(sequence, name, recovered)
        else:
            print(sequence, name, data_hex)
