# AGENTS.md

This file provides guidance for LLM Agents when working with code in this repository.

Ariadne is a native C++20 agent harness (a port of the Python "Daedalus" agent): one agent loop, an OpenAI-compatible inference wire, a context "tape", a knowledge router, MCP tools, a cell-diff TUI, and external long-term memory through MazeMaker. Namespace is `dd::`, headers live in `include/dd/<module>/`, sources in `src/<module>/`.

## Build

Plain GNU Make + g++ (mold used automatically if installed). Links libcurl, OpenSSL, OpenMP, pthreads. Flags are `-std=c++20 -Wall -Wextra -Werror -O2`, so **any warning breaks the build**.

```bash
make                      # all three products: build/ariadne, build/ariadne-agent, build/ariadne-skills
make build/ariadne        # fast loop: TUI/harness binary only
make install              # installs ariadne into $(INSTALL_DIR) (default ~/ariadne); install-all adds agent + skills
make clean
```

- **Whole-target builds, no object files.** Each binary/test is one `g++` invocation over every source it needs. Editing a shared `.cpp` or any header recompiles every dependent binary from scratch, so `make test` from cold takes roughly 20–25 min. Plan for that.
- Products: `src/dd_tui.cpp` → `ariadne` (interactive TUI, also `serve`/gateway, and `ariadne limbo` for the local inference stack), `src/dd_agent_main.cpp` → `ariadne-agent` (headless, message from stdin/args), `src/cli_main.cpp` → `ariadne-skills` (skill catalogue CLI: `list`, `replay`, …).
- Source groups (`CORE`, `AGENT`, `CONTEXT`, `KNOW`, `LIMBO`, …) are variables at the top of `Makefile`. A new `.cpp` must be added to the right group, or it is not linked into anything.
- `src/core/default_config.cpp` is **generated** (gitignored) from `examples/ariadne/config.yaml`, byte for byte. Edit the YAML, never the `.cpp`.
- `build/` is make output, never a runtime home. The binary refuses to use it as one.

## Tests

No test framework. Each `tests/*_test.cpp` is a standalone binary with a local `CHECK` macro that returns non-zero on failure. Rules for test binaries live in `Makefile` and in `mk/*.mk` (one fragment per subsystem, see `mk/README.md`). Each fragment appends to `TEST_BINS`.

```bash
make test                 # builds + runs every TEST_BINS binary serially, continues past failures; green = "0 test binary(ies) failed"
make tsan                 # ThreadSanitizer twins (TSAN_BINS), halt_on_error=1; not part of `make test`
make doc-audit            # bench/doc_audit.py: wiring + path:line citation check; must be clean

# a single test: target names don't follow the file names, so find the rule first
grep -rn 'context_test.cpp' Makefile mk/      # -> $(BUILD)/dd_context_tests
make build/dd_context_tests && DD_TEST_TMP=build/tmp ./build/dd_context_tests
```

- Tests must use `DD_TEST_TMP` for scratch space, never `/tmp`.
- Product-level probes (not in `make test`): `make runtime-probe RUNTIME_BASE_URL=…` (or `sh tests/runtime_probe_with_stub.sh`), `make asan-product`, `make interrupt-probe` (needs a pty), `make build/dd_delegate_tsan && ./build/dd_delegate_tsan`. Test doubles live in `tests/fake_*.{hpp,sh}` and `tests/fixtures/`.
- If a product looks stale after a "successful" make, check `find src include -newer build/ariadne`.

## Doc honesty is enforced by a tool

`bench/doc_audit.py` treats the docs as checkable claims:
1. **Wiring.** It derives which `src/*.cpp` / `include/dd/**` are reachable from the three product recipes and compares that against README's "Ported but not wired" list. Being in the tree does not mean being in a binary. Many ported units are deliberately unwired.
2. **Citations.** Every backticked `path:line[-line]` in `README.md`, `ENGINEERING_README.md`, `docs/*.md`, `mk/README.md`, `plan.md`, `ARIADNE-GATEWAY-PLAN.md`, `BLOG.md` must resolve, and the cited lines must share a token with the citing line.

So when you insert or remove lines in heavily cited files (`src/dd_tui.cpp`, `src/agent/http_llm.cpp`, `src/tools/delegate_tool.cpp`, `src/ui.cpp`, …), re-run `make doc-audit` and re-point the anchors you shifted. Docs cite exact lines, and the repo treats a drifted anchor as a bug.

