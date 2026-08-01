"""Square payments for the fridge.

A MODULE, not a script. `fridged` imports it; the `__main__` block at the bottom
still runs it standalone for exactly the kind of bench testing that proved the
sandbox account works in the first place.

    import square
    link = square.create_payment_link(600, "2xCoke,1xFanta", "fridge-99")
    ...
    status = square.poll_once(link.order_id)
    if status.paid: ...
    square.cancel(link.payment_link_id)

WHY NOTHING HERE BLOCKS FOR LONG
--------------------------------
The original version polled in a `while` loop for up to two minutes. `fridged`
cannot do that: it also owns the serial link to the RP2040 and the camera
subprocess, and a two-minute nap would stall both. So `poll_once()` makes ONE
request and returns, and the caller decides how often to ask. Every HTTP call
has a timeout for the same reason -- a hung socket must not become a hung fridge.

    pip install requests qrcode[pil]
"""

import logging
import time
from dataclasses import dataclass
from typing import Optional

import requests

import tokens

log = logging.getLogger(__name__)


# --- Environment ------------------------------------------------------------

# Sandbox is Square's test environment: play money, published card numbers,
# nothing that can move real funds. Switching to production needs THREE changes,
# not one -- this flag, plus a production access token and location id in
# tokens.py. Getting one of the three wrong fails in confusing ways, so they are
# listed here together.
SANDBOX = True

SANDBOX_BASE_URL = "https://connect.squareupsandbox.com/v2"
PRODUCTION_BASE_URL = "https://connect.squareup.com/v2"
BASE_URL = SANDBOX_BASE_URL if SANDBOX else PRODUCTION_BASE_URL

# Confirmed against the account: the one location on it is AUD.
CURRENCY = "AUD"

# Pinned deliberately. Square's API is versioned by this header, and letting it
# float means a change at their end can alter behaviour here with no code
# change and no warning.
SQUARE_VERSION = "2024-07-17"

HEADERS = {
    "Square-Version": SQUARE_VERSION,
    "Authorization": f"Bearer {tokens.SQUARE_ACCESS_TOKEN}",
    "Content-Type": "application/json",
}

# Generous enough for a slow mobile connection, short enough that a dead network
# is noticed rather than waited on.
HTTP_TIMEOUT_S = 15

# Square rejects an over-long quick_pay name; the description is only ever a
# short item list, so this is a guard rather than a real constraint.
MAX_DESCRIPTION = 120


class SquareError(RuntimeError):
    """Any failure talking to Square: network, HTTP status, or bad response.

    One exception type on purpose. Every caller does the same thing with all of
    them -- tell the board the payment link could not be made -- so splitting
    them into a hierarchy would add ceremony without changing any behaviour.
    """


@dataclass
class PaymentLink:
    """What Square hands back when a checkout is created."""

    url: str               #: Short checkout URL. This is the one the QR encodes
    order_id: str          #: Used to poll for payment
    payment_link_id: str   #: Used to CANCEL. Easy to miss and easy to need


@dataclass
class OrderStatus:
    """A single look at an order."""

    paid: bool
    state: str             #: Square's own state. See the note in poll_once()
    cents: int             #: Total on the order
    tender_count: int


def _call(method: str, path: str, payload: Optional[dict] = None) -> dict:
    """One HTTP request, with the error handling every caller would repeat."""
    url = f"{BASE_URL}{path}"
    try:
        response = requests.request(method, url, headers=HEADERS, json=payload,
                                    timeout=HTTP_TIMEOUT_S)
    except requests.RequestException as exc:
        raise SquareError(f"{method} {path} failed: {exc}") from exc

    if response.status_code != 200:
        raise SquareError(f"{method} {path} returned {response.status_code}: "
                          f"{response.text[:300]}")

    try:
        return response.json()
    except ValueError as exc:
        raise SquareError(f"{method} {path} returned unparseable JSON") from exc


def create_payment_link(amount_cents: int, description: str,
                        idempotency_key: str) -> PaymentLink:
    """Create a checkout for `amount_cents`, described as `description`.

    `description` becomes the line item on the customer's receipt, so it should
    say what they actually bought ("2xCoke,1xFanta") rather than something
    generic.

    `idempotency_key` should be derived from the transaction id, NOT from the
    clock. If a request is retried after a timeout, the same key returns the
    same link; a timestamp would create a second checkout for a drink that
    already has one, and the customer could pay the wrong one.

    Raises SquareError on any failure.
    """
    if amount_cents <= 0:
        raise SquareError(f"refusing to create a link for {amount_cents} cents")

    data = _call("POST", "/online-checkout/payment-links", {
        "idempotency_key": idempotency_key,
        "quick_pay": {
            "name": (description or "Drink")[:MAX_DESCRIPTION],
            "price_money": {"amount": amount_cents, "currency": CURRENCY},
            "location_id": tokens.LOCATION_ID,
        },
    })

    try:
        link = data["payment_link"]
        result = PaymentLink(url=link["url"],
                             order_id=link["order_id"],
                             payment_link_id=link["id"])
    except KeyError as exc:
        raise SquareError(f"payment link response missing {exc}") from exc

    log.info("created payment link %s for %d cents (order %s)",
             result.payment_link_id, amount_cents, result.order_id)
    return result


