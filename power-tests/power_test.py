from pybricks.hubs import TechnicHub
from pybricks.pupdevices import Motor
from pybricks.parameters import Port
from pybricks.tools import wait

hub = TechnicHub()
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

print("4. Entering 60-second Stop 2 + RTC test...")
stop2_report = hub.system.power_test_standby()

print("5. RTC wake completed. LED should now be yellow.")
print(stop2_report)
