"""The database: the schema, and every write that goes into it.

This module knows SQL and nothing about the wire protocol. `ingest.py` knows the
wire protocol and no SQL. Keeping that line sharp is what makes a protocol change
one file and a schema change one other file (CLAUDE.md section 2).

WHY ONE PROCESS OWNS THIS FILE
------------------------------
SQLite allows one writer at a time. Having the firmware bridge, the vision
program and the payment script all write directly is how you get
`database is locked` during a demo, plus three copies of the schema drifting
apart in three languages (dashboard.md section 3.1). Everything that writes goes
through here; Grafana connects read-only.

THE SCHEMA IS COMPLETE, THE WRITERS ARE NOT
-------------------------------------------
Every table from dashboard.md section 5 is created below, including the ones
nothing writes yet. Tables are cheap and a half-built schema invites a migration
later; writer methods arrive with the stage that needs them. So an empty table
here means "stage not reached", not "forgotten".
"""

import os
import sqlite3
import stat
import time
from pathlib import Path

from . import config

#: Bumped whenever the schema below changes incompatibly. Stored in SQLite's own
#: `PRAGMA user_version`, so it costs no table and cannot be forgotten in a
#: `SELECT`. A mismatch refuses to run rather than writing rows into a shape that
#: no longer means what they say.
SCHEMA_VERSION = 2

SCHEMA = """
-- Scalars sampled over time. ONE generic table, deliberately: adding a metric
-- must never be a schema change (dashboard.md section 5.1). Drives the
-- temperature graph, the health row, the money-box tile and link health alike.
CREATE TABLE IF NOT EXISTS measurement (
    ts      REAL NOT NULL,          -- unix seconds, stamped by the Pi on arrival
    metric  TEXT NOT NULL,          -- 'temp.freezer', 'coinbox.mass_g', ...
    value   REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_measurement_metric_ts ON measurement(metric, ts);

-- One row per time the board started. Exists because a transaction id is
-- `ms_since_boot` and is therefore unique only WITHIN a boot: two reboots in a
-- day can mint the same id, and without this the second would overwrite the
-- first (dashboard.md section 5.3 amendment 1). Gives the reset counter free.
CREATE TABLE IF NOT EXISTS boot (
    id     INTEGER PRIMARY KEY AUTOINCREMENT,
    ts     REAL NOT NULL,
    fw     TEXT,
    reason TEXT                     -- 'boot_frame' | 'resumed' | 'ms_rollback'
);

-- ROM code -> which shelf the sensor is actually on. The mapping lives here and
-- NOT in board.h, so replacing a failed sensor is editing one row rather than a
-- firmware rebuild and reflash (dashboard.md section 4.3). An unknown ROM code
-- is inserted automatically with a NULL zone_label, so a newly fitted sensor
-- appears on the dashboard as an unmapped row showing a live temperature: warm
-- the one you just fitted, see which row moves, name it.
CREATE TABLE IF NOT EXISTS sensor (
    rom_code   TEXT PRIMARY KEY,
    zone_label TEXT,
    kind       TEXT NOT NULL DEFAULT 'ds18b20',
    first_seen REAL NOT NULL
);

CREATE TABLE IF NOT EXISTS door_event (
    ts         REAL NOT NULL,
    state      TEXT NOT NULL CHECK (state IN ('open', 'closed')),
    duration_s REAL                 -- filled in on close; NULL while open
);
CREATE INDEX IF NOT EXISTS ix_door_event_ts ON door_event(ts);

-- Named `txn` rather than `transaction`, which is a reserved word in SQL: every
-- Grafana panel query would have to quote it forever, and a forgotten quote is a
-- syntax error at display time rather than at write time.
CREATE TABLE IF NOT EXISTS txn (
    boot_id    INTEGER NOT NULL REFERENCES boot(id),
    txn_id     INTEGER NOT NULL,    -- ms_since_boot at TXN_START
    ts_start   REAL,
    ts_end     REAL,
    method     TEXT,                -- 'cash' | 'online'
    owed_cents  INTEGER,
    paid_cents  INTEGER,
    outcome    TEXT CHECK (outcome IS NULL OR
                           outcome IN ('paid', 'stolen', 'cancelled')),
    member_uid TEXT,                -- populated but NOT displayed; section 9
    PRIMARY KEY (boot_id, txn_id)
);
CREATE INDEX IF NOT EXISTS ix_txn_ts_start ON txn(ts_start);

CREATE TABLE IF NOT EXISTS txn_item (
    boot_id          INTEGER NOT NULL,
    txn_id           INTEGER NOT NULL,
    drink            TEXT NOT NULL,
    qty              INTEGER NOT NULL,
    unit_price_cents INTEGER,
    PRIMARY KEY (boot_id, txn_id, drink)
);

-- txn_id is ATTRIBUTED, not received: EVT COIN carries denom= and delta_g= and
-- no id= at all, whatever plan.md's table says. A coin arriving with no
-- transaction open is stored with NULL, which is itself worth being able to see.
CREATE TABLE IF NOT EXISTS coin_event (
    ts           REAL NOT NULL,
    boot_id      INTEGER,
    txn_id       INTEGER,
    denom_cents  INTEGER,           -- NULL when classified = 0
    mass_delta_g REAL,
    classified   INTEGER NOT NULL   -- 0 means it was a COIN_REJECT
);
CREATE INDEX IF NOT EXISTS ix_coin_event_ts ON coin_event(ts);

CREATE TABLE IF NOT EXISTS rfid_event (
    ts      REAL NOT NULL,
    uid     TEXT NOT NULL,
    granted INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS member (
    uid    TEXT PRIMARY KEY,
    label  TEXT,
    active INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS stock_snapshot (
    ts      REAL NOT NULL,
    drink   TEXT NOT NULL,
    count   INTEGER NOT NULL,
    trigger TEXT NOT NULL           -- 'door_close', 'manual', 'seed'
);
CREATE INDEX IF NOT EXISTS ix_stock_snapshot_ts ON stock_snapshot(ts);

CREATE TABLE IF NOT EXISTS cash_count (
    ts             REAL NOT NULL,
    counted_cents  INTEGER NOT NULL,
    expected_cents INTEGER,
    note           TEXT,
    entered_by     TEXT
);

-- Everything that came down the wire, including lines that were not frames and
-- frames that failed validation. A rising bad-frame count NEXT TO the offending
-- bytes is a five-minute debug; the counter on its own is a two-hour one.
CREATE TABLE IF NOT EXISTS raw_line (
    ts            REAL NOT NULL,
    source        TEXT NOT NULL,    -- 'board' | 'seed'
    line          TEXT NOT NULL,
    reject_reason TEXT              -- NULL for anything that parsed
);
CREATE INDEX IF NOT EXISTS ix_raw_line_ts ON raw_line(ts);

-- Temperature with its zone name resolved at READ time.
--
-- Readings are stored under the sensor's ROM code, never under its zone name,
-- and the name is attached here by a join. That is the difference between
-- naming a sensor relabelling its whole history and naming a sensor putting a
-- discontinuity in the graph at the moment you named it — and the discovery
-- procedure in dashboard.md section 4.3 is "warm the one you just fitted, watch
-- which row moves, name it", which is done by looking at history.
--
-- It also means a panel query is one SELECT with no join of its own, so the
-- join exists once, here, instead of in every panel that touches temperature.
CREATE VIEW IF NOT EXISTS temperature AS
SELECT m.ts                                              AS ts,
       s.rom_code                                        AS rom_code,
       COALESCE(s.zone_label, 'unmapped ' || SUBSTR(s.rom_code, 9)) AS zone,
       (s.zone_label IS NULL)                            AS unmapped,
       m.value                                           AS celsius
FROM measurement m
JOIN sensor s ON m.metric = 'temp.rom.' || s.rom_code;
"""


