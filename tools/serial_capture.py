# Diagnostic helper: capture the ESP32-S3 USB CDC serial log across a reset.
# The USB-JTAG reset drops the USB connection, so reads after the reset fail:
# catch, wait for re-enumeration, reopen, keep reading.
# Usage: python tools/serial_capture.py <port> <seconds>
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM13"
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 90


def open_port():
    for _ in range(20):
        try:
            return serial.Serial(port, 115200, timeout=1)
        except serial.SerialException:
            time.sleep(0.5)
    return None


ser = open_port()
if ser is None:
    sys.exit("could not open %s" % port)

if len(sys.argv) > 3 and sys.argv[3] == "reset":
    # esptool's USB-JTAG-Serial hard-reset sequence. WARNING: this sometimes
    # wedges the chip's USB stack until a physical power-cycle — prefer a
    # manual reset and pass no third argument.
    ser.dtr = False
    ser.rts = True
    ser.dtr = True
    ser.rts = False
    ser.dtr = False

buf = b""
deadline = time.time() + seconds
while time.time() < deadline:
    try:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
    except serial.SerialException:
        # Port dropped when the chip reset; wait for it to come back.
        try:
            ser.close()
        except Exception:
            pass
        ser = open_port()
        if ser is None:
            break
try:
    ser.close()
except Exception:
    pass
sys.stdout.write(buf.decode("utf-8", errors="replace"))
