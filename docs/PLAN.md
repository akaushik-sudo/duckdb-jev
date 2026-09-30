# snx_jev — plan

Turn upstream `jev` (a general "ask Jev about any row" extension) into `snx_jev`: one hardened
function, `snx_prompt_intent(user_prompt)`, that the analytics search service loads to classify
`ai_txn` prompts. Each phase is one PR.

The taxonomy (labels, descriptions, instructions), the measured batching results and the reasons
for the design are in analytics-schema's prompt intent spec (`ollylake/prompt_intent_spec.md`,
§1, §3, §11, §12 — in history on `demo/prompt-jev-batch`).

## P0 — Fork and build (done)

- Renamed `jev` → `snx_jev`, so it can never collide with the community extension.
- Pinned `duckdb` and `extension-ci-tools` to v1.5.4, the DuckDB version the services run.
- CI: the mock suite runs on PRs and main; the platform builds run on a `v*` tag or a manual dispatch.
- Verified: `make test_mock` passes, and the loadable binary loads into a stock duckdb 1.5.4.

## P1 — Harden (done)

- **Endpoint:** fixed once per process, at LOAD. The default is `https://api.typesafe.ai/v1/systemone`.
  `SNX_JEV_API_URL` may replace it only with loopback (`localhost` / `127.0.0.1` / `[::1]`, port
  1-65535). The URL is parsed once, strictly; the HTTP client is built from the canonical
  `scheme://host:port` of that parse, so no second parser can read it differently. Anything else,
  including userinfo (`@`), backslash, queries, percent-escapes, lookalike hosts and other schemes,
  is recorded as a refusal (without repeating the value), and every request fails with it before a
  connection opens. The `jev_api_url` setting is gone.
- **Key:** only from `TYPESAFE_API_KEY`, read once at LOAD, and the `jev_api_key` setting is gone,
  so `current_setting()` / `duckdb_settings()` have nothing to show. The JDBC and Python clients have no
  SQL `getenv()`; only the CLI shell does. **But on Linux, `read_text('/proc/self/environ')` returns
  the environment while external access is on**, so the key is protected only once P5 switches file
  access off (see P5). A DuckDB secret was not added: the environment is what search's deployment
  already uses.
- **Cache:** namespaced by the connection's `scope_customer_id` variable, which search already pins
  from the verified token (`ClassicSearchEngineV2.pinCustomerScope`). No argument was needed, so
  P3 stays single-argument. An unset variable is a namespace of its own.
- The remaining settings are renamed `jev_*` → `snx_jev_*`: upstream's `RegisterSettings` silently
  skips a name that is already registered, so a community `jev` loaded first would have owned ours.
- User-Agent is `snx-jev/<version>`.
- Tests: one `unittest` process per environment (`scripts/run_tests_with_mock.sh`): offline, no key,
  mock, wrong key, unreachable, and ten hostile URLs. Several of those would reach the running mock
  if let through; a negative control, the same test run with the real mock URL, fails as it should.
- Tests: every file that can send requires `SNX_JEV_API_URL`, and the runner refuses to give a
  sending case anything but loopback. The runner fails if a `test/sql/*.test` has no case in it, and
  takes one file as an argument again.
- CI: release binaries (`duckdb-stable-build`) need `mock-api-tests` to pass.
- Still global, on purpose until P3 removes them from the search path: `jev_stats()` counters and
  the `snx_jev_*` settings. P5 freezes the settings with `lock_configuration = true`.
- **Trust assumption:** the cache split trusts `scope_customer_id`, an ordinary `SET VARIABLE`. It
  holds for search because search's query guard rejects `SET` statements in generated SQL. Any
  other process that runs untrusted SQL needs the same guard. Also process-wide and callable by
  any connection today: `jev_cache_clear()` and the `snx_jev_cache_max_entries` eviction (P3/P5).

## P2 — What our questions need (done)

- `choice` criteria carry descriptions (label → description), not bare labels.
- Several questions per row share one request, so intent and malicious cost one call.
- Compact layout: the rubric goes in the state once, and each per-row question is a pointer
  (−70% tokens in Phase 1).
- Return the choice's `probabilities`.
- Close batches by estimated tokens as well as row count; truncate each prompt at 2,000 chars.
- Done when: the mock tests pass, and a manual live check reproduces Phase 1 (≈138 input tokens per prompt).

What landed:
- **Compact layout for every request**: `state = {"rubric": {"q0": ...}, "rows": [...]}`, and one pointer
  question per (row, question), keyed `r<row>_q<k>`. The single-question functions are a one-question
  set on the same path, so upstream's per-row repetition of choice/score questions is gone for them too.
- **Descriptions**: `jev_choice` / `jev_confidence` / `jev_eval` take options as a list or a
  `MAP(label → description)` (one `ANY` signature, checked at bind, so an untyped NULL is not ambiguous).
  `jev_ask` noul questions take `{"true": ..., "false": ...}` criteria. Descriptions are in the cache key.
