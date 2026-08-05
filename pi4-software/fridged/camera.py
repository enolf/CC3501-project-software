"""What is on the shelf. The Pi's answer to `CMD SCAN`.

THIS IS THE ONE THING THAT FLOWS THE OTHER WAY
-----------------------------------------------
Every other metric goes board -> Pi. Stock does not: the board asks
(`CMD SCAN`) and the Pi answers (`EVT INV`), because the camera is the source
of truth for what is on the shelf and the board has no way of knowing
(dashboard.md section 6.3). The board then diffs two answers — one from when
the door opened, one from when it shut — and *that* is the basket.

The consequence worth stating: `stock_snapshot` rows are written by `fridged`
when it ANSWERS a scan. The board never reports stock and never could.

WHY THE SHELF IS SELF-CORRECTING
--------------------------------
Counts are absolute, never differences. A restock needs no manual entry and no
message of its own: the next scan simply reports more drinks, and the shelf is
right again. A missed or corrupted frame costs one stale reading rather than
permanently offsetting the count (dashboard.md section 2).

Two backends, one interface — the same shape as `pi_link` on the firmware side:

    SimCamera       a modelled shelf, for development
    PiCapture       the real thing, stage D8. Not written yet.
"""

import logging
import random

log = logging.getLogger("fridged.camera")

#: Drink keys as they appear on the wire. The firmware builds these by
#: lowercasing `catalogue::name()`, so they must match that table exactly —
#: see the INV handler in pi_link_serial.cpp.
DRINKS = ("coke", "sprite", "fanta", "pasito")

#: Display names, for `txn_item` and the panels. Same order.
DISPLAY = {"coke": "Coke", "sprite": "Sprite", "fanta": "Fanta",
           "pasito": "Pasito"}

#: How many of each drink a full shelf holds.
SHELF_CAPACITY = 6

#: Restock when the total drops to this fraction of a full shelf. Somebody
#: notices it is getting empty and refills it, which is the event that makes
#: the camera's self-correction visible on the dashboard.
RESTOCK_AT = 0.3


class SimCamera:
    """A modelled shelf that customers take drinks from.

    Deliberately owns the shelf rather than being told what was taken. In the
    real system the camera *observes* a physical world it does not control, and
    the board learns what happened by diffing two observations. Letting the
    simulated board decide the basket and telling the camera afterwards would
    invert that and leave the diff logic untested.
    """

    def __init__(self, rng=None, capacity=SHELF_CAPACITY):
        self.rng = rng or random.Random()
        self.capacity = capacity
        self.shelf = {drink: capacity for drink in DRINKS}
        self.restocks = 0
        self.scans = 0

    # --- The physical world -------------------------------------------------

    def customer_takes(self):
        """Somebody opened the door. Decide what, if anything, left the shelf.

        Returns what was removed, which the caller does not need but the tests
        do. The board is not told: it finds out by scanning.
        """
        if self.rng.random() >= 0.55:
            return {}           # looked, changed their mind

        roll = self.rng.random()
        count = 1 if roll < 0.72 else (2 if roll < 0.94 else 3)

        taken = {}
        available = [d for d in DRINKS if self.shelf[d] > 0]
        for _ in range(count):
            if not available:
                break
            drink = self.rng.choice(available)
            self.shelf[drink] -= 1
            taken[drink] = taken.get(drink, 0) + 1
            available = [d for d in DRINKS if self.shelf[d] > 0]
        return taken

    def maybe_restock(self):
        """Refill if it is looking empty. Returns True if it happened.

        No message announces this and none is needed — the next scan reports the
        higher counts. That is the whole argument for making the camera the
        source of truth (dashboard.md section 2).
        """
        total = sum(self.shelf.values())
        if total > self.capacity * len(DRINKS) * RESTOCK_AT:
            return False
        self.shelf = {drink: self.capacity for drink in DRINKS}
        self.restocks += 1
        log.info("shelf restocked")
        return True

    # --- What the board asks for --------------------------------------------

    def scan(self):
        """Answer a `CMD SCAN`: absolute counts plus a confidence figure.

        `conf` is the vision system's own reliability signal (dashboard.md
        section 7). The real CV is the least trustworthy subsystem in the
        project, so the number exists to be graphed; here it wanders a little
        rather than sitting at 100, so a panel watching it has something to
        show.
        """
        self.scans += 1
        confidence = max(60, min(100, int(self.rng.gauss(95, 6))))
        return dict(self.shelf), confidence

    def payload(self):
        """The `EVT INV` payload: `coke=5 sprite=4 ... conf=98`."""
        counts, confidence = self.scan()
        pairs = " ".join(f"{drink}={counts[drink]}" for drink in DRINKS)
        return f"{pairs} conf={confidence}", counts
