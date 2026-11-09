# Logging — one file, one level, nothing on the deck

The harness has exactly one log: a JSON-lines file under the home, written by `include/dd/core/logbook.hpp`
and `src/core/logbook.cpp`. The TUI never reads it and it never writes to the TUI. That split is the
whole design: the deck is for the operator's eyes during a turn, the logbook is for the question asked
afterwards — *what did the harness do, when, and why*.

## Where it is and what moves it

```
<home>/logs/harness.jsonl          # one JSON object per line
<home>/logs/harness.jsonl.1        # the previous file, after 64 MiB
<home>/logs/knowledge.jsonl        # the router's own record (offers, opens, tokens) — unchanged
```

| lever | scope | values |
|---|---|---|
| `logging.level` in `config.yaml` | the file | `off` `error` `warn` `info` `debug` `full` |
| `DD_LOG` | the file, this run | same words; overrides the config |
| `logging.path` | the file | empty = `<home>/logs/harness.jsonl`; `~/…` expands |
| `/verbose off\|normal\|full` | deck **and** file | `off`→`error`, `normal`→`info`, `full`→`full` |
| `/log [word]` | the file alone | bare prints level, path, records written and lost |
| `display.verbose` | the deck at boot | `off` `normal` `full` (`DD_VERBOSE` overrides) |

A level named by `logging.level` or `DD_LOG` is kept: the display no longer overrules an operator who
asked for a quiet deck and a full file. `/verbose` is the exception — typing it is an instruction, and
it moves both surfaces.

## The levels, and what earns a line

| level | what lands |
|---|---|
| `off` | nothing. The file is not even created: the descriptor opens on the first record. |
| `error` | a tool that threw, a wire call that failed for good, a child that died, a turn that threw |
| `warn` | no mazemaker server, the pod not answering, a pod call that failed |
| `info` | `start`, `turn.start`, `turn.end`, delegate `child.start` / `child.end` / `batch` / `batch.start` (a background job) |
| `debug` | every tool call, every wire request/response, every router signal and every model open |
| `full` | and the heads of the payloads: arguments, request body, tool result (600–800 bytes, marked) |

`full` is what "maximum verbose" means here: the arguments a tool was called with, the body the model
was sent, the result it got back — the first few hundred bytes of each, with the rest counted rather
than silently dropped.

## A record

```json
{"t":1794180991.234567,"lvl":"info","cat":"tool","ev":"call","pid":4711,"tid":4712,
 "session":"dd-6af0f82d","model":"deepseek-v4.1","name":"read_file","ms":12.4,
 "ok":true,"out_bytes":812,"why":"read the prompt composer before editing it"}
```

`t` is unix seconds with microseconds; `pid`/`tid` say which thread (a delegate child has its own);
`session` and `model` ride every line; `why` is the model's own `intent` argument when the call carried
one. There is no free-text message field: a record that cannot name its fields is not worth a line.

## What it costs, and why there is no lock

Every thread may write — the turn, each delegate child, the router, the pod's poller, the tool
threads. A per-thread buffer behind a mutex would serialize them, and the harness's hand-offs are
lock-free by construction. So a record is formatted whole on the caller's stack and handed to a single
`::write()` on an `O_APPEND` descriptor: the kernel places the record under the inode's write lock, so
records never interleave and no writer waits for another. A record over 8 KiB is **cut**, not split: the
envelope survives, `"cut":true` and the real `bytes` are recorded, and the head rides along in `raw`.

`tests/core_logbook_test.cpp` holds that line: four threads × 300 records, every line must parse and
every record must be present exactly once.

## Rules the next change has to keep

1. **Never write to the TUI from a library.** A tool, an agent, a child or the pod has no business
   painting; it returns its error to the caller, and the caller decides. `std::cerr` in a hot path is
   a bug: while the deck owns the alternate screen it lands on top of the transcript.
2. **A log call is a fact, not a story.** One event, named fields, no prose. If a reader has to parse
   English to count retries, the record is wrong.
3. **Off costs nothing.** Call sites that would build an expensive field use `logbook::at`, which
   returns before evaluating the builder.
4. **A failed write is a counter.** `logbook::lost()` — never a message on the deck, never a throw from
   a logging path.