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
    14: "RTC_WAKE_FLAG_VALID",
    18: "AUTOSTART_MARKER_MISSING",
    19: "AUTOSTART_MARKER_FOUND",
    22: "SLOT0_START_ACCEPTED",
    23: "SLOT0_START_FAILED",
    24: "RESET_CHECKPOINT_RECOVERED",
}

records = hub.system.power_test_log()

if not records:
    print("Power log is empty.")
else:
    print("Persistent power log:")
    for sequence, event, data_hex in records:
        name = event_names.get(event, "UNKNOWN_EVENT")
        data_small = int(data_hex, 16) if data_hex[:1] == "0" else None
        if event in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
            print(sequence, name, "0x" + data_hex)
        elif event == 18 and data_small is not None:
            detail = {1: "user-data read failed", 2: "marker value invalid"}.get(data_small, data_hex)
            print(sequence, name, detail)
        elif event == 24 and data_small is not None:
            recovered = event_names.get(data_small, "event " + str(data_small))
            print(sequence, name, recovered)
        else:
            print(sequence, name, data_hex)
