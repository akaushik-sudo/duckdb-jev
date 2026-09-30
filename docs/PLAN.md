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

## P1 — Harden

- The API host is fixed when the extension loads, and nothing in SQL can change it. The mock URL
  is accepted only through an env var, for tests.
- The key comes only from `TYPESAFE_API_KEY` (or a DuckDB secret). There is no `jev_api_key`
  setting, so no query can read the key back through `current_setting()` / `duckdb_settings()`.
- `customer_id` goes into the cache key (argument or setting — decide in the PR).
- Tests: a `SET` of the URL fails, the key cannot be read back, and a planted host is refused.

## P2 — What our questions need

- `choice` criteria carry descriptions (label → description), not bare labels.
- Several questions per row share one request, so intent and malicious cost one call.
- Compact layout: the rubric goes in the state once, and each per-row question is a pointer
  (−70% tokens in Phase 1).
- Return the choice's `probabilities`.
- Close batches by estimated tokens as well as row count; truncate each prompt at 2,000 chars.
- Done when: the mock tests pass, and a manual live check reproduces Phase 1 (≈138 input tokens per prompt).

## P3 — `snx_prompt_intent(user_prompt)`, the only public function

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
- The generic `jev`, `jev_prob`, `jev_choice`, `jev_score`, `jev_score_norm`, `jev_confidence`
  and `jev_eval` are **not registered**. `jev_stats` / `jev_cache_clear` stay off the search path too.
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
- Teach `ai_log_search.yaml` when to call `snx_prompt_intent(user_prompt)`; add eval cases against the mock.
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
