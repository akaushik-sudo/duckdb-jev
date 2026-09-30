#!/usr/bin/env python3
"""Deterministic stand-in for the TypeSafe Jev API, used by the regression tests.

Requests use the compact layout the extension sends:

  state     = {"rubric": {"q0": {...}, "q1": {...}}, "rows": [...]}
  questions = {"r<row>_q<k>": {"type": ..., "instructions": ..., "criteria": ...}}

The mock is strict about that shape: a question key that names a missing row or rubric
entry, a type that disagrees with its rubric entry, or criteria that differ from the
rubric's labels answer 422, so a request-building bug fails the tests instead of
passing them.

The rules are fixed so the expected results in test/sql/*.test never move:

  noul   -> 0.9 when the LAST word of the rubric entry's condition appears
            (case-insensitively) in the row JSON, else 0.1
  score  -> level index  = length of the row JSON modulo the number of levels
  choice -> option index = length of the row JSON modulo the number of options

  A rubric text containing "trigger422" answers 422 (a non-retryable error).
  A rubric text containing "trigger503" answers 503 (retryable, so it exhausts the retries).
  usage.input_tokens = len(request body) // 4

Every answer also carries fields a real answer does not, so the tests can see what was sent:

  mock_described  choice: how many options arrived with a description;
                  noul: whether true/false descriptions arrived
  mock_row_chars  when the row is a plain string, its length in characters
  mock_batch_rows how many rows the request carried

Run: python3 test/mock_api.py [port]        (default 18765)
"""
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API_KEY = "jev-test-key"
QUESTION_KEY = re.compile(r"^r(\d+)_(q\d+)$")


class BadRequest(Exception):
    pass


def rubric_text(entry):
    return entry.get("condition") or entry.get("question") or ""


def answer_one(kind, entry, question, row, batch_rows):
    row_json = json.dumps(row, sort_keys=True)
    extra = {"mock_batch_rows": batch_rows}
    if isinstance(row, str):
        extra["mock_row_chars"] = len(row)

    if kind == "noul":
        words = rubric_text(entry).split()
        needle = words[-1].lower() if words else ""
        hit = bool(needle) and needle in row_json.lower()
        extra["mock_described"] = "true_means" in entry and "false_means" in entry
        return {"type": "noul", "noul": 0.9 if hit else 0.1, **extra}

    if kind == "score":
        levels = question.get("criteria")
        if levels != entry.get("levels") or not levels:
            raise BadRequest("score criteria differ from the rubric levels")
        chosen = len(row_json) % len(levels)
        return {
            "type": "score",
            "score": float(chosen),
            "legend": {str(i): level for i, level in enumerate(levels)},
            "probabilities": {str(i): (1.0 if i == chosen else 0.0) for i in range(len(levels))},
            "confidence": 1.0,
            **extra,
        }

    if kind == "choice":
        criteria = question.get("criteria")
        rubric_options = entry.get("options")
        if not isinstance(criteria, dict) or not isinstance(rubric_options, dict):
            raise BadRequest("choice criteria and rubric options must be objects")
        if list(criteria.keys()) != list(rubric_options.keys()) or not criteria:
            raise BadRequest("choice criteria differ from the rubric labels")
        options = list(criteria.keys())
        chosen = len(row_json) % len(options)
        extra["mock_described"] = sum(1 for d in rubric_options.values() if d)
        return {
            "type": "choice",
            "choice": options[chosen],
            "probabilities": {o: (1.0 if i == chosen else 0.0) for i, o in enumerate(options)},
            "confidence": 1.0,
            **extra,
        }

    raise BadRequest("unknown question type " + repr(kind))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # keep-alive, so connection reuse is exercised

    def log_message(self, *args):  # quiet
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.headers.get("Authorization", "") != "Bearer " + API_KEY:
            return self._send(401, {"error": "invalid api key"})
        try:
            request = json.loads(body)
            state, questions = request["state"], request["questions"]
            rubric, rows = state["rubric"], state["rows"]
            texts = " ".join(rubric_text(entry) for entry in rubric.values())
            if "trigger422" in texts:
                return self._send(422, {"error": "mock validation failure"})
            if "trigger503" in texts:
                return self._send(503, {"error": "mock overloaded"})

            expected = {"r%d_%s" % (i, q) for i in range(len(rows)) for q in rubric}
            if set(questions) != expected:
                raise BadRequest("questions must be exactly one per (row, rubric entry)")

            answers = {}
            for question_id, question in questions.items():
                match = QUESTION_KEY.match(question_id)
                index, rubric_key = int(match.group(1)), match.group(2)
                entry = rubric[rubric_key]
                if question.get("type") != entry.get("type"):
                    raise BadRequest(question_id + ": type differs from its rubric entry")
                if "`rubric.%s`" % rubric_key not in question.get("instructions", ""):
                    raise BadRequest(question_id + ": instructions do not point at its rubric entry")
                if "`rows[%d]`" % index not in question.get("instructions", ""):
                    raise BadRequest(question_id + ": instructions do not point at its row")
                answers[question_id] = answer_one(entry["type"], entry, question, rows[index], len(rows))
        except (BadRequest, KeyError, TypeError, ValueError, AttributeError) as error:
            return self._send(422, {"error": "mock: bad request: " + str(error)})

        self._send(
            200,
            {
                "model": request.get("model", "jev-mock"),
                "answers": answers,
                "usage": {"input_tokens": len(body) // 4, "output_tokens": len(answers)},
            },
        )

    def _send(self, code, payload):
        data = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18765
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
