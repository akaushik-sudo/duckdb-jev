<h1 align="center">snx_jev</h1>

<p align="center">A DuckDB extension that classifies AI-gateway prompts by intent, and flags the malicious
ones, with TypeSafe's Jev model.</p>

```sql
LOAD snx_jev;   -- TYPESAFE_API_KEY is in the environment DuckDB started with

SELECT r.intent, count(*) AS prompts, count(*) FILTER (r.malicious_probability >= 0.5) AS malicious
FROM (SELECT snx_prompt_intent(user_prompt) AS r
      FROM (SELECT DISTINCT user_prompt FROM v_ai_txn WHERE user_id = 42 AND user_prompt IS NOT NULL))
GROUP BY ALL;
```

`snx_jev` is Sentrinox's fork of [judoaseeta/duckdb-jev](https://github.com/judoaseeta/duckdb-jev)
(MIT), hardened and narrowed for one job: classifying the prompts in the analytics lake's `ai_txn`, from
the Logs search box and from hand-written SQL. It is built for **DuckDB v1.5.4**, the version the
analytics services run.

| | |
|---|---|
| **Status** | `v0.2.0`, pre-release: [Releases](https://github.com/akaushik-sudo/duckdb-jev/releases). Hands-on use only for now; the Logs search box gets it in P5 |
| **Use it** | [`docs/USING.md`](docs/USING.md): download, load, classify, cost, errors |
| **Plan** | [`docs/PLAN.md`](docs/PLAN.md): phases P0-P6, decisions, live measurements |

## snx_prompt_intent

```sql
snx_prompt_intent(user_prompt VARCHAR)
  -> STRUCT(intent                VARCHAR,              -- 'work_related' | 'personal' | 'other'
            intent_confidence     DOUBLE,
            intent_probabilities  MAP(VARCHAR, DOUBLE),
            malicious_probability DOUBLE)              -- 0..1; >= 0.5 is treated as malicious
```

Two questions, asked of each prompt in the same request:

| Question | Answer | What the model is told (compiled in) |
|---|---|---|
| **intent** | `work_related` | Asks for help with a job task: code, data, documents, analysis, business communication |
| | `personal` | Personal life, hobbies, health, relationships, shopping, entertainment, not a job task |
| | `other` | Too short, empty, or ambiguous to place in either label above |
| **malicious** | P(yes), 0-1 | Tries to cause harm or misuse the assistant: jailbreaks, prompt injection, malware, fraud, harassment, extracting secrets or credentials |

- **What is asked is fixed.** The labels, descriptions and wording are compiled in; SQL chooses only
  which prompts to classify. Changing the taxonomy is a code change in `PromptIntentQuestions()`
  (`src/jev_functions.cpp`) followed by a live re-check.
- **Text only.** Passing a row or a struct is a binder error, so a query cannot send other columns.
- **NULL in, NULL out**, and nothing is sent. A field the API did not answer is NULL, never `'other'` or 0.
- **Measured** on 50 hand-labelled prompts against the live API (`jev-1.13.0`): intent 98 % correct;
  every malicious prompt ≥ 0.92 and every other ≤ 0.07; ~138 input tokens, ≈ **$0.006 per 1,000 prompts**.
- `intent` of a purely malicious prompt is not meaningful (it reads `work_related`). Lead with
  `malicious_probability` for those.

`snx_jev_last_query_stats()` reports what the last query **on this connection** that used the
extension did: `requests`, `rows_sent`, `cache_hits`, `shared_in_flight`, `retries`,
`rate_limited_ms`, `input_tokens`, `estimated_cost_usd`. It never shows another connection's spend.

## What is registered

| By default | With `SNX_JEV_ENABLE_GENERIC=1` at LOAD |
|---|---|
| `snx_prompt_intent`, `snx_jev_last_query_stats`, `jev_version` | also the [general-purpose functions](#general-purpose-functions): `jev`, `jev_prob`, `jev_choice`, `jev_ask`, ... |

A process that runs generated SQL (the search service) never sets the flag, so the general functions,
which take their question from the caller, do not exist there. SQL cannot turn it on.

## Configuration

**Environment**, read once when the extension loads; SQL can neither read nor change these:

| Variable | Default | Meaning |
|---|---|---|
| `TYPESAFE_API_KEY` | none | The API key. Requests fail with "no API key" without it |
| `SNX_JEV_MAX_REQUESTS_PER_MINUTE` | `1000` | Process-wide pace, below Jev's 1,200; `0` = no limit; a value that is not a number refuses requests |
| `SNX_JEV_ENABLE_GENERIC` | unset | `1` registers the general-purpose functions |
| `SNX_JEV_API_URL` | TypeSafe's API | Tests only: may point at a **loopback** address (the mock); anything else is refused |

**Settings**, per connection (`SET`), read once per statement:

| Setting | Default | Meaning |
|---|---|---|
| `snx_jev_max_rows_per_statement` | `0` (off) | Refuse a statement that would send more prompts than this. Search will set 1,000 |
| `snx_jev_max_chars_per_statement` | `0` (off) | The same, for characters of row data |
| `snx_jev_batch_size` | `25` | Prompts per request |
| `snx_jev_max_batch_tokens` | `24000` | Estimated input tokens at which a batch closes early (Jev allows 32k of state) |
| `snx_jev_max_value_chars` | `2000` | Characters of each string that are sent (code points; `0` = all) |
| `snx_jev_concurrency` | `16` | Requests in flight at once, process-wide |
| `snx_jev_timeout` | `30` | Seconds a single request may take |
| `snx_jev_max_retries` | `6` | Attempts for a retryable failure (429, 5xx, a dropped connection) |
| `snx_jev_max_wait_seconds` | `600` | How long a query waits for another query already sending the same prompt |
| `snx_jev_cache_max_entries` | `200000` | Answers kept before the oldest are dropped |
| `snx_jev_model` | `jev-latest` | Model, or a pinned version such as `jev-1.13.0` |
| `snx_jev_threshold` | `0.5` | Probability at which the general `jev()` returns true |

## How it works

1. **DuckDB hands over ~2,048 rows at a time.** Identical prompts among them are sent once, and
   prompts already answered come from the cache.
2. **25 prompts per request, in the compact layout.** The state holds each question once
   (`"rubric": {"intent": ..., "malicious": ...}`) and the prompts keyed `"items": {"1": ..., "25": ...}`;
   each (prompt, question) gets a one-line pointer ("Using rubric.malicious in the state: is
   items."7" malicious?"). This is the layout measured against the live API: ~70 % fewer tokens than
   repeating the question per prompt, at the same accuracy. A shorter, generic wording cost the
   malicious margin (lowest malicious score 0.37 instead of 0.92), so the measured one stays.
3. **Bounded requests.** Each string is cut to 2,000 characters first, and a batch closes before its
   estimated tokens pass `snx_jev_max_batch_tokens` (estimated per character class, so CJK and emoji are
   not undercounted).
4. **Parallel and paced.** Up to 16 requests are in flight per process, however many threads DuckDB
   scans with; a token bucket keeps the process under `SNX_JEV_MAX_REQUESTS_PER_MINUTE`, so a large
   query slows down instead of meeting 429s. 429s and 5xx are retried with backoff.
5. **Single-flight.** When two queries (or two scan threads) meet the same uncached prompt at once, one
   sends it and the other waits for that answer. If the sender fails for its own reasons, a waiter
   claims the prompt and sends it itself; a prompt goes out at most twice in all.
6. **Cache, per customer.** Answers are kept for the life of the process, keyed by the connection's
   `scope_customer_id` variable (which search pins from the caller's verified token), the model, the
   questions and the prompt. One customer's answers are never another's cache hit. The customer is read
   when the query runs, so a prepared statement follows a re-pinned connection.

## Security model

| Concern | How it is handled |
|---|---|
| Sending data elsewhere | Requests go to `https://api.typesafe.ai/v1/systemone` only. The endpoint is fixed at LOAD; there is no setting for it, and `SNX_JEV_API_URL` accepts loopback only (strictly parsed: no userinfo, query, odd ports or lookalike hosts). A refused URL is not echoed in errors |
| Reading the key back | The key is not a setting, so `current_setting()` / `duckdb_settings()` show nothing, and JDBC/Python have no `getenv()`. **But on Linux, `read_text('/proc/self/environ')` returns the environment while DuckDB's external access is on.** A process that runs untrusted SQL must switch file access off (`enable_external_access = false`, with `allowed_directories` / `allowed_paths` for what it reads) |
| Asking arbitrary questions | Only `snx_prompt_intent` exists by default; its questions are compiled in |
| Sending other columns | `snx_prompt_intent` takes VARCHAR only |
| Spend | Per-statement row and character caps; a process-wide rate limit no connection can raise; per-connection cost reporting |
| Cross-customer leaks | The cache is split by `scope_customer_id`. That variable is ordinary `SET VARIABLE`: it holds for search because search's query guard rejects `SET` in generated SQL |
| Signing | The binary is ours, not signed by DuckDB: loaders need `allow_unsigned_extensions` (`duckdb -unsigned`) |

Row contents go to a third-party API (TypeSafe). Do not classify data you may not share.

## Performance and cost

| | Result | Notes |
|---|---|---|
| 50 prompts, one per request (Phase 1) | 44.8 s, 464 tokens/prompt | measured; the old SQL macros, sequential |
| 50 prompts, compact batches (Phase 1) | 0.7 s, 138 tokens/prompt | measured; the layout this extension uses |
| 50 prompts through `snx_prompt_intent` | 2 requests, 137.6 tokens/prompt, $0.0003 | measured, live, `jev-1.13.0` |
| 1,000 distinct prompts | ~2-3 s, ~40 requests, ~$0.006 | **estimated, not measured yet**; the basis for search's 1,000 cap |

## Building and testing

The extension is compiled together with DuckDB v1.5.4 (the `duckdb/` submodule): an extension only
loads into the exact DuckDB version it was built against. A cold build takes ~25 minutes; ccache makes
later ones quick.

On Linux or WSL (Ubuntu 24.04):

```bash
git clone --recurse-submodules https://github.com/akaushik-sudo/duckdb-jev.git && cd duckdb-jev
sudo apt-get install -y build-essential cmake ninja-build libssl-dev python3 ccache
GEN=ninja make release                 # DuckDB + the extension + the test runner
make test_mock                         # the whole regression suite against test/mock_api.py
./scripts/run_tests_with_mock.sh test/sql/jev_prompt_intent.test   # one file
./build/release/duckdb                 # a shell with snx_jev statically linked
```

No test ever calls the live API: `test/mock_api.py` answers deterministically and rejects any request
that is not the exact compact shape. The key and the endpoint come only from the environment, so
`run_tests_with_mock.sh` runs each case in its own process with its own environment, and fails if a
test file has no case. See [`test/README.md`](test/README.md).

CI (GitHub Actions) runs the mock suite and a format check on every PR. The platform build is
manual-only, to keep Actions minutes down.

### Releases

```bash
./scripts/build-release.sh 0.2.0      # linux_amd64 and linux_arm64, in Docker
```

It builds one resolved commit (`git archive`, submodules included, LF line endings) in `ubuntu:24.04`,
runs the mock suite against that build, then `scripts/package-release.sh` loads each binary **exactly
as shipped** into a stock duckdb 1.5.4 and writes
`dist/<version>/snx_jev-<version>-duckdb-v1.5.4-<platform>.zip`: the binary, its `.sha256`,
`USING.md` and `VERSION.txt` (commits and tests run). The file inside must stay named
`snx_jev.duckdb_extension`: DuckDB derives the extension's entry point from the file name.

`linux_arm64` builds under QEMU emulation on an x86 machine: ~6 hours cold. Releases are attached to
GitHub Releases for now; publishing to the `analytics-maven` MinIO repo, which search's image build
will need, waits on upload credentials.

## Repository layout

| Path | What |
|---|---|
| `src/snx_jev_extension.cpp` | Entry point: fixes the environment, registers settings and functions |
| `src/jev_functions.cpp` | The SQL functions, `snx_prompt_intent`'s taxonomy, batching, single-flight, cache use |
| `src/jev_client.cpp` | Request layout, the HTTP call, retries, token estimates |
| `src/jev_config.cpp` | Environment (endpoint, key, rate, generic flag) and settings |
| `src/jev_state.cpp` | Process-wide cache, single-flight registry, rate limiter, request pool; per-connection stats |
| `src/jev_json.cpp` | DuckDB values to JSON, with truncation |
| `test/` | sqllogictests and the mock API |
| `scripts/` | Test runner, release build and packaging |
| `docs/` | [`USING.md`](docs/USING.md) for users, [`PLAN.md`](docs/PLAN.md) for the plan |
| `duckdb/`, `extension-ci-tools/` | Submodules, pinned to v1.5.4 |

## General-purpose functions

Only with `SNX_JEV_ENABLE_GENERIC=1`. They ask any question of any row, so they are for analysts, not
for a process that runs generated SQL.

| Function | Returns | Purpose |
|---|---|---|
| `jev(row, condition [, threshold])` | `BOOLEAN` | `WHERE` predicate; threshold: argument, then `snx_jev_threshold`, then 0.5 |
| `jev_prob(row, condition)` | `DOUBLE` | Probability that the row satisfies the condition |
| `jev_choice(row, question, options)` | `VARCHAR` | The most likely option. `options`: a list of labels, or a `MAP` of label → description |
| `jev_score(row, question, levels)` / `jev_score_norm` | `DOUBLE` | Position on ordered levels (0..n-1), or normalised to 0..1 |
| `jev_confidence(row, question, kind, options)` | `DOUBLE` | Confidence of a choice or score answer |
| `jev_eval(row, question [, kind [, options]])` | `JSON` | The full answer, probabilities included |
| `jev_ask(row, questions)` | `JSON` | Several named questions about the row in **one** request |
| `jev_stats()` / `jev_cache_clear()` | `JSON` / `BOOLEAN` | Process-wide counters / forget the cache |

`row` is a table or alias (passed as a struct, column names included), or any single expression.

```sql
SELECT subject, jev_prob(t, 'the customer threatens to leave') AS p
FROM (SELECT subject, body FROM tickets) t ORDER BY p DESC LIMIT 20;

SELECT jev_choice(t, 'which team should handle this?',
                  MAP {'billing': 'money, invoices, refunds', 'technical': 'bugs and outages', 'sales': NULL})
FROM tickets t;

SELECT jev_ask(prompt, '{
  "topic":  {"type": "choice", "question": "What is it about?", "options": ["billing", "bug", "other"]},
  "urgent": {"type": "noul",   "question": "Does it need an answer today?"},
  "anger":  {"type": "score",  "question": "How angry is it?", "levels": ["calm", "annoyed", "furious"]}
}') AS a FROM prompts;
```

`jev_ask` validates its questions strictly: question names are identifiers, `choice` takes 1-255
options, `score` 2-10 levels, `noul` an optional `{"true": ..., "false": ...}`, and an unknown field is
an error. These functions keep upstream's names, so do not load community `jev` in the same process.

## Differences from upstream

[judoaseeta/duckdb-jev](https://github.com/judoaseeta/duckdb-jev) 0.1.0 is a general "ask Jev about any
row" extension. This fork:

- is renamed `snx_jev` (and its settings `snx_jev_*`) and pinned to DuckDB v1.5.4;
- registers only `snx_prompt_intent` by default; the general functions are opt-in;
- pins the endpoint and takes the key from the environment only, with no URL or key settings;
- sends descriptions with labels, asks several questions per request (`jev_ask`), uses the measured
  compact layout, bounds batches by tokens and truncates strings;
- adds the per-customer cache, single-flight, the process-wide rate limiter and per-connection stats.

## Credit and license

The design, the function set and the request format come from [pg-jev](https://github.com/realZachi/pg-jev)
by realZachi, ported to DuckDB by [judoaseeta](https://github.com/judoaseeta/duckdb-jev). Jev and TypeSafe
are trademarks of their owners; this project is affiliated with neither.

MIT, see [LICENSE](LICENSE); judoaseeta's copyright notice is kept.
