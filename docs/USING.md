# Using snx_jev locally

`snx_jev` is a DuckDB extension that classifies AI-gateway prompts with TypeSafe's Jev model:

```sql
snx_prompt_intent(user_prompt VARCHAR)
  -> STRUCT(intent VARCHAR,                    -- 'work_related' | 'personal' | 'other'
            intent_confidence DOUBLE,
            intent_probabilities MAP(VARCHAR, DOUBLE),
            malicious_probability DOUBLE)       -- 0..1; treat >= 0.5 as malicious
```

## What you need

- **The file for your machine.** Each file only loads into **DuckDB 1.5.4**, on Linux:

  | File | Runs on |
  |---|---|
  | `snx_jev-0.2.0-duckdb-v1.5.4-linux_amd64.duckdb_extension` | Linux on Intel/AMD, and Docker on an Intel/AMD Windows or Mac |
  | `snx_jev-0.2.0-duckdb-v1.5.4-linux_arm64.duckdb_extension` | Linux on ARM, and Docker on an Apple Silicon Mac |

  On Windows or macOS, run DuckDB inside Docker or WSL (below): there are no native Windows or
  macOS builds yet. Check the file against its `.sha256` before you use it.
- **A TypeSafe API key** in the environment variable `TYPESAFE_API_KEY`. It is read once, when the
  extension loads, and there is no way to set it from SQL.
- **Unsigned extensions allowed.** The extension is built by us, not signed by DuckDB:
  `duckdb -unsigned` on the command line, or `allow_unsigned_extensions` in Python, JDBC and so on.

## Try it

On Linux, with the [DuckDB 1.5.4 CLI](https://github.com/duckdb/duckdb/releases/tag/v1.5.4):

```sh
sha256sum -c snx_jev-0.2.0-duckdb-v1.5.4-linux_amd64.duckdb_extension.sha256
TYPESAFE_API_KEY=... duckdb -unsigned
```

```sql
LOAD './snx_jev-0.2.0-duckdb-v1.5.4-linux_amd64.duckdb_extension';

SELECT snx_prompt_intent('Write a SQL query that returns the top 10 models by cost last week.');

-- what the last query sent and cost
SELECT snx_jev_last_query_stats();
```

In Docker, from the folder that holds the file (works on Windows, macOS and Linux). Set
`TYPESAFE_API_KEY` in your shell first; `-e TYPESAFE_API_KEY` passes it in without writing it on
the command line:

```sh
docker run --rm -it -e TYPESAFE_API_KEY -v "$PWD:/ext" python:3.12-slim bash
# inside the container:
pip -q install duckdb==1.5.4
python
```

```python
import duckdb
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("LOAD '/ext/snx_jev-0.2.0-duckdb-v1.5.4-linux_amd64.duckdb_extension'")
print(con.sql("SELECT snx_prompt_intent('plan a weekend trip to Lisbon') AS r"))
print(con.sql("SELECT snx_jev_last_query_stats()"))
```

On an Apple Silicon Mac, use the `linux_arm64` file. Or, if you only have the `linux_amd64` one,
add `--platform linux/amd64` to `docker run`: Docker runs the container emulated, which is slower
but makes little difference here, where the time goes to waiting on the API. Neither file loads into
a native (non-Docker) macOS or Windows DuckDB.

## Classifying real prompts

The same as any other function, over `ai_txn` / `v_ai_txn` once you have the lake attached
(read-only is enough):

```sql
SELECT r.intent, count(*) AS prompts,
       count(*) FILTER (r.malicious_probability >= 0.5) AS malicious
FROM (SELECT snx_prompt_intent(user_prompt) AS r
      FROM (SELECT DISTINCT user_prompt FROM v_ai_txn WHERE user_id = 42 AND user_prompt IS NOT NULL))
GROUP BY ALL;
```

- Classify **distinct** prompts: identical prompts in one query are sent once anyway, and answers are
  cached for as long as the DuckDB process runs, but `DISTINCT` keeps the query small.
- `intent` on a purely malicious prompt is not meaningful (it tends to read `work_related`): look at
  `malicious_probability` for those.

## Cost and limits

- About **$0.006 per 1,000 prompts** (~138 input tokens each, at $0.042 per million input tokens).
- **The key's budget is shared** with everyone who uses the same key. Before a large run, try a
  `LIMIT`, then check `snx_jev_last_query_stats()`'s `estimated_cost_usd`.
- Cap a session: `SET snx_jev_max_rows_per_statement = 1000;` refuses any statement that would send
  more than 1,000 prompts.
- Requests are paced at 1,000 a minute per process (`SNX_JEV_MAX_REQUESTS_PER_MINUTE` in the
  environment changes it), and 25 prompts go in each request.

## Asking other questions

Start DuckDB with `SNX_JEV_ENABLE_GENERIC=1` as well, and the general functions exist too:
`jev(row, 'condition')`, `jev_prob`, `jev_choice`, `jev_score`, `jev_ask(row, questions)` and more.
See the [README](../README.md). Do not load community `jev` in the same process: the names clash.

## If something goes wrong

| Error | Meaning |
|---|---|
| `... built for DuckDB version ...` / `... platform ...` | Wrong DuckDB version (must be 1.5.4) or wrong file for your CPU |
| `extension ... is not signed` / unsigned | Start DuckDB with `-unsigned` (or `allow_unsigned_extensions`) |
| `snx_jev: no API key` | `TYPESAFE_API_KEY` was not set when DuckDB started |
| `API error 401` | The key is wrong |
| `... would send N rows to the API, above snx_jev_max_rows_per_statement` | Your own cap: narrow the query or raise the cap |
