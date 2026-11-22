"""Check both Shopping Sessions tables against independent public-seed digests."""
import argparse
import hashlib
import json
import os
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from time import perf_counter

import duckdb

SCHEMAS = {
    "user_daily": [
        ("user_id", "VARCHAR"), ("day", "DATE"), ("sessions", "INTEGER"),
        ("events", "INTEGER"), ("revenue_cents", "INTEGER"), ("top_category", "VARCHAR"),
    ],
    "campaign_daily": [
        ("campaign_id", "VARCHAR"), ("channel", "VARCHAR"), ("day", "DATE"),
        ("sessions", "INTEGER"), ("purchasing_sessions", "INTEGER"),
        ("revenue_cents", "INTEGER"), ("assisted_sessions", "INTEGER"),
    ],
}
INTEGERS = {
    "TINYINT", "SMALLINT", "INTEGER", "BIGINT", "HUGEINT",
    "UTINYINT", "USMALLINT", "UINTEGER", "UBIGINT", "UHUGEINT",
}


def hash_stream(stream):
    h = hashlib.sha256()
    size = 0
    while chunk := stream.read(1 << 20):
        h.update(chunk)
        size += len(chunk)
    return h.hexdigest(), size


def to_csv(db, sql):
    copy = (f"COPY ({sql}) TO ? (FORMAT CSV, HEADER FALSE, DELIMITER ',', "
            "QUOTE '\"', ESCAPE '\"', FORCE_QUOTE *, NULL '\\N', DATEFORMAT '%Y-%m-%d')")
    if Path("/proc/self/fd").is_dir():
        rd, wr = os.pipe()
        with os.fdopen(rd, "rb") as stream, ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(hash_stream, stream)
            try:
                rows = db.execute(copy, [f"/proc/self/fd/{wr}"]).fetchone()[0]
            finally:
                os.close(wr)
            digest, size = pending.result()
    else:
        with tempfile.TemporaryDirectory() as tmp:
            csv = Path(tmp) / "ordered.csv"
            rows = db.execute(copy, [str(csv)]).fetchone()[0]
            with csv.open("rb") as stream:
                digest, size = hash_stream(stream)
    return {"rows": rows, "sha256": digest, "bytes": size}


def digest_table(db, output, table):
    folder = output / table
    if not folder.is_dir() or folder.is_symlink():
        raise ValueError(f"Missing table directory {folder}")
    files = sorted(f for f in folder.glob("*.parquet")
                   if f.is_file() and not f.is_symlink() and not f.name.startswith((".", "_")))
    if not files:
        raise ValueError(f"No Parquet parts in {folder}")
    schema = dict(SCHEMAS[table])
    groups = {}
    for file in files:
        relation = db.read_parquet(str(file), hive_partitioning=False)
        actual = dict(zip(relation.columns, map(str, relation.types)))
        if len(relation.columns) != len(schema) or actual.keys() != schema.keys():
            raise ValueError(f"{file}: expected columns {list(schema)}, got {list(actual)}")
        for name, kind in schema.items():
            if actual[name] not in (INTEGERS if kind == "INTEGER" else {kind}):
                raise ValueError(f"{file}: {name} must be {kind}, found {actual[name]}")
        groups.setdefault(tuple(actual.items()), []).append(str(file))
    # Normalize before combining physical widths. Even mixed signed and unsigned
    # 64-bit values retain every digit. Null propagation is unchanged.
    cols = ", ".join(f'CAST("{c}" AS VARCHAR) AS "{c}"' if t == "INTEGER" else f'"{c}"'
                     for c, t in schema.items())
    projections = []
    for i, group in enumerate(groups.values()):
        view = f"parts_{i}"
        db.read_parquet(group, hive_partitioning=False).create_view(view, replace=True)
        projections.append(f"SELECT {cols} FROM {view}")
    query = " UNION ALL ".join(projections)
    return to_csv(db, f"SELECT * FROM ({query}) AS all_parts ORDER BY ALL ASC NULLS FIRST")


def connection():
    if duckdb.__version__ != "1.5.6":
        raise RuntimeError(f"Requires duckdb==1.5.6, found {duckdb.__version__}")
    db = duckdb.connect()
    db.execute("SET threads=8")
    db.execute("SET memory_limit='4GB'")
    db.execute("SET preserve_insertion_order=true")
    db.execute("SET default_collation='binary'")
    return db
