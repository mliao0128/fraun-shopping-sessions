#!/usr/bin/env python3
"""Independent checker for datasets produced by the official Shopping Sessions generator.

Install the tested dependency with python -m pip install duckdb==2.0.1.
Takes a generator dataset, an output directory, and a new scratch directory.
The candidate can contain the published CSV answers or the scored Parquet folders.
The scratch directory retains the independent answers in oracle.duckdb.
"""

import argparse
import csv
import json
import re
import sys
import time
from pathlib import Path

import duckdb


OUTPUT_TYPES = {
    "user_daily": {
        "user_id": "VARCHAR", "day": "DATE", "sessions": "BIGINT", "events": "BIGINT",
        "revenue_cents": "BIGINT", "top_category": "VARCHAR",
    },
    "campaign_daily": {
        "campaign_id": "VARCHAR", "channel": "VARCHAR", "day": "DATE", "sessions": "BIGINT",
        "purchasing_sessions": "BIGINT", "revenue_cents": "BIGINT", "assisted_sessions": "BIGINT",
    },
}
INTEGER_TYPES = {
    "TINYINT", "SMALLINT", "INTEGER", "BIGINT", "HUGEINT",
    "UTINYINT", "USMALLINT", "UINTEGER", "UBIGINT", "UHUGEINT",
}


def sql_string(value):
    return "'" + str(value).replace("'", "''") + "'"


def path_list(paths):
    return "[" + ", ".join(sql_string(path.as_posix()) for path in paths) + "]"


def write_aliases(data, destination):
    parents = {}

    def find(key):
        root = parents.setdefault(key, key)
        while root != parents[root]:
            parents[root] = parents[parents[root]]
            root = parents[root]
        parents[key] = root
        return root

    pair_pattern = re.compile(rb'("[^",]*"|[^",]*),("[^",]*"|[^",]*)')
    for path in (data / "links").glob("*.csv"):
        with path.open("rb") as stream:
            next(stream)
            for line in stream:
                match = pair_pattern.fullmatch(line.removesuffix(b"\n"))
                if not match:
                    continue
                first, second = (field.strip(b'"') for field in match.groups())
                if first and second:
                    first, second = find(first), find(second)
                    parents[max(first, second)] = min(first, second)
    with destination.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("alias", "person"))
        for key in parents:
            writer.writerow((key.decode(), find(key).decode()))


def parsed_rows(files, columns):
    # The generator never emits the unit separator, CR, or quotes inside a value.
    # Reading physical lines first stops a broken quote from consuming the next row.
    field = '("[^",]*"|[^",]*)'
    pattern = sql_string(",".join([field] * len(columns)))
    names = ", ".join(f"fields[{i}] AS {name}" for i, name in enumerate(columns, 1))
    return f"""
        SELECT {names} FROM (
            SELECT string_split(replace(line, '"', ''), ',') AS fields
            FROM read_csv({path_list(files)}, delim=chr(31), quote='', escape='',
                          header=true, auto_detect=false, columns={{'line':'VARCHAR'}})
            WHERE regexp_full_match(line, {pattern})
        )
    """