- **Several questions, one request**: `jev_ask(row, questions JSON) → JSON`, strict validation (types,
  unknown fields, 1-255 options, 2-10 levels, ≤ 16 questions). One cache entry holds all of a row's answers.
  P3's `snx_prompt_intent` is this path with the questions compiled in; `jev_ask` becomes opt-in there.
- **Probabilities**: carried in `jev_eval` / `jev_ask` answers (`->'probabilities'`), for P3's struct.
- **Bounded batches**: `snx_jev_max_batch_tokens` (24000, estimated at 3 bytes a token from the real
  request body) closes a batch before `snx_jev_batch_size`; `snx_jev_max_value_chars` (2000 code points)
  cuts every string before it is sent, and before the cache key is taken.
- Tests: `jev_ask.test` (78 assertions) against a mock that now rejects any malformed compact request.
- **Live check (2026-09-30, `jev-1.13.0`).** The Phase 1 set (50 hand-labelled prompts) went through
  `jev_ask` with the §1 taxonomy, batch 32: 2 requests, 5,676 input tokens, ≈ $0.0002.

  | | Phase 1 compact 32 | P2 `jev_ask` |
  |---|---|---|
  | Intent accuracy | 92% | 90% |
  | Malicious caught / false alarms | 10/10, 0 | 10/10, 0 |
  | Input tokens / prompt | 138 | 113.5 |

  Four of the five intent misses are Phase 1's known case: the intent of a purely malicious prompt
  reads `work_related`. The fifth is new (the pentest report template → `personal`, confidence 0.64).
  **The malicious margin narrowed:** non-malicious ≤ 0.13 as before, but malicious ≥ 0.61 (Phase 1:
  ≥ 0.92). The lowest is the one hidden inside a personal task. 0.5 still separates them. Suspect the
  noul pointer wording ("satisfy the condition in `rubric.q1`?") when the condition is phrased as a
  question. **P3:** tune the compiled wording for `snx_prompt_intent` and re-check with one 2-request run.

## P3 — `snx_prompt_intent(user_prompt)`, the only public function

**Done.** What landed, beyond the list below:

- `snx_prompt_intent(VARCHAR)`, `snx_jev_last_query_stats()` and `jev_version()` are always registered;
  everything else only with `SNX_JEV_ENABLE_GENERIC=1` at LOAD. Tests run the prompt-intent suite without it.
