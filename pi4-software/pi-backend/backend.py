import sys
import time
import serial
import threading

# Import our custom Square module from the 'api' subfolder
from api import square 

# --- Serial Configuration ---
if sys.platform.startswith('win'):
    PICO_PORT = 'COM9' 
else:
    PICO_PORT = '/dev/ttyACM0' 

BAUD_RATE = 115200

# Global tracking variables
current_order_id = None
ser = None
camera_running = False

# --- Hardware & API Threads ---

def monitor_square_payment():
    """Background thread that polls Square while an order is active."""
    global current_order_id
    
    while True:
        if current_order_id:
            # Call the stateless function from square.py
            is_paid = square.check_payment_status(current_order_id)
            
            if is_paid:
                print("\nSUCCESS: Payment cleared! Pushing STATUS:PAID to RP2040.")
                if ser:
                    ser.write(b"STATUS:PAID\n") 
                    ser.flush()
                # Clear the order ID to stop polling
                current_order_id = None 
                
        time.sleep(2) 

def mock_camera_stream():
    """Simulates the Pi Camera output over serial at 4Hz."""
    global camera_running
    sequences = [
        ("C:4,F:2,P:3,S:1;\n", 20),
        ("C:4,F:1,P:3,S:1;\n", 20),
        ("C:4,F:2,P:3,S:0;\n", 20)
    ]
    
    for seq_string, count in sequences:
        for _ in range(count):
            if not camera_running: return 
            ser.write(seq_string.encode('utf-8'))
            ser.flush()
            time.sleep(0.25) 
            
    while camera_running:
        ser.write("C:4,F:2,P:3,S:1;\n".encode('utf-8'))
        ser.flush()
        time.sleep(0.25)

# --- Main Execution ---

if __name__ == "__main__":
    while True:
        try:
            ser = serial.Serial(PICO_PORT, BAUD_RATE, timeout=0.1)
            print(f"Connected to RP2040 on {PICO_PORT}. Listening for commands...")
            break 
        except Exception as e:
            print(f"Waiting for RP2040 to connect... ({e})")
            time.sleep(2) 

    # Start the background Square polling thread
    threading.Thread(target=monitor_square_payment, daemon=True).start()

    # The main loop purely handles listening to the RP2040
    while True:
        try:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8').strip()
                
                # 1. Handle Door Open
                if line == "picam 1":
                    print("Door Opened: Starting Mock Camera Stream...")
                    camera_running = True
                    threading.Thread(target=mock_camera_stream, daemon=True).start()
                    
                # 2. Handle Door Close
                elif line == "picam 0":
                    print("Door Closed: Stopping Camera Stream...")
                    camera_running = False
                
                # 3. Handle Payment Request
                elif line.startswith("CHARGE:"):
                    amount_str = line.split(":")[1]
                    
                    # Call the stateless function from square.py
                    checkout_url, order_id = square.create_payment_link(amount_str)
                    
                    if checkout_url and order_id:
                        current_order_id = order_id 
                        response = f"URL:{checkout_url}\r\n"
                        ser.write(response.encode('utf-8'))
                        ser.flush() 
                        print(f"Sent URL to RP2040: {checkout_url}")
                        
                # 4. Print logs from Pico
                elif line:
                    print(f"[PICO LOG]: {line}")
                        
        except Exception as e:
            pass
            
        time.sleep(0.05)