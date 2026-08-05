"""Card payments. The Pi talks to Square; the board only ever sees a URL.

    CMD SQUARE_LINK  cents= id= desc=   ->  RSP SQUARE_URL id= url=
                                        or  RSP SQUARE_ERR id= reason=
                                        then EVT SQUARE_PAID id= order=
    CMD SQUARE_CANCEL id=               ->  RSP SQUARE_CANCELLED id= ok=
                                        and possibly EVT SQUARE_LATE_PAID

WHY THE PI POLLS SQUARE (decision D12)
--------------------------------------
`square.py` already had the polling path, and its tenders check was learned the
hard way. A webhook would need the Pi reachable from the internet, which
reverses dashboard.md section 2's "local network only" for one feature. The Pi
is already awake and already polling.

WHY THERE IS A THREAD
---------------------
Every Square call is a network round trip. Done inline it would stall the
service loop, which also drains the serial link and answers the camera — and a
hung socket would become a hung fridge. So the worker thread does HTTP and
nothing else: it never touches the database, never touches the link, and
communicates only through two queues. The main loop stays single-threaded, and
`Store`'s one connection is never shared.

TWO BACKENDS, ONE INTERFACE
---------------------------
`FakeSquare` needs no credentials and no network, and is what the tests and the
default `--square fake` use. `RealSquare` imports `square.py` LAZILY — that
module reads the access token at import time, so importing it eagerly would make
`fridged` refuse to start on any machine without `tokens.py`, including every
machine that only ever wanted the simulator.
"""

import logging
import queue
import threading
import time

log = logging.getLogger("fridged.payments")

#: How often an unpaid order is re-checked. Square is not going to tell us, so
#: this is a poll; two seconds is responsive to a customer standing at the
#: terminal without being rude to the API.
POLL_INTERVAL_S = 2.0

#: Give up polling an order after this long. The board's own
#: `PAYMENT_TIMEOUT_MS` is 120 s, so it will have moved on well before this —
#: the margin exists so a late payment is still noticed and reported (D17).
POLL_GIVE_UP_S = 300.0

#: What the fake backend hands back. A real, short URL so the QR is scannable
#: off the screen during bench testing.
FAKE_URL = "https://example.com/pay/SIMULATED"
FAKE_LINK_DELAY_S = 1.0
FAKE_PAID_AFTER_S = 8.0


class FakeSquare:
    """A Square that costs nothing and needs no credentials."""

    name = "fake"

    def __init__(self, rng=None):
        self._created = {}
        self._counter = 0

    def create(self, txn_id, cents, description):
        time.sleep(FAKE_LINK_DELAY_S)       # on the worker thread, never the loop
        self._counter += 1
        order_id = f"SIM-ORDER-{self._counter}"
        self._created[order_id] = time.monotonic()
        return FAKE_URL, order_id, f"SIM-LINK-{self._counter}"

    def poll(self, order_id):
        started = self._created.get(order_id)
        return started is not None and (
            time.monotonic() - started) >= FAKE_PAID_AFTER_S

    def cancel(self, link_id):
        return True


class RealSquare:
    """The sandbox or production Square account, via `square.py`."""

    name = "real"

    def __init__(self, rng=None):
        import sys
        from pathlib import Path

        # online-payment/ is not a package and square.py is not importable from
        # anywhere else, so its directory goes on the path here — the same
        # arrangement, and the same reasoning, as tools/protocol.py.
        payments_dir = Path(__file__).resolve().parent.parent / "online-payment"
        if str(payments_dir) not in sys.path:
            sys.path.insert(0, str(payments_dir))

        # Imported HERE rather than at module scope. square.py reads
        # tokens.SQUARE_ACCESS_TOKEN while it is being imported, so an eager
        # import would stop fridged starting on any machine without credentials
        # — including every machine that only wanted `--port sim`.
        import square

        self._square = square
        log.info("Square: %s", "SANDBOX" if square.SANDBOX else "PRODUCTION")

    def create(self, txn_id, cents, description):
        # The idempotency key is the transaction id, not a timestamp: a retried
        # request must return the SAME link rather than creating a second
        # checkout for the same drinks (plan.md stage 15.2).
        link = self._square.create_payment_link(
            cents, description, f"fridge-{txn_id}")
        return link.url, link.order_id, link.payment_link_id

    def poll(self, order_id):
        return self._square.poll_once(order_id).paid

    def cancel(self, link_id):
        return self._square.cancel(link_id)


BACKENDS = {"fake": FakeSquare, "real": RealSquare}


