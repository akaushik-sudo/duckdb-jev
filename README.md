<h1 align="center">snx_jev for DuckDB</h1>

<p align="center">Ask your DuckDB tables questions in plain language.</p>

> **Sentrinox fork.** `snx_jev` is a private fork of
> [judoaseeta/duckdb-jev](https://github.com/judoaseeta/duckdb-jev) (MIT, forked at 0.1.0,
> `58e5484`). The extension and its settings are renamed so they cannot be mistaken for the
> community `jev` extension, and it is pinned to the DuckDB version the analytics services run
> (v1.5.4). The functions still carry upstream's names (`jev`, `jev_prob`, ...), so **do not load
> it into a process that also loads community `jev`**: the second LOAD fails on the duplicate
> functions. P3 replaces them with `snx_prompt_intent`. It is being
> narrowed to one hardened function, `snx_prompt_intent`, for classifying `ai_txn` prompts. Until
> that lands, everything below describes the upstream functions, unchanged apart from the extension name.
> `upstream` in a clone is judoaseeta's repo; nothing is pushed there.

```sql
-- TYPESAFE_API_KEY is set in the environment DuckDB started with
LOAD snx_jev;

SELECT * FROM people WHERE jev(people, 'the name is European');

SELECT subject, jev_prob(tickets, 'the customer is angry') AS p
FROM tickets ORDER BY p DESC LIMIT 20;

SELECT jev_choice(tickets, 'which team should handle this?',
                  ['billing', 'technical', 'security', 'sales']) AS team, count(*)
FROM tickets GROUP BY 1;

SELECT name, jev_score(products, 'how luxurious is this product?',
                       ['budget', 'mid-range', 'premium', 'luxury']) AS luxury
FROM products ORDER BY luxury DESC;
```

