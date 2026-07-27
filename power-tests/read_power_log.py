from pybricks.hubs import TechnicHub

hub = TechnicHub()

event_names = {
    1: "TEST_REQUESTED",
    2: "PROGRAM_STOP_REQUESTED",
    3: "PROGRAM_CLEANUP_COMPLETE",
    4: "AUTOSTART_MARKER_REQUESTED",
    5: "STORAGE_SAVE_BEGIN",
    6: "STORAGE_SAVE_COMPLETE",
    7: "SUPERVISOR_SLEEP_ENTER",
    8: "RTC_SETUP_BEGIN",
    9: "RTC_TIMER_ARMED",
    10: "WATCHDOG_EXTENDED",
    11: "LED_OFF",
    12: "STOP2_WFI_ENTER",
    13: "RTC_IRQ_ENTER",
    14: "RTC_WAKE_FLAG_VALID",
    15: "SYSTEM_RESET_REQUESTED",
    16: "BOOT_SYSTEM_INITIALIZED",
    17: "AUTOSTART_CHECK_BEGIN",
    18: "AUTOSTART_MARKER_MISSING",
    19: "AUTOSTART_MARKER_FOUND",
    20: "AUTOSTART_MARKER_CLEAR",
    22: "SLOT0_START_ACCEPTED",
    23: "SLOT0_START_FAILED",
    24: "RESET_CHECKPOINT_RECOVERED",
}

records = hub.system.power_test_log()

if not records:
    print("Power log is empty.")
else:
    print("Persistent power log:")
    for sequence, event, data in records:
        name = event_names.get(event, "UNKNOWN_EVENT")
        if event == 18:
            detail = {1: "user-data read failed", 2: "marker value invalid"}.get(data, str(data))
            print(sequence, name, detail)
        elif event == 24:
            recovered = event_names.get(data, "event " + str(data))
            print(sequence, name, recovered)
        else:
            print(sequence, name, data)
