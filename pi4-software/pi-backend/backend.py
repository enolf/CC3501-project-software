import sys
import time
import serial
import threading
import subprocess
import traceback

# Import Square module from the 'api' subfolder
from api import square 

# ==========================================
# --- CONFIGURATION ---
# ==========================================
# Set to True to use the actual Pi Camera executable, False for mock testing
USE_REAL_CAMERA = False 

# --- MOCK DATA ----
# each line represents 5 seconds of choosing drinks
mock_sequence_data = [
        ("C:4,F:2,P:3,S:1;\n", 20),
        ("C:4,F:1,P:1,S:1;\n", 20), # 1 fanta and 2 passitos taken, 
        ("C:4,F:2,P:3,S:0;\n", 20) # 1 fanta and 2 passitos returned, 1 solo taken
    ]
# the final state when mock customer is finished choosing drinks
mock_final_data = "C:3,F:2,P:2,S:1;\n" # solo is returned 1 coke and 1 passito taken
# final state would show 1 coke and 1 passito taken

# --- PI CAMERA ---
# If USE_REAL_CAMERA is True, point this to the compiled C++ executable
CAMERA_EXEC_PATH = "./your_cpp_camera_executable" 
# ==========================================

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
camera_process = None # Tracks the C++ executable process

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
    
    for seq_string, count in mock_sequence_data:
        for _ in range(count):
            if not camera_running: return 
            ser.write(seq_string.encode('utf-8'))
            ser.flush()
            time.sleep(0.25) 
            
    while camera_running:
        ser.write(mock_final_data.encode('utf-8'))
        ser.flush()
        time.sleep(0.25)

def forward_real_camera_data():
    """Reads standard output from the C++ camera executable and forwards it to Serial."""
    global camera_process, ser
    
    if camera_process is None:
        return
        
    try:
        # Continuously read lines as the C++ program prints them
        for line in camera_process.stdout:
            # Clean it and ensure it ends with exactly one newline for the RP2040
            clean_line = line.strip() + "\n"
            ser.write(clean_line.encode('utf-8'))
            ser.flush()
    except Exception as e:
        print(f"Real Camera Process Error: {e}")

# --- Main Execution ---

if __name__ == "__main__":
    try:
        # 1. Connect to RP2040
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
                        if USE_REAL_CAMERA:
                            print(f"Door Opened: Starting Real Pi Camera ({CAMERA_EXEC_PATH})...")
                            # Boot the C++ executable and capture its output
                            camera_process = subprocess.Popen(
                                [CAMERA_EXEC_PATH],
                                stdout=subprocess.PIPE,
                                text=True
                            )
                            # Start thread to read the C++ output and forward to RP2040
                            threading.Thread(target=forward_real_camera_data, daemon=True).start()
                        else:
                            print("Door Opened: Starting Mock Camera Stream...")
                            camera_running = True
                            threading.Thread(target=mock_camera_stream, daemon=True).start()
                        
                    # 2. Handle Door Close
                    elif line == "picam 0":
                        if USE_REAL_CAMERA:
                            print("Door Closed: Stopping Real Pi Camera...")
                            if camera_process:
                                camera_process.terminate()
                                camera_process = None
                        else:
                            print("Door Closed: Stopping Mock Camera Stream...")
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
                print(f"Runtime Warning: {e}")
                
            time.sleep(0.05)

    except Exception as e:
        # If a fatal crash happens anywhere in the script, catch it here
        print("\n=== FATAL ERROR OCCURRED ===")
        traceback.print_exc()
        print("============================\n")
        input("Press Enter to close this window...")