import time
import requests
import serial
import threading
import tokens # Ensure your tokens.py file is in the same directory

import sys

# --- Serial Configuration ---
# Automatically detect if we are testing on Windows or running on the Pi
if sys.platform.startswith('win'):
    PICO_PORT = 'COM9'  # <--- Change this to match your Device Manager!
else:
    PICO_PORT = '/dev/ttyACM0' # The Raspberry Pi port

BAUD_RATE = 115200

# --- Square Configuration ---
BASE_URL = "https://connect.squareupsandbox.com/v2"

HEADERS = {
    "Square-Version": "2024-07-17",
    "Authorization": f"Bearer {tokens.SQUARE_ACCESS_TOKEN}",
    "Content-Type": "application/json"
}

# Global tracking variables
current_order_id = None
ser = None

def create_payment_link(amount_dollars_str):
    """Asks Square to generate a checkout URL and an Order ID dynamically."""
    print(f"Generating Square Payment Link for ${amount_dollars_str}...")
    
    # Square expects the amount in cents as an integer
    amount_cents = int(float(amount_dollars_str) * 100)
    
    url = f"{BASE_URL}/online-checkout/payment-links"
    
    payload = {
        "idempotency_key": str(time.time()), 
        "quick_pay": {
            "name": "Smart Fridge Purchase",
            "price_money": {
                "amount": amount_cents, 
                "currency": "AUD"
            },
            "location_id": tokens.LOCATION_ID
        }
    }
    
    response = requests.post(url, headers=HEADERS, json=payload)
    
    if response.status_code == 200:
        data = response.json()
        checkout_url = data["payment_link"]["url"]
        order_id = data["payment_link"]["order_id"]
        return checkout_url, order_id
    else:
        print(f"Failed to create link: {response.text}")
        return None, None

def monitor_square_payment():
    """Background thread that polls Square while an order is active."""
    global current_order_id
    
    while True:
        if current_order_id:
            url = f"{BASE_URL}/orders/{current_order_id}"
            response = requests.get(url, headers=HEADERS)
            
            if response.status_code == 200:
                data = response.json()
                
                # Check if a successful payment (tender) was attached to the order
                tenders = data.get("order", {}).get("tenders", [])
                
                if len(tenders) > 0:
                    print("\nSUCCESS: Payment cleared! Pushing STATUS:PAID to RP2040.")
                    
                    if ser:
                        # Send the exact string the Pico is waiting for
                        ser.write(b"STATUS:PAID\n") 
                    
                    # Clear the order ID to stop polling and wait for the next customer
                    current_order_id = None 
            else:
                print(f"API Error during status check: {response.status_code}")

        # Wait 2 seconds before checking the API again to avoid rate limits
        time.sleep(2) 

# --- Main Execution ---
if __name__ == "__main__":
    if tokens.SQUARE_ACCESS_TOKEN == "YOUR_SANDBOX_ACCESS_TOKEN":
        print("WARNING: Please insert your Square Sandbox credentials into tokens.py before running!")
        exit()
        
    # Keep trying to connect until successful
    while True:
        try:
            ser = serial.Serial(PICO_PORT, BAUD_RATE, timeout=0.1)
            print(f"Connected to RP2040 on {PICO_PORT}. Listening for commands...")
            break # Break out of the loop once connected
        except Exception as e:
            print(f"Waiting for RP2040 to connect... ({e})")
            time.sleep(2) # Wait 2 seconds and try again

    # Start the background Square polling thread
    checker_thread = threading.Thread(target=monitor_square_payment, daemon=True)
    checker_thread.start()

    # The main loop purely handles listening to the RP2040
    while True:
        try:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8').strip()
                
                if line.startswith("CHARGE:"):
                    amount_str = line.split(":")[1]
                    checkout_url, order_id = create_payment_link(amount_str)
                    
                    if checkout_url and order_id:
                        current_order_id = order_id 
                        
                        # --- THE FIX: Use strict \r\n for Windows USB ---
                        response = f"URL:{checkout_url}\r\n"
                        ser.write(response.encode('utf-8'))
                        ser.flush() 
                        
                        print(f"Sent URL to RP2040: {checkout_url}")
                        
                elif line:
                    # --- THE FIX: Print anything else the Pico says! ---
                    print(f"[PICO LOG]: {line}")
                        
        except Exception as e:
            pass
            
        time.sleep(0.05)