- Request layout back to Phase 1's measured shape for **every** function: named rubric entries
  (`rubric.intent`), keyed items (`items."3"`), pointers "Using rubric.X in the state, classify items."n"."
  and, for the malicious question, "... is items."n" malicious?". Questions carry a `name` (jev_ask: the
  caller's; snx_prompt_intent: intent / malicious) and an optional pointer template.
- Engine: batch 25; the rate limiter; single-flight (`JevState::LookupOrClaim` / `Complete`, with every
  claim completed even when the statement throws); the cache scope read at execute.

**Live runs on the Phase 1 set (`jev-1.13.0`, 2 requests each, ≈ $0.0008 for all three):**

| Layout | Intent | Malicious caught / false alarms | Lowest malicious / highest other | Tokens / prompt |
|---|---|---|---|---|
| Phase 1 compact 32 (spec §11) | 92% | 10/10, 0 | 0.92 / ≤ 0.2 | 138 |
| P2 (`rows[i]`, `q0` keys, "satisfy the condition") | 90% | 10/10, 0 | 0.61 / 0.13 | 113.5 |
| P3 first try (same, malicious as a statement) | 86% | 9/10, 0 | 0.37 / 0.13 | 113.5 |
| **P3 final (Phase 1 layout)** | **98%** | **10/10, 0** | **0.92 / 0.07** | 137.6 |

The ~20% token saving of the P2 wording cost the malicious margin, so the measured layout wins. The one
remaining intent miss is the known case (a phishing-email request reads `work_related`, P(mal) 0.98).

```sql
snx_prompt_intent(user_prompt VARCHAR)
  -> STRUCT(intent VARCHAR, intent_confidence DOUBLE,
            intent_probabilities MAP(VARCHAR, DOUBLE), malicious_probability DOUBLE)
```

- **One argument.** It is safe because the gateway never sets `llm.user_prompt_suppressed` or
  `llm.prompt_enc_key_id` (checked 2026-09-30 in sgwe `cpp/src/otel/snx_otel_log.cc`, which sends
  `req.prompt` without checking either). In ai_txn today, `user_prompt_suppressed` is always false
  and `prompt_enc_key_id` is always NULL.
- NULL in → NULL out, and nothing is sent.
- VARCHAR only: a whole row or struct cannot be passed.
- The questions, labels, descriptions and taxonomy version are compiled in; SQL cannot change what is asked.
- Read the cache scope (`scope_customer_id`) when the query **runs**, not at bind: DuckDB does not
  rebind a prepared statement when a variable changes, so a statement prepared under one customer and
  run after the connection is re-pinned would use the old customer's namespace.
- The generic `jev`, `jev_prob`, `jev_choice`, `jev_score`, `jev_score_norm`, `jev_confidence`,
  `jev_eval` and `jev_ask`, and the process-wide `jev_stats` / `jev_cache_clear`, are registered only
  when `SNX_JEV_ENABLE_GENERIC=1` is in the environment at LOAD (analysts' machines). Search never sets
  it, so none of them exists there. One binary, not a dev and a prod build.
- `snx_jev_last_query_stats()`: requests, tokens, prompts sent, cache hits and waits of the last query
  on **this connection** that used the extension, so search can report and log spend (`LlmSpend`).
- The live check's narrower malicious margin: the compiled malicious question is phrased as a
  condition (a statement), which is what the noul pointer ("satisfy the condition in ...") expects.
  Re-check with one 2-request run on the Phase 1 set.

### Throughput (decided 2026-09-30)

A breakdown ("alice's work vs personal") classifies every distinct prompt in the slice, so volume is
the constraint, not parallelism. At 25 prompts a request and ~113 tokens a prompt:

| Distinct prompts | Requests | Floor at 1,200 req/min | Cost |
|---|---|---|---|
| 1,000 | 40 | ~2 s | $0.005 |
| 100,000 | 4,000 | 3.3 min | $0.47 |

- **Batch 25** is the default `snx_jev_batch_size` (upstream measured accuracy dropping above ~20-25).
- **Parallel:** batches already go out on a process-wide pool (`snx_jev_concurrency`, 16), however many
  threads DuckDB scans with.
- **Rate limiter:** a process-wide token bucket (`snx_jev_max_requests_per_minute`, default 1000, below
  Jev's 1,200), so 16 parallel requests pace themselves instead of meeting 429s.
- **Single-flight:** when two queries (or two scan threads) meet the same uncached prompt at once, one
  sends it and the other waits for that answer, instead of paying twice.
- **Cap and estimate (search, P5):** at most **1,000 distinct prompts are sent per search**
  (`snx_jev_max_rows_per_statement = 1000`, set and locked in search's `connect.sql`). Above that,
  search classifies a deterministic sample of 1,000 distinct prompts (lowest `hash(user_prompt)`
  first, so the same question gets the same sample) and reports an estimate: "estimated from 1,000 of
  48,213 prompts: work 71% ± 3%" (95% interval at n = 1,000 is at most ±3.1 points). The user accepted
  estimates for large slices; exact counts at any size would need classifying prompts as they arrive
  (persisted labels), which is not planned.
- **Encryption, when sgwe implements it:** `user_prompt` would hold ciphertext. That leaks no
  plaintext, but it wastes calls on meaningless labels. Decide then between skipping rows that have
  a key id and keeping ciphertext in its own column.

## P4 — Distribution

- Release binaries (tag → CI) for linux_amd64, linux_arm64, osx_arm64 and windows_amd64.
- Bake the binary into the search image and `LOAD` it by path. The fallback is a custom extension
  repository in MinIO. Either way it is unsigned, so the loader needs `allow_unsigned_extensions`.
- Version rule: a DuckDB bump in analytics-schema (`duckdb.jdbc.version`) needs a matching
  snx_jev release first, the same ordering as the migrator.

## P5 — Wire into analytics-schema

- `services/search/conf/connect.sql`: `LOAD` it, set the caps, then `enable_external_access = false`
  and `lock_configuration = true`. `TYPESAFE_API_KEY` goes in search's deployment env.
- **Prove the key is unreadable** from generated SQL before enabling it: `read_text` /
  `read_blob` / `read_csv` on `/proc/self/environ` must fail. Search reads the lake from S3 and local
  caches, so `enable_external_access = false` needs `allowed_directories` / `allowed_paths` for those,
  and it must be tested with the real lake. If that cannot be made to work, change the key source
  (a file outside the allowed paths, or handed over by search's Java at startup) before rollout.
- Teach `ai_log_search.yaml` when to call `snx_prompt_intent(user_prompt)`; add eval cases against the mock.
- Breakdown query shape: classify **distinct** prompts, then join back and count; above 1,000 distinct,
  take the deterministic sample and scale, and say so in the answer (see P3 Throughput). The
  statement cap is the backstop if the generated SQL forgets to sample.
- CI guard: `http_client` stays banned; `snx_jev` is allowed only in search's connect.sql.
- Retire `ollylake/jev/jev_macros.sql` (branch `demo/prompt-jev-batch`).

## P6 (last) — Suppression guard in the lake

A separate analytics-schema PR from main. In `R__ai_txn_transform.sql`, `user_prompt` becomes
NULL when `llm.user_prompt_suppressed` is true. `ai_txn_schema.md` already documents
`user_prompt` as "NULL when absent, suppressed, or encrypted", but the transform copies
`req.prompt` whatever the flags say. With the guard, no suppressed prompt reaches any consumer,
not just Jev, once sgwe starts setting the flag. Until then it is a no-op.

## Found along the way

- sgwe `cpp/src/otel/snx_otel_log.cc:238` sets `req.srciptuple` to `h_req->prompt_get()`, the
  prompt text rather than an IP tuple, so every prompt is emitted twice. The lake does not read
  that attribute. Report it to the sgwe owners.