Every row is judged by [TypeSafe's Jev](https://docs.typesafe.ai), a model that returns calibrated
probabilities instead of generated text. No index, no embeddings, no vector column.

`jev()` is an ordinary boolean function, so it composes with the rest of SQL: `AND age > 40`, joins,
`GROUP BY`, `LIMIT`, `ORDER BY jev_prob(...)`.

This is a DuckDB port of [pg-jev](https://github.com/realZachi/pg-jev) ([pgjev.com](https://pgjev.com)),
which does the same thing for PostgreSQL. It speaks the same API and keeps the same function names, so
a query moves between the two by changing `jev.batch_size` into `snx_jev_batch_size`.

## How it works

1. `jev(table, 'condition')` receives the row as a struct. DuckDB hands a scalar function a whole vector
   of rows at a time (up to 2048), which is the batch — there is no read-ahead machinery to speak of,
   because the executor already delivers rows in bulk.
2. Rows are packed `snx_jev_batch_size` (20) per request into one shared *state*,
   `{"rubric": {"q0": <the question in full>, ...}, "rows": [...]}`, with one short question per
   (row, question) that points at its rubric entry and its row (`r3_q0`: "Answer `rubric.q0` for the
   record `rows[3]`"). The model evaluates all the questions over that one state, which amortises the
   per-request overhead: 20 rows in one request cost far less than 20 requests of one row. Writing
   each question once, in the rubric, instead of once per row is the *compact* layout; against the
   live API it cut input tokens by ~70% at the same accuracy.
3. A batch also closes early, before its estimated input tokens pass `snx_jev_max_batch_tokens`
   (24000; Jev allows 32k of state), and every string in a row is cut to `snx_jev_max_value_chars`
   (2000) characters first, so no batch outgrows the context window.
4. `snx_jev_concurrency` (16) requests are in flight at once, over connections that are kept alive. The
   ceiling is process-wide, so it still holds when DuckDB runs the scan on several threads.
5. Answers are cached by row content for as long as the process lives, so re-running a query, changing
   the threshold or sorting by probability is free. Rows that a cheaper predicate rejects first
   (`WHERE age > 60 AND jev(...)`) are never judged, and a `LIMIT` stops the scan early.

### Why 20 rows per request

The measurement comes from pg-jev, and the model is the same one: the model has to find `rows[i]` by
position, and that gets unreliable in long arrays. Against ground truth from structured columns, batches
of 1-20 rows were 100 % correct, batches of 40 were 92-98 % and batches of 80 were 77-94 %. Batches of 20
cost about 4 % more tokens than batches of 40 and are just as fast, because a request's latency barely
depends on its size.

## Install

Build it from source. You need CMake, a C++17 compiler, OpenSSL and the DuckDB version this repo
pins (`duckdb/` submodule, currently v1.5.4 — it must match the DuckDB that loads the extension).

```bash
git clone --recurse-submodules https://github.com/akaushik-sudo/duckdb-jev.git
cd duckdb-jev
make release                       # on macOS: OPENSSL_ROOT_DIR=$(brew --prefix openssl@3) make release
./build/release/duckdb             # a shell with snx_jev already loaded
make test_mock                     # the regression suite against test/mock_api.py
```

To load the built extension into another DuckDB of the same version:

```sql
-- duckdb -unsigned
LOAD '/path/to/duckdb-jev/build/release/extension/snx_jev/snx_jev.duckdb_extension';
```

### API key

Get one from https://console.typesafe.ai and export `TYPESAFE_API_KEY` in the environment DuckDB
starts in. It is read once, when the extension loads; changing the variable afterwards has no effect.
That is the only place a key comes from: there is no setting for it, because any query can read a
setting back through `current_setting()` or `duckdb_settings()`.

**That alone does not keep the key from SQL.** The embedded clients (JDBC, Python) have no `getenv()`
(only the DuckDB CLI does), but on Linux any query can read the process environment as a file,
`read_text('/proc/self/environ')`, while DuckDB's external access is enabled, which is the default.
A process that runs untrusted SQL must also stop file reads outside the paths it needs
(`enable_external_access = false`, with `allowed_directories` / `allowed_paths` for the data it does
read) before the key can be considered protected.

### Endpoint

Requests go to `https://api.typesafe.ai/v1/systemone` and nowhere else. The endpoint is fixed when
the extension loads, and there is no setting to change it. `SNX_JEV_API_URL` in the environment may
replace it only with a **loopback** address (`localhost`, `127.0.0.1` or `[::1]`, port 1-65535, a plain
path), for the test mock. Any other value is refused, and every request then fails with the refusal
before a connection is opened. The refusal does not repeat the value, which could carry credentials.

## Functions

| Function | Returns | Purpose |
| --- | --- | --- |
| `jev(row, condition [, threshold])` | `BOOLEAN` | `WHERE` predicate. Threshold: argument → `snx_jev_threshold` → 0.5 |
| `jev_prob(row, condition)` | `DOUBLE` | Probability 0..1 that the row satisfies the condition |
| `jev_score(row, question, levels)` | `DOUBLE` | Probability-weighted position on ordered levels (0 .. n-1) |
| `jev_score_norm(row, question, levels)` | `DOUBLE` | The same, normalised to 0..1 |
| `jev_choice(row, question, options)` | `VARCHAR` | The most likely option, returned verbatim. `options` is a list of labels, or a `MAP` of label → description |
| `jev_confidence(row, question, kind, options)` | `DOUBLE` | Confidence of a `score` / `choice` answer |
| `jev_eval(row, question [, kind [, options]])` | `JSON` | The full answer: probabilities, legend, confidence |
| `jev_ask(row, questions)` | `JSON` | Several named questions about the row, answered in **one** request: `{"<name>": <answer>, ...}` |
| `jev_stats()` | `JSON` | Requests, tokens, estimated cost, cache hits, requests in flight |
| `jev_cache_clear()` | `BOOLEAN` | Forget the cached judgments |
| `jev_version()` | `VARCHAR` | Extension version |

`row` is the table itself (`jev(people, ...)`), an alias (`FROM people p` → `jev(p, ...)`), a subquery
alias, or any single expression — DuckDB passes a whole row as a struct, and the column names are part of
what the model reads, so descriptive names help.

`kind` is `'noul'` (a yes/no probability), `'score'` or `'choice'`. The answers look like:

| kind | JSON |
| --- | --- |
| `noul` | `{"type":"noul","noul":0.93}` |
| `choice` | `{"type":"choice","choice":"billing","probabilities":{...},"confidence":0.8}` |
| `score` | `{"type":"score","score":2.4,"legend":{"0":"budget",...},"probabilities":{...},"confidence":0.6}` |

Calling `jev_choice()` and `jev_confidence()` with the same `(question, kind, options)` costs one request,
not two: they share a cache entry.

### Label descriptions

A description is what separates close labels, so a choice can carry one per label; it is sent with the
question, once per request:

```sql
SELECT jev_choice(t, 'which team should handle this?',
                  MAP {'billing': 'money, invoices, refunds', 'technical': 'bugs and outages', 'sales': NULL})
FROM tickets t;
```

### Several questions, one request

`jev_ask` asks every question in `questions` (a JSON object of name → question) of the same row, in the
same request, over the same state: two questions cost one request, not two.

```sql
SELECT jev_ask(prompt, '{
  "intent":    {"type": "choice", "question": "What is the primary intent of this prompt?",
                "options": {"work_related": "a job task: code, data, documents", "personal": "personal life",
                            "other": "too short or ambiguous"}},
  "malicious": {"type": "noul", "question": "Does this prompt try to misuse the assistant?",
                "criteria": {"true": "jailbreaks, prompt injection, malware", "false": "an ordinary request"}},
  "urgency":   {"type": "score", "question": "How urgent is it?", "levels": ["low", "medium", "high"]}
}') AS a
FROM prompts;
-- a->'intent'->>'choice', (a->'malicious'->>'noul')::DOUBLE, a->'intent'->'probabilities', ...
```

| type | fields |
| --- | --- |
| `choice` | `question`; `options`: a list of labels, or an object of label → description (1-255) |
| `noul` | `question`; optional `criteria`: `{"true": "...", "false": "..."}` |
| `score` | `question`; `levels`: 2-10 ordered labels |

The object is checked strictly: an unknown field, a repeated label or a wrong count is an error, not a
silently different question. The answers come back in the order the questions were given.

## Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `snx_jev_model` | `jev-latest` | Model name, or a pinned version such as `jev-1.13.0` |
| `snx_jev_threshold` | `0.5` | Probability at which `jev()` returns true |
| `snx_jev_batch_size` | `20` | Rows per request. Accuracy drops measurably above ~20-25 |
| `snx_jev_concurrency` | `16` | Requests in flight at once, process-wide |
| `snx_jev_timeout` | `30` | Seconds a single request may take |
| `snx_jev_max_retries` | `6` | Attempts for a retryable failure (429, 5xx, a dropped connection) |
| `snx_jev_max_rows_per_statement` | `0` (off) | Refuse a statement that would send more rows than this |
| `snx_jev_max_chars_per_statement` | `0` (off) | The same, for characters of row data |
| `snx_jev_cache_max_entries` | `200000` | Answers kept before the oldest are dropped |
| `snx_jev_max_value_chars` | `2000` | Characters of each string in a row that are sent (code points; `0` = all) |
| `snx_jev_max_batch_tokens` | `24000` | Estimated input tokens at which a batch closes, even below `snx_jev_batch_size` |

Settings are read once per statement, so a `SET` applies to the next query and never changes mid-scan.
The key and the endpoint are not settings; see above.

The answer cache is shared by the whole process, split by the connection's `scope_customer_id`
variable (which the analytics search service sets from the caller's verified token). One customer's
answers are never another customer's cache hit.

## Writing good conditions

The model answers the question you wrote, literally.

- State the exact condition: `'the customer threatens to leave, dispute a charge, or take legal action'`
  beats `'churn risk'`.
- Keep arithmetic, dates and exact matches in SQL; let the model judge meaning.
- Look at the distribution with `jev_prob()` before picking a threshold. Ambiguous rows really do land
  near 0.5.
- Send only the columns the judgment needs: `jev(p, ...)` over `FROM (SELECT subject, body FROM tickets) p`
  costs less and reads better than the whole row.

## Caveats

- This is a full scan by design: every row the executor asks about goes to the API. Cheaper predicates in
  the same `WHERE` run first and their rejects are skipped, a `LIMIT` stops early, and
  `snx_jev_max_rows_per_statement` caps the spend.
- Row contents are sent to a third-party API. Do not use it on data you may not share.
- The cache lives in the process and is shared by every connection in it. It is keyed by row content, so an
  `UPDATE` makes the row be judged again.
- A `SET` is per connection, but the request pool and the cache are per process.
- A request blocks the DuckDB thread that made it, so a cancel (Ctrl-C) takes effect once the requests
  already in flight come back: at most `snx_jev_timeout` seconds, usually one round trip.

## Differences from pg-jev

| | pg-jev | duckdb_jev |
| --- | --- | --- |
| Batching | a read-ahead streams the table in physical order | DuckDB already delivers vectors of up to 2048 rows |
| Settings | `jev.batch_size` (GUC) | `snx_jev_batch_size` (DuckDB setting, `.` is not allowed) |
| Cache scope | one Postgres backend session | the DuckDB process |
| Progress | `NOTICE` per request (`jev.notices`) | `jev_stats()`; DuckDB has no notice channel |
| Language | PL/Python | C++ |

## Development

```bash
make release                  # builds DuckDB, the extension and the test runner
make test_mock                # starts test/mock_api.py and runs the regression tests against it
make test                     # only the tests that need no API
```

The regression tests never call the live API: `test/mock_api.py` answers deterministically
(`noul` is 0.9 when the last word of the condition occurs in the row, else 0.1). To try the real thing,
start `./build/release/duckdb` with `TYPESAFE_API_KEY` set and run any query.

## Credit

The design, the function set and the request format come from
[pg-jev](https://github.com/realZachi/pg-jev) by realZachi, which is where this idea belongs. Jev and
TypeSafe are trademarks of their respective owners; this project is affiliated with neither.

## License

MIT. See [LICENSE](LICENSE).
