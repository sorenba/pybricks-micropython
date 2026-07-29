from pybricks.hubs import TechnicHub
from pybricks.parameters import Color
from pybricks.tools import wait

SLEEP_CYCLES = 6
# 6 = 30 seconds, 180 = 15 minutes, 360 = 30 minutes, 720 = 60 minutes.

hub = TechnicHub()

wake_report = hub.system.power_test_standby_result()

if wake_report is not None:
    cycles, seconds_per_cycle, total_sleep_seconds, start_voltage_mv, start_current_ma, end_voltage_mv, end_current_ma, upper_voltage_mv, upper_current_ma, upper_current_raw = wake_report
    hub.light.on(Color.YELLOW)
    print("Stop 2 battery test completed without Python between RTC wakes.")
    print("Sleep cycles:", cycles)
    print("Seconds per cycle:", seconds_per_cycle)
    print("Total Stop 2 time:", total_sleep_seconds, "seconds")
    print("Starting voltage:", start_voltage_mv, "mV")
    print("Ending voltage:", end_voltage_mv, "mV")
    print("Voltage change:", end_voltage_mv - start_voltage_mv, "mV")
    print("Starting active current:", start_current_ma, "mA")
    print("Ending active current:", end_current_ma, "mA")
    print("Minimal-wake upper-bound voltage:", upper_voltage_mv, "mV")
    print("Minimal-wake upper-bound current:", upper_current_ma, "mA")
    print("Minimal-wake raw current:", upper_current_raw)
    wait(3000)
else:
    # print("3. Running 10-second active idle power test...")
    # active_report = hub.system.power_test_active()
    # print(active_report)

    print("Measuring starting voltage, then running", SLEEP_CYCLES, "autonomous 5-second Stop 2 cycles...")
    hub.system.power_test_standby(SLEEP_CYCLES)