class PaymentService:
    """Owns the backend and the worker thread. Everything else asks it politely.

    `request_*` never blocks. `drain()` returns whatever has finished since it
    was last called, as `(kind, ...)` tuples the caller turns into frames.
    """

    def __init__(self, backend):
        self.backend = backend
        self._requests = queue.Queue()
        self._results = queue.Queue()
        #: txn_id -> {order_id, link_id, cents, cancelled, deadline}
        self._open = {}
        self._lock = threading.Lock()
        self._running = True
        self._thread = threading.Thread(target=self._work, daemon=True,
                                        name="square")
        self._thread.start()

    # --- Called from the service loop ---------------------------------------

    def request_link(self, txn_id, cents, description):
        self._requests.put(("create", txn_id, cents, description))

    def request_cancel(self, txn_id):
        self._requests.put(("cancel", txn_id))

    def drain(self):
        """Everything the worker has finished. Never blocks."""
        out = []
        while True:
            try:
                out.append(self._results.get_nowait())
            except queue.Empty:
                return out

    def close(self):
        self._running = False
        self._requests.put(None)

    # --- The worker thread ---------------------------------------------------

    def _work(self):
        while self._running:
            try:
                self._serve_requests()
                self._poll_open_orders()
            except Exception:                      # noqa: BLE001
                # This thread must not die. It is the only thing answering card
                # payments, and a dead one produces a terminal that hangs on
                # "Contacting payment service" with nothing in the log.
                log.exception("payment worker error")
            time.sleep(0.1)

    def _serve_requests(self):
        try:
            request = self._requests.get(timeout=0.1)
        except queue.Empty:
            return
        if request is None:
            return

        if request[0] == "create":
            _, txn_id, cents, description = request
            try:
                url, order_id, link_id = self.backend.create(
                    txn_id, cents, description)
            except Exception as exc:               # noqa: BLE001
                log.warning("could not create a payment link: %s", exc)
                self._results.put(("error", txn_id, str(exc)[:40]))
                return
            with self._lock:
                # A cancel can arrive BEFORE the link exists — the board gives
                # up while the API call is still in flight. The cancellation is
                # remembered and applied here, which is the only way that
                # orphaned live link ever gets killed (plan.md stage 15.3).
                already = self._open.pop(txn_id, None)
                cancelled = bool(already and already.get("cancelled"))
                self._open[txn_id] = {
                    "order_id": order_id, "link_id": link_id, "cents": cents,
                    "cancelled": cancelled,
                    "deadline": time.monotonic() + POLL_GIVE_UP_S,
                    "next_poll": time.monotonic() + POLL_INTERVAL_S}
            if cancelled:
                log.info("txn %s was cancelled before its link arrived; "
                         "cancelling it now", txn_id)
                self._do_cancel(txn_id)
                return
            self._results.put(("url", txn_id, url))

        elif request[0] == "cancel":
            self._do_cancel(request[1])

    def _do_cancel(self, txn_id):
        with self._lock:
            entry = self._open.get(txn_id)
            if entry is None:
                # Nothing created yet. Remember it so the create path can apply
                # it the moment the link exists.
                self._open[txn_id] = {"cancelled": True, "order_id": None,
                                      "link_id": None, "cents": 0,
                                      "deadline": time.monotonic() + POLL_GIVE_UP_S,
                                      "next_poll": time.monotonic()}
                return
            entry["cancelled"] = True
            link_id = entry.get("link_id")

        if link_id is None:
            return
        try:
            ok = self.backend.cancel(link_id)
        except Exception as exc:                   # noqa: BLE001
            log.warning("cancel failed for txn %s: %s", txn_id, exc)
            ok = False
        self._results.put(("cancelled", txn_id, ok))

    def _poll_open_orders(self):
        now = time.monotonic()
        with self._lock:
            due = [(txn_id, dict(entry)) for txn_id, entry in self._open.items()
                   if entry.get("order_id") and entry["next_poll"] <= now]

        for txn_id, entry in due:
            with self._lock:
                if txn_id in self._open:
                    self._open[txn_id]["next_poll"] = now + POLL_INTERVAL_S

            if now > entry["deadline"]:
                with self._lock:
                    self._open.pop(txn_id, None)
                continue

            try:
                paid = self.backend.poll(entry["order_id"])
            except Exception as exc:               # noqa: BLE001
                log.debug("poll failed for txn %s: %s", txn_id, exc)
                continue
            if not paid:
                continue

            with self._lock:
                self._open.pop(txn_id, None)
            if entry["cancelled"]:
                # The customer's payment and our cancellation crossed in flight.
                # There is no automatic refund (decision D17): it is recorded as
                # money owed back, and a human deals with it.
                self._results.put(("late_paid", txn_id, entry["order_id"],
                                   entry["cents"]))
            else:
                self._results.put(("paid", txn_id, entry["order_id"]))