## Architecture (big picture)

- **One loop.** `Agent::run` (`src/agent/agent.cpp`) is the only orchestration. The TUI, the headless agent, and `delegate_task` children all run it. The model asks for a tool, the registry validates and runs it, the result is appended, and the loop continues until a final answer or a stop condition.
- **Wire** (`src/agent/http_llm.cpp`, `chat_stream.cpp`, `retry.cpp`): OpenAI-compatible `/v1` streaming. A turn ends at the SSE `done()` terminator, not EOF. Retries only happen while nothing has been emitted yet (408/429/5xx and refused connects, using a rate ladder with `Retry-After`). `model.extra` is merged into the body *before* the harness's own fields, so it can't override `model`/`messages`/`stream`.
- **Context tape** (`src/context/`, `src/agent/payload_tape.cpp`, `kv_watermarks.cpp`): the prompt is rendered once per *epoch*, then append-only so the previous request stays an exact prefix of the next (KV/prefix-cache reuse). When a watermark is crossed, it *rebases* from named state (current question, anchor, turn ledger, one recall, protected tail), with no summarizer. Don't break prefix stability within an epoch.
- **Knowledge** (`src/know/`): local BM25F over skill and curated-memory cards plus deadline-bounded MazeMaker recall (`src/context/recall.cpp`, `maze.cpp`). It offers pointers, not whole documents. `src/learn/` is the background learning journal and skill lifecycle (probation, provenance, tombstones).
- **Tools** (`src/tools/`): fs tools reject `..`. The terminal has a denylist and **is not a sandbox**. MCP tools are discovered live (`mcp_client.cpp`, schema cache keyed by sha256 of config). `delegate_task` children get a fresh agent and transport, inherit only the wire config, shell denylist, and ToolGate, and stop at depth 2. A child must never touch the parent's UI (`on_wait` stays unset).
- **TUI** (`src/ui.cpp`, `ui_simd.cpp`, `ui_grammar.cpp`, wired in `src/dd_tui.cpp`): render-thread-owned scene, cell canvas plus frame diff, SIMD kernels picked at runtime. Syntax colours come from `skills/highlight.yaml`, not compiled tables.
- **Concurrency invariant:** `src/` and `include/` contain **no `std::mutex`, `lock_guard`, or `condition_variable`**. Hand-off uses fixed-slot rings (`core/slot_ring.hpp`), atomics, snapshots (`snap_cell.hpp`), and `MarkSet`. The only locks are `flock` on files (`core/fslock.hpp`, gateway device store). Keep it that way. Use the TSan targets to back up any new cross-thread claim.
- **Other modules:** `src/gateway/` (device gateway: pairing, identity, AES-GCM, relay, authz before body parsing), `src/serve/` + `src/net/` (HTTP server/chat API, URL safety), `src/limbo/` (`ariadne limbo`: the local inference stack — setup, start/stop, bench, mazemaker's AFE model), `src/cron/`.

## Runtime / config

- Config is `config.yaml` in the selected home (reference: `examples/ariadne/config.yaml`). Env overrides include `DD_BASE_URL` (must end in `/v1`), `DD_MODEL`, `DD_API_KEY`, `DD_MAX_TURNS`, `DD_WIRE_ATTEMPTS`, `DD_WIRE_BUDGET_MS`, `DD_TAPE_LOG`, `DD_FAST_LAYER`, `DD_NO_SOAK=1`, `DD_LOG` (see `ENGINEERING_README.md` § Configuration).
- One log: `<home>/logs/harness.jsonl` (JSON lines, level via `logging.level` / `DD_LOG`). The TUI never reads it. See `docs/LOGGING.md`.

## Working conventions in this repo

- `plan.md` is the active build plan, and its build protocol is the expected workflow: read the seam from source (line numbers in docs drift), change it, `make test`, **run the changed thing and look at its output**, record measured numbers in `docs/PARITY.md` / claims in `README.md`, then one commit per wave. Anything needing an operator decision (permissions, spending money, choosing a backend) is out of scope without asking.
- `docs/PARITY.md` tracks parity with the Python harness. Provenance paths into the Python tree are written `py:` and are not checked by the audit.
- Numbers in the docs are measurements with provenance (machine, model, config). Don't restate or invent performance claims without a run.