def poll_once(order_id: str) -> OrderStatus:
    """Look at an order once. Returns immediately; never loops.

    PAYMENT IS DECIDED BY THE TENDERS ARRAY, NOT BY THE ORDER STATE. This is not
    a stylistic preference and it must not be "simplified" back to checking
    `state`. A fully paid sandbox order reads:

        state: OPEN, tenders: 1, total_money: 500 AUD

    -- so a state check concludes "not paid" for an order that has the money.
    The presence of a tender means a payment is genuinely attached.

    Raises SquareError if the order cannot be read.
    """
    data = _call("GET", f"/orders/{order_id}")

    order = data.get("order")
    if order is None:
        raise SquareError(f"order {order_id} missing from response")

    tenders = order.get("tenders", [])
    return OrderStatus(
        paid=len(tenders) > 0,
        state=order.get("state", "UNKNOWN"),
        cents=order.get("total_money", {}).get("amount", 0),
        tender_count=len(tenders),
    )


def cancel(payment_link_id: str) -> bool:
    """Kill a checkout the customer has walked away from.

    ONE call does everything. Deleting the payment link also cancels the order
    behind it -- the response carries `cancelled_order_id`, the order moves to
    CANCELED, and the checkout URL starts returning 404. A follow-up UpdateOrder
    is not just unnecessary, it FAILS with VERSION_MISMATCH, because the delete
    has already bumped the order's version.

    Returns True only if a link was ACTUALLY cancelled. Never raises: a
    cancellation that fails must not take down the caller, which by this point
    has already sent the customer back to choose cash.

    The status code is not enough to judge this by. Square answers 200 even for
    a payment link id that has never existed, so a wrong id -- a bug in our own
    plumbing -- would otherwise be logged as a successful cancellation while the
    real link stayed live and payable. The response body is what distinguishes
    them: a genuine cancellation carries `cancelled_order_id`, a no-op does not.
    """
    try:
        data = _call("DELETE", f"/online-checkout/payment-links/{payment_link_id}")
    except SquareError as exc:
        # Worth a warning rather than an exception. The window this closes is
        # already small, and the customer is not waiting on the answer.
        log.warning("could not cancel payment link %s: %s", payment_link_id, exc)
        return False

    cancelled_order = data.get("cancelled_order_id")
    if not cancelled_order:
        log.warning("cancel of %s cancelled nothing - is that id right?",
                    payment_link_id)
        return False

    log.info("cancelled payment link %s (order %s)",
             payment_link_id, cancelled_order)
    return True


# --- Convenience, for standalone use ----------------------------------------
# Not used by fridged: both of these block, which is exactly what the service
# path must not do. They exist so this file is still directly runnable for bench
# testing, which is how the sandbox account was verified in the first place.

def poll_for_payment(order_id: str, timeout_seconds: int = 120,
                     poll_interval: int = 3) -> bool:
    """BLOCKING. Poll until paid or the timeout expires."""
    deadline = time.time() + timeout_seconds

    while time.time() < deadline:
        try:
            status = poll_once(order_id)
        except SquareError as exc:
            print(f"  API error: {exc}")
            time.sleep(poll_interval)
            continue

        elapsed = int(timeout_seconds - (deadline - time.time()))
        print(f"[{elapsed}s] state={status.state} tenders={status.tender_count}")

        if status.paid:
            print(f"\nSUCCESS: {status.cents} {CURRENCY} received.")
            return True

        time.sleep(poll_interval)

    print("\nTIMEOUT: no payment received.")
    return False


def display_qr_code(url: str) -> None:
    """BLOCKING-ish. Open a QR for `url` in the desktop image viewer.

    The RP2040 renders its own QR from the URL string, so this plays no part in
    the real system. Kept for testing this file on a desktop.
    """
    import qrcode

    qr = qrcode.QRCode(box_size=10, border=4)
    qr.add_data(url)
    qr.make(fit=True)
    qr.make_image(fill_color="black", back_color="white").show()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="  %(levelname)s: %(message)s")

    print(f"Square {'SANDBOX' if SANDBOX else 'PRODUCTION'} - {BASE_URL}")

    link = create_payment_link(500, "Bench test drink",
                               f"bench-{int(time.time())}")
    print(f"\n  url      : {link.url}")
    print(f"  order    : {link.order_id}")
    print(f"  link id  : {link.payment_link_id}")

    display_qr_code(link.url)

    if not poll_for_payment(link.order_id):
        print("\nCancelling the unpaid link ...")
        cancel(link.payment_link_id)

    input("\nPress Enter to close...")
