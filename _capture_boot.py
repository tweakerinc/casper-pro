import serial
import time
import sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM15"
ser = serial.Serial(port, 115200, timeout=0.2)
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setRTS(False)
ser.setDTR(True)
time.sleep(0.05)
ser.reset_input_buffer()
end = time.time() + 16
while time.time() < end:
    n = ser.in_waiting
    if n:
        data = ser.read(n)
        sys.stdout.write(data.decode("utf-8", errors="replace"))
        sys.stdout.flush()
    else:
        time.sleep(0.05)
ser.close()