def build_reference(db, data, scratch):
    began = time.monotonic()

    def stage(label, query):
        db.execute(query)
        print(f"{label} ({time.monotonic() - began:.1f}s elapsed)", flush=True)

    alias_file = scratch / "aliases.csv"
    write_aliases(data, alias_file)
    stage("Resolved linked IDs", f"""
        CREATE TABLE aliases AS SELECT * FROM read_csv({sql_string(alias_file.as_posix())}, all_varchar=true)
    """)
    for table in ("products", "campaigns"):
        source = sql_string((data / f"{table}.csv").as_posix())
        db.execute(f"CREATE TABLE {table} AS SELECT * FROM read_csv({source}, all_varchar=true)")
    db.execute("""
        CREATE MACRO stamp(s) AS CASE
            WHEN regexp_full_match(s, '[0-9]+') THEN try_cast(s AS BIGINT)
            WHEN regexp_full_match(s, '[0-9]{4}-[0-9]{2}-[0-9]{2}T([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9](Z|[+-]([01][0-9]|2[0-3]):[0-5][0-9])')
            THEN epoch_ms(try_strptime(left(s, 19), '%Y-%m-%dT%H:%M:%S')) -
                CASE substr(s, 20, 1) WHEN 'Z' THEN 0 ELSE
                    (try_cast(substr(s,21,2) AS INT)*60 + try_cast(substr(s,24,2) AS INT))
                    * 60000 * if(substr(s,20,1)='+',1,-1)
                END
        END
    """)
    bounds = json.loads((data / "manifest.json").read_text(encoding="utf-8"))
    start, end = db.execute("SELECT stamp(?), stamp(?)", [bounds["window_start"], bounds["window_end"]]).fetchone()
    events = parsed_rows((data / "events").glob("*.csv"), ["eid", "uid", "ts_text", "ingest", "kind", "pid", "cents"])
    # Earliest ingress wins globally. The generator writes ingress as fixed-width UTC.
    stage("Selected earliest event copies", f"""
        CREATE TABLE keepers AS SELECT DISTINCT ON(eid) uid, ts_text, ingest, kind, pid, cents
        FROM ({events}) WHERE eid <> '' ORDER BY eid, ingest
    """)
    stage("Validated events", f"""
        CREATE TABLE ev AS
        SELECT coalesce(a.person,v.uid) AS person, v.ts, v.price, coalesce(p.category,'unknown') AS category
        FROM (
            SELECT uid, pid, kind, stamp(ts_text) AS ts, stamp(ingest) AS ing,
                   CASE WHEN kind='purchase' THEN
                       CASE WHEN regexp_full_match(cents,'[0-9]+') THEN try_cast(cents AS BIGINT) END
                   ELSE 0 END AS price
            FROM keepers WHERE uid<>'' AND pid<>'' AND kind IN ('view','cart','purchase')
        ) v LEFT JOIN aliases a ON a.alias=v.uid LEFT JOIN products p ON p.product_id=v.pid
        WHERE ts >= {start} AND ts < {end} AND ing-ts <= 86400000
          AND (kind <> 'purchase' OR price BETWEEN 1 AND 10000000)
    """)
    stage("Assigned sessions", """
        CREATE TABLE assigned AS
        WITH gaps AS (SELECT *, lag(ts) OVER (PARTITION BY person ORDER BY ts) AS prior FROM ev),
        starts AS (SELECT *, if(prior IS NULL OR ts-prior>1800000, ts, NULL) AS new_start FROM gaps)
        SELECT person, ts, price, category,
               max(new_start) OVER (PARTITION BY person ORDER BY ts RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS t0
        FROM starts
    """)
    stage("Grouped sessions", """
        CREATE TABLE sess AS
        SELECT person, t0, cast(epoch_ms(t0) AS DATE) AS day, count(*) AS events, sum(price)::BIGINT AS revenue
        FROM assigned GROUP BY person,t0
    """)
    stage("Computed user daily", """
        CREATE TABLE user_daily AS
        WITH category_money AS (
            SELECT person, cast(epoch_ms(t0) AS DATE) AS day, category, sum(price) AS money
            FROM assigned WHERE price>0 GROUP BY ALL
        ), topcat AS (
            SELECT person, day, first(category ORDER BY money DESC,category) AS top_category
            FROM category_money GROUP BY person,day
        ), totals AS (
            SELECT person,day,count(*) AS sessions,sum(events)::BIGINT AS events,sum(revenue)::BIGINT AS revenue_cents
            FROM sess GROUP BY person,day
        )
        SELECT t.person AS user_id,t.day,t.sessions,t.events,t.revenue_cents,a.top_category
        FROM totals t LEFT JOIN topcat a USING(person,day)
    """)
    clicks = parsed_rows((data / "clicks").glob("*.csv"), ["cid", "uid", "campaign_id", "ts_text"])
    stage("Validated clicks", f"""
        CREATE TABLE click AS
        SELECT DISTINCT cid, coalesce(a.person,v.uid) AS person, v.campaign_id, stamp(v.ts_text) AS ts
        FROM ({clicks}) v INNER JOIN campaigns p ON p.campaign_id=v.campaign_id
        LEFT JOIN aliases a ON a.alias=v.uid
        WHERE cid<>'' AND uid<>'' AND ts >= {start-604800000} AND ts < {end}
    """)
    stage("Attributed sessions", """
        CREATE TABLE credited AS
        WITH one AS (
            SELECT person,ts,first(campaign_id ORDER BY cid) AS campaign_id FROM click GROUP BY person,ts
        ), chosen AS (
            SELECT s.*, CASE WHEN s.t0-c.ts <= 604800000 THEN c.campaign_id ELSE 'organic' END AS campaign_id
            FROM sess s ASOF LEFT JOIN one c ON s.person=c.person AND s.t0>=c.ts
        )
        SELECT campaign_id,day,count(*) AS sessions,count(*) FILTER(WHERE revenue>0) AS purchasing_sessions,
               sum(revenue)::BIGINT AS revenue_cents
        FROM chosen GROUP BY campaign_id,day
    """)
    # Each click covers [ts, ts + 7 days]. Merge overlapping intervals per campaign
    # so a session receives exactly one assist from every campaign covering its start.
    stage("Counted distinct assists", """
        CREATE TABLE assisted AS
        WITH gaps AS (
            SELECT *, lag(ts) OVER (PARTITION BY person,campaign_id ORDER BY ts) AS prior FROM click
        ), spans AS (
            SELECT *, sum(if(prior IS NULL OR ts-prior>604800000,1,0))
                OVER (PARTITION BY person,campaign_id ORDER BY ts) AS grp FROM gaps
        ), cover AS (
            SELECT person,campaign_id,min(ts) AS lo,max(ts)+604800000 AS hi
            FROM spans GROUP BY person,campaign_id,grp
        )
        SELECT c.campaign_id,s.day,count(*) AS assisted_sessions FROM sess s INNER JOIN cover c
        ON s.person=c.person AND s.t0 BETWEEN c.lo AND c.hi GROUP BY c.campaign_id,s.day
    """)
    stage("Computed campaign daily", """
        CREATE TABLE campaign_daily AS
        WITH totals AS (
            SELECT coalesce(cr.campaign_id,a.campaign_id) AS campaign_id,coalesce(cr.day,a.day) AS day,
                   coalesce(sessions,0) AS sessions,coalesce(purchasing_sessions,0) AS purchasing_sessions,
                   coalesce(revenue_cents,0) AS revenue_cents,coalesce(assisted_sessions,0) AS assisted_sessions
            FROM credited cr FULL OUTER JOIN assisted a USING(campaign_id,day)
        )
        SELECT t.campaign_id,coalesce(c.channel,'organic') AS channel,t.day,t.sessions,
               t.purchasing_sessions,t.revenue_cents,t.assisted_sessions
        FROM totals t LEFT JOIN campaigns c ON t.campaign_id=c.campaign_id
    """)

def candidate_query(db, candidate, table):
    expected = OUTPUT_TYPES[table]
    csv_file = candidate / f"{table}.csv"
    if csv_file.is_file():
        types = ", ".join(f"{sql_string(k)}:{sql_string(v)}" for k, v in expected.items())
        query = f"SELECT * FROM read_csv({sql_string(csv_file.as_posix())}, header=true, types={{{types}}})"
        actual = {row[0] for row in db.execute("DESCRIBE " + query).fetchall()}
        if actual != set(expected):
            raise ValueError(f"{csv_file} has columns {sorted(actual)}, expected {list(expected)}")
        return query

    folder = candidate / table
    if not folder.is_dir() or folder.is_symlink():
        raise ValueError(f"Expected an output folder at {folder}")
    files = sorted(path for path in folder.glob("*.parquet")
                   if path.is_file() and not path.is_symlink() and not path.name.startswith((".", "_")))
    if not files:
        raise ValueError(f"No visible Parquet files in {folder}")
    for path in files:
        actual = {row[0]: row[1] for row in db.execute(
            "DESCRIBE SELECT * FROM read_parquet(?)", [path.as_posix()]
        ).fetchall()}
        if actual.keys() != expected.keys():
            raise ValueError(f"{path} has columns {list(actual)}, expected {list(expected)}")
        for column, kind in expected.items():
            if actual[column] != kind and not (kind == "BIGINT" and actual[column] in INTEGER_TYPES):
                raise ValueError(f"{path}: column {column} has type {actual[column]}, expected {kind}")
    return f"SELECT * FROM read_parquet({path_list(files)}, union_by_name=true)"


def compare(db, candidate):
    matches = True
    for table, columns in OUTPUT_TYPES.items():
        query = candidate_query(db, candidate, table)
        projection = ", ".join(columns)
        expected = f"SELECT {projection} FROM {table}"
        actual = f"SELECT {projection} FROM ({query})"
        count_expected = db.execute(f"SELECT count(*) FROM ({expected})").fetchone()[0]
        count_actual = db.execute(f"SELECT count(*) FROM ({actual})").fetchone()[0]
        print(f"{table}: {count_expected:,} expected rows, {count_actual:,} candidate rows")
        # Include multiplicities. A duplicate can otherwise masquerade as a match.
        for label, difference in (("missing", f"{expected} EXCEPT ALL {actual}"),
                                  ("extra", f"{actual} EXCEPT ALL {expected}")):
            n = db.execute(f"SELECT count(*) FROM ({difference})").fetchone()[0]
            print(f"  {label}: {n:,}")
            if n:
                matches = False
                print("  Examples:", db.execute(f"SELECT * FROM ({difference}) LIMIT 4").fetchall())
    return matches


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("data", type=Path, help="Official generator's input dataset directory")
    parser.add_argument("candidate", type=Path, help="Candidate Parquet output or published dev-answer CSV directory")
    parser.add_argument("scratch", type=Path, help="New directory for DuckDB tables and spill files")
    parser.add_argument("--threads", type=int, default=6, help="DuckDB worker threads (default 6)")
    parser.add_argument("--memory-gib", type=int, default=8, help="DuckDB buffer memory limit in GiB (default 8)")
    args = parser.parse_args()
    if args.threads < 1 or args.memory_gib < 1:
        parser.error("threads and memory-gib must be positive")
    data, candidate, scratch = (path.resolve() for path in (args.data, args.candidate, args.scratch))
    if not (data / "manifest.json").is_file() or not candidate.is_dir():
        parser.error("provide a generated dataset with manifest.json and an existing candidate directory")
    try:
        scratch.mkdir(parents=True)
    except FileExistsError:
        parser.error(f"scratch directory already exists: {scratch}")
    with duckdb.connect(str(scratch / "oracle.duckdb")) as db:
        db.execute(f"SET threads={args.threads}")
        db.execute(f"SET memory_limit='{args.memory_gib}GiB'")
        db.execute("SET preserve_insertion_order=false")
        db.execute(f"SET temp_directory={sql_string((scratch / 'spill').as_posix())}")
        build_reference(db, data, scratch)
        return 0 if compare(db, candidate) else 1


if __name__ == "__main__":
    sys.exit(main())