class SchemaMismatch(RuntimeError):
    """The database on disk was written by a different version of this schema."""


class Store:
    """Owns the connection and every write.

    High-volume append-only rows (`measurement`, `raw_line`) are queued and
    written in batches; anything that has to be readable immediately, or that
    hands back an id, is written straight through.
    """

    def __init__(self, path=None):
        self.path = Path(path) if path else config.DEFAULT_DB_PATH
        self._conn = None
        # {sql: [params, ...]} — a dict rather than a flat list so each table
        # gets one executemany. Insertion-ordered, so a batch replays in the
        # order the statements were first queued.
        self._pending = {}
        self._pending_rows = 0
        self._last_flush = 0.0
        self.rows_written = 0

    # --- Lifecycle ----------------------------------------------------------

    def open(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)

        # isolation_level=None turns off the driver's implicit transaction
        # handling, so every statement autocommits and flush() can open exactly
        # one explicit transaction around the whole batch. Left at its default,
        # the driver would open a transaction behind our back on the first INSERT
        # and hold it until something committed, which is precisely the
        # long-lived writer that makes a reader see stale data.
        self._conn = sqlite3.connect(str(self.path), isolation_level=None)

        # WAL: readers never block the writer and the writer never blocks
        # readers, which is what lets Grafana query the file while this process
        # is writing it. Persistent — set once, stored in the file header.
        self._conn.execute("PRAGMA journal_mode=WAL")
        # With WAL, NORMAL fsyncs at checkpoints rather than at every commit. The
        # exposure is losing the last few seconds of telemetry on a power cut,
        # against a large reduction in SD-card wear (dashboard.md section 13).
        # For a fridge log that is the right side of the trade.
        self._conn.execute("PRAGMA synchronous=NORMAL")
        self._conn.execute("PRAGMA foreign_keys=ON")
        self._conn.execute(f"PRAGMA busy_timeout={config.SQLITE_BUSY_TIMEOUT_MS}")

        self._migrate()
        self._ensure_group_writable()
        self._last_flush = time.monotonic()
        return self

    def _ensure_group_writable(self):
        """Force the database to a group-writable mode.

        SQLite creates the file 0644 and **umask can only remove permission
        bits, never add them**, so setting a umask alone does not produce a
        group-writable database — an earlier version of this code assumed it did
        and quietly did not work.

        It has to be group-writable because Grafana runs as a different user and
        SQLite cannot open a WAL database read-only: a reader takes a lock in
        the -shm wal-index, which is a write. The group comes from the setgid
        bit on the containing directory (`/var/lib/fridge`, group `grafana`),
        which is why only the mode is set here and never the owner.

        SQLite then creates -wal and -shm with *this* file's mode, so fixing the
        database fixes the sidecars too — provided the umask lets it, which is
        what `config.DB_FILE_UMASK` is for.

        Best effort: a database on a filesystem without Unix modes, or owned by
        somebody else, is not a reason to refuse to run.
        """
        if os.name != "posix":
            return
        try:
            current = stat.S_IMODE(self.path.stat().st_mode)
            # Add the group bits and leave owner and other exactly as they are —
            # this must not quietly widen or narrow anything an admin chose.
            wanted = current | (config.DB_FILE_MODE & 0o070)
            if wanted != current:
                self.path.chmod(wanted)
        except OSError:
            pass

    def _migrate(self):
        found = self._conn.execute("PRAGMA user_version").fetchone()[0]
        if found == 0:
            # Either brand new, or an existing file from before versioning. The
            # CREATE IF NOT EXISTS statements make both cases the same.
            self._conn.executescript(SCHEMA)
            self._conn.execute(f"PRAGMA user_version={SCHEMA_VERSION}")
        elif found != SCHEMA_VERSION:
            raise SchemaMismatch(
                f"{self.path} was written by schema version {found}, this is "
                f"version {SCHEMA_VERSION}. Move the old file aside or write a "
                f"migration — do not just bump the number.")

    def close(self):
        if self._conn is None:
            return
        self.flush(force=True)
        self._conn.close()
        self._conn = None

    def __enter__(self):
        return self.open()

    def __exit__(self, *exc):
        self.close()

    # --- Batching -----------------------------------------------------------

    def _queue(self, sql, params):
        self._pending.setdefault(sql, []).append(params)
        self._pending_rows += 1

    def flush(self, force=False):
        """Write queued rows if enough have accumulated or enough time has passed.

        Both bounds matter. The row bound keeps a busy period from queueing
        without limit; the time bound keeps an idle fridge's trickle of
        heartbeats from sitting unwritten, invisible to the dashboard, for as
        long as it takes to reach 200 rows.
        """
        if not self._pending:
            self._last_flush = time.monotonic()
            return 0
        if not force:
            due = (self._pending_rows >= config.FLUSH_ROWS or
                   time.monotonic() - self._last_flush >= config.FLUSH_INTERVAL_S)
            if not due:
                return 0

        batch, self._pending = self._pending, {}
        written, self._pending_rows = self._pending_rows, 0

        self._conn.execute("BEGIN")
        try:
            for sql, rows in batch.items():
                self._conn.executemany(sql, rows)
        except Exception:
            self._conn.execute("ROLLBACK")
            raise
        self._conn.execute("COMMIT")

        self._last_flush = time.monotonic()
        self.rows_written += written
        return written

    # --- Writers: batched ---------------------------------------------------

    def measurement(self, ts, metric, value):
        self._queue("INSERT INTO measurement (ts, metric, value) VALUES (?,?,?)",
                    (ts, metric, float(value)))

    def raw_line(self, ts, line, source="board", reject_reason=None):
        self._queue(
            "INSERT INTO raw_line (ts, source, line, reject_reason) "
            "VALUES (?,?,?,?)", (ts, source, line, reject_reason))

    # --- Writers: immediate -------------------------------------------------

    def open_boot(self, ts, fw=None, reason="boot_frame"):
        """Record that the board started, and return the id everything else uses.

        Immediate rather than batched: the id is needed by the very next row, so
        there is nothing to gain by deferring it and a foreign key to break by
        doing so.
        """
        cur = self._conn.execute(
            "INSERT INTO boot (ts, fw, reason) VALUES (?,?,?)", (ts, fw, reason))
        self.rows_written += 1
        return cur.lastrowid

    def note_sensor(self, rom_code, ts, kind="ds18b20"):
        """Make sure a ROM code has a row, without disturbing its zone label.

        `ON CONFLICT DO NOTHING` is the whole trick: a sensor seen for the
        thousandth time must not have the name someone gave it overwritten with
        a NULL.
        """
        self._conn.execute(
            "INSERT INTO sensor (rom_code, zone_label, kind, first_seen) "
            "VALUES (?, NULL, ?, ?) ON CONFLICT(rom_code) DO NOTHING",
            (rom_code, kind, ts))

    def zone_for_rom(self, rom_code):
        """The zone label for a ROM code, or None if it has not been named yet."""
        row = self._conn.execute(
            "SELECT zone_label FROM sensor WHERE rom_code = ?",
            (rom_code,)).fetchone()
        return row[0] if row else None
