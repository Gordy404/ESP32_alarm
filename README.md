# ESP32_alarm
rduino IDE sketch for the ESP32 DevKit V1. It reads a 3x4 keypad and sounds a key beep. It arms with ***** followed by #, gives an exit delay ,  when delay ends checks the door switch and PIR. If either is active, it sets the relay (through the NPN transistor) HIGH for 10 minutes. Entering the code disarms it. (DISARMED, EXIT_DELAY, ARMED, ALARM) 
