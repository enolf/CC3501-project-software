import subprocess
import sys
import threading

import serial
import serial.tools.list_ports
from serial import SerialException, SerialTimeoutException

# Pico/RP2040 default USB vendor ID (Raspberry Pi Foundation) is 0x2E8A.
# If you're using a custom board with your own VID/PID, swap this out.
RP2040_VID = 0x2E8A


def find_rp2040_port():
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if port.vid == RP2040_VID:
            return port.device
    return None


def read_from_rp2040(ser, stop_event):
    """Background thread: print anything the RP2040 sends back."""
    while not stop_event.is_set():
        try:
            line = ser.readline()
            if line:
                print("RP2040 >>", line.decode(errors="replace").strip())
        except SerialException:
            # Port closed out from under us — expected during shutdown.
            break


def main():
    port = find_rp2040_port()
    if port is None:
        print("No RP2040 device found. Available ports:")
        for p in serial.tools.list_ports.comports():
            vidpid = f" (VID:PID {p.vid:04x}:{p.pid:04x})" if p.vid else ""
            print(f"  {p.device} — {p.description}{vidpid}")
        sys.exit(1)

    print(f"Found RP2040 on {port}")

    try:
        ser = serial.Serial(port, 115200, timeout=1, write_timeout=1)
    except SerialException as e:
        print(f"Could not open {port}: {e}")
        sys.exit(1)

    stop_event = threading.Event()
    reader_thread = threading.Thread(
        target=read_from_rp2040, args=(ser, stop_event), daemon=True
    )
    reader_thread.start()

    proc = subprocess.Popen(
        ["./build/PiCapture"], stdout=subprocess.PIPE, text=True, bufsize=1
    )
    if proc.stdout is None:
        stop_event.set()
        ser.close()
        raise RuntimeError("Failed to open subprocess stdout")

    try:
        for line in proc.stdout:
            line = line.strip()
            if not line:
                continue
            print("Forwarding:", line)
            try:
                ser.write((line + "\n").encode())
            except SerialTimeoutException:
                print("Serial write timed out - RP2040 not draining buffer")
                break
    except KeyboardInterrupt:
        print("Keyboard interrupt, shutting down")
    finally:
        print("Cleaning up...")
        stop_event.set()

        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            print("PiCapture didn't exit in time, killing it")
            proc.kill()
            proc.wait()

        reader_thread.join(timeout=2)

        ser.close()
        print("Shutdown complete.")


if __name__ == "__main__":
    main()
