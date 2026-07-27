from pybricks.hubs import TechnicHub
from pybricks.pupdevices import Motor
from pybricks.parameters import Color, Port
from pybricks.tools import wait

hub = TechnicHub()

wake_report = hub.system.power_test_standby_result()

if wake_report is not None:
    hub.light.on(Color.YELLOW)
    print("Stop 2 RTC wake completed and stored program autostarted.")
    print(wake_report)
else:
    motor = Motor(Port.A)

    print("1. Running motor...")
    motor.run(500)
    wait(2000)

    print("2. Stopping motor and returning to idle...")
    motor.stop()
    wait(2000)

    print("3. Running 10-second active idle power test...")
    active_report = hub.system.power_test_active()
    print(active_report)

    print("4. Requesting supervisor-controlled 30-second Stop 2 + RTC test...")
    hub.system.power_test_standby()
