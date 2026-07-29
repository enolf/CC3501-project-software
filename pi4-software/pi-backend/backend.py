import time
import serial
import threading
import sys

if sys.platform.startswith('win'):
    PICO_PORT = 'COM3' 
else:
    PICO_PORT = '/dev/ttyACM0' 

BAUD_RATE = 115200

# Global flags
ser = None
camera_running = False

def mock_camera_stream():
    """Simulates the Pi Camera output over serial at 4Hz."""
    global camera_running
    
    # Define the required sequences: (String, Repetitions)
    # \n is added at the end so the RP2040 buffer detects the end of the line
    sequences = [
        ("C:4,F:2,P:3,S:1;\n", 20), # Initial
        ("C:4,F:1,P:3,S:1;\n", 20), # 1 Fanta taken
        ("C:4,F:2,P:3,S:0;\n", 20)  # Fanta returned, 1 Solo taken
    ]
    
    for seq_string, count in sequences:
        for _ in range(count):
            if not camera_running:
                return # Exit immediately if door closed early
            
            ser.write(seq_string.encode('utf-8'))
            ser.flush()
            time.sleep(0.25) # 4 strings per second
            
    # Final state: loop endlessly until the door closes
    while camera_running:
        ser.write("C:4,F:2,P:3,S:1;\n".encode('utf-8'))
        ser.flush()
        time.sleep(0.25)

def main():
    global ser, camera_running
    
    try:
        ser = serial.Serial(PICO_PORT, BAUD_RATE, timeout=0.1)
        print(f"Connected to RP2040 on {PICO_PORT}. Waiting for door events...")
    except Exception as e:
        print(f"Serial Error: {e}")
        return

    while True:
        try:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8').strip()
                
                # --- Handle Door Events ---
                if line == "picam 1":
                    print("Door Opened: Starting Mock Camera Stream...")
                    camera_running = True
                    # Launch the camera loop in a separate thread so we can keep listening
                    threading.Thread(target=mock_camera_stream, daemon=True).start()
                    
                elif line == "picam 0":
                    print("Door Closed: Stopping Camera Stream...")
                    camera_running = False
                
                # --- Handle Payments ---
                elif line.startswith("CHARGE:"):
                    amount = line.split(":")[1]
                    print(f"RP2040 requested Square charge for ${amount}")
                    # ... [Insert your existing Square API call here] ...
                    
        except Exception:
            pass
            
        time.sleep(0.05)

if __name__ == '__main__':
    main()