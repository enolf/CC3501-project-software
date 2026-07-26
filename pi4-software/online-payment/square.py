import time
import requests
import qrcode
import tokens

# required
# pip install requests qrcode[pil]

# --- Configuration ---
BASE_URL = "https://connect.squareupsandbox.com/v2" # Using Sandbox environment

PRICE = 500 # $5.00 (in cents)

HEADERS = {
    "Square-Version": "2024-07-17",
    "Authorization": f"Bearer {tokens.SQUARE_ACCESS_TOKEN}",
    "Content-Type": "application/json"
}

def create_payment_link():
    """Asks Square to generate a checkout URL and an Order ID."""
    print("Generating Square Payment Link...")
    
    url = f"{BASE_URL}/online-checkout/payment-links"
    
    payload = {
        "idempotency_key": str(time.time()), # Unique key to prevent duplicates
        "quick_pay": {
            "name": "Test Drink",
            "price_money": {
                "amount": PRICE, 
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

def display_qr_code(url):
    """Generates a QR code and opens it natively on Windows."""
    print("Opening QR Code on screen...")
    qr = qrcode.QRCode(box_size=10, border=4)
    qr.add_data(url)
    qr.make(fit=True)
    
    img = qr.make_image(fill_color="black", back_color="white")
    
    # This will automatically open the image using the default Windows Photos app
    img.show() 

def poll_for_payment(order_id, timeout_seconds=120, poll_interval=3):
    """Polls the Sandbox API until the order is PAID or time runs out."""
    print(f"\nWaiting for payment on Order: {order_id}")
    print(f"Timeout set for {timeout_seconds} seconds. Scan the QR code to simulate payment!\n")
    
    url = f"{BASE_URL}/orders/{order_id}"
    start_time = time.time()
    
    while (time.time() - start_time) < timeout_seconds:
        response = requests.get(url, headers=HEADERS)
        
        if response.status_code == 200:
            data = response.json()
            if "order" in data:
                # 1. Grab the state just to see it in the terminal
                state = data["order"].get("state")
                
                # 2. The fix: Check if a successful payment (tender) was attached to the order
                tenders = data["order"].get("tenders", [])
                
                print(f"[{int(time.time() - start_time)}s] Order State: {state} | Payments Attached: {len(tenders)}")
                
                # If the tenders array has at least one item, the drink is paid for!
                if len(tenders) > 0:
                    print("\nSUCCESS: Payment cleared! Drink authorized.")
                    return True
        else:
            print(f"API Error: {response.status_code}")

        time.sleep(poll_interval)
        
    print("\nTIMEOUT: No payment received. Drink flagged as stolen.")
    return False

# --- Main Execution ---
if __name__ == "__main__":
    if tokens.SQUARE_ACCESS_TOKEN == "YOUR_SANDBOX_ACCESS_TOKEN":
        print("WARNING: Please insert your Square Sandbox credentials before running!")
    else:
        # 1. Get the link and ID
        checkout_url, order_id = create_payment_link()
        
        if checkout_url and order_id:
            # 2. Show the QR code to the user (pops up on Windows)
            display_qr_code(checkout_url)
            
            # 3. Enter the waiting loop
            poll_for_payment(order_id)

            input("\nPress Enter to close the window...")