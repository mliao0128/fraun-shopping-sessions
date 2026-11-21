"""Compare Parquet output against published CSV answers using the starter's reader.

Install duckdb==1.5.6, then pass a fixture's expected directory or dev-answer,
followed by the directory containing the candidate's two Parquet tables.
"""
import argparse
from pathlib import Path

import duckdb

from check_generated import OUTPUT_TYPES, candidate_query, compare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("expected", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(f"DuckDB {duckdb.__version__}")
    with duckdb.connect() as db:
        for table in OUTPUT_TYPES:
            if not (args.expected / f"{table}.csv").is_file():
                parser.error(f"Missing {table}.csv in {args.expected}")
            if (args.output / f"{table}.csv").exists():
                parser.error(f"Expected Parquet output in {args.output}")
            db.execute(f"CREATE VIEW {table} AS {candidate_query(db, args.expected, table)}")
        return 0 if compare(db, args.output) else 1


if __name__ == "__main__":
    raise SystemExit(main())
