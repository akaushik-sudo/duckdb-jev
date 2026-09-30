# Tests

`sql/` holds [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html):

The key (`TYPESAFE_API_KEY`) and the endpoint (`SNX_JEV_API_URL`) come only from the environment the
process starts with; SQL can set neither. So each case that needs a different key or endpoint is a
file of its own, guarded by `require-env`, and `scripts/run_tests_with_mock.sh` runs each in its own
`unittest` process with exactly the environment it needs:

| File | Environment | What it checks |
| --- | --- | --- |
| `jev_offline.test` | none | Settings and defaults; that the key and the endpoint are not settings; NULL handling; argument validation |
| `jev_no_key.test` | no key | Nothing is sent without a key |
| `jev_api.test` | mock + its key | The judgment functions, batching, the cache and its per-customer split, errors |
| `jev_wrong_key.test` | mock + a wrong key | A 401 is reported as such |
| `jev_unreachable.test` | a dead loopback port | The spend guard; an unreachable endpoint |
| `jev_host_refused.test` | ten hostile `SNX_JEV_API_URL`s | Each is refused before a request; several would reach the mock if let through |

```bash
make test        # jev_offline.test only; every other file is skipped for lack of its environment
make test_mock   # starts mock_api.py and runs every case
```

`mock_api.py` is a deterministic stand-in for the TypeSafe endpoint, so the expected results never
move and no test ever reaches the live API:

- `noul` answers 0.9 when the last word of the condition occurs in the row JSON, else 0.1
- `score` picks the level at `len(row_json) % len(levels)`
- `choice` picks the option at `len(row_json) % len(options)`
- a condition containing `trigger422` answers 422, `trigger503` answers 503

To try the real API, start DuckDB with `TYPESAFE_API_KEY` set and run a query.
