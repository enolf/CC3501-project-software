import time
import requests
from api import tokens

# --- Square Configuration ---
BASE_URL = "https://connect.squareupsandbox.com/v2"

HEADERS = {
    "Square-Version": "2024-07-17",
    "Authorization": f"Bearer {tokens.SQUARE_ACCESS_TOKEN}",
    "Content-Type": "application/json"
}

def create_payment_link(amount_dollars_str):
    """Asks Square to generate a checkout URL and an Order ID dynamically."""
    print(f"Generating Square Payment Link for ${amount_dollars_str}...")
    
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

def check_payment_status(order_id):
    """Checks if a specific order has been paid. Returns True if paid, False otherwise."""
    url = f"{BASE_URL}/orders/{order_id}"
    response = requests.get(url, headers=HEADERS)
    
    if response.status_code == 200:
        data = response.json()
        
        # Check if a successful payment (tender) was attached to the order
        tenders = data.get("order", {}).get("tenders", [])
        
        if len(tenders) > 0:
            return True # Payment cleared!
    else:
        print(f"API Error during status check: {response.status_code}")

    return False # Not paid yet or error