# OVERVIEW — Ariadne from the inside out

A short map, ordered from the core outward. Every claim here is expanded
somewhere else in this tree; this page is the route, not the reference.

## The core: one loop

Ariadne is a native C++20 agent harness: one binary, one execution loop
(`src/agent/agent.cpp`), one OpenAI-compatible wire. The model asks for a tool,
the registry validates and runs it, the result goes back on the tape, and the
loop continues until there is an answer or a stop condition. The interactive
TUI, the headless agent, and every `delegate_task` child run that same loop —
there is no second orchestration hiding behind the interface.

## Around the core

Ordered inside out: each layer exists to control what reaches the loop, and
what survives after a turn ends.

- **Context** — a managed data plane (`src/context/`): rendered once per
  epoch, appended thereafter, rebuilt from explicit state when it grows past
  its watermark. The window is working capacity; nothing pretends it is
  memory, and every cut is accounted rather than silent.
- **Knowledge** — one retrieval path (`src/know/`): local BM25F over skill and
  curated-memory cards, plus deadline-bounded recall from MazeMaker. It offers
  pointers, not payloads — the agent opens what it needs. What has to outlive
  the session lives outside the window (`docs/KNOWLEDGE.md`).
- **Tools** — one registry (`src/tools/`): filesystem, terminal, live MCP
  discovery, and sub-agents that get a fresh loop with a narrowed toolset.
  Output is bounded before it enters the context. The honest line: the
  terminal tool is not a sandbox.
- **Surfaces** — all of them drive the same core: the cell-diff TUI
  (`src/ui.cpp`), the headless `ariadne-agent`, the device gateway
  (`src/gateway/`), and cron. Presentation is a layer, not a second runtime.
- **Inference** — any OpenAI-compatible endpoint (`src/agent/http_llm.cpp`),
  local or remote; `ariadne limbo` (`src/limbo/`) runs the machine's own stack
  when you want it local. The model is replaceable, the endpoint is
  replaceable — the execution state is not allowed to become accidental.

## See it run

```bash
make                        # build/ariadne, build/ariadne-agent, build/ariadne-skills
build/ariadne --init        # write config.yaml + folders into the resolved home (never build/ itself)
build/ariadne               # the interactive harness

printf '%s\n' 'List files in /tmp/' | build/ariadne-agent   # headless, one message

build/ariadne-skills list   # the skill catalogue CLI
```

Requirements: Linux, `g++` with C++20, GNU Make, libcurl, and a running
OpenAI-compatible endpoint (`config.yaml` in the selected home; the reference
configuration is `examples/ariadne/config.yaml`).

## Two checks, not two promises

```bash
make test        # every registered test binary; green = "0 test binary(ies) failed"
make doc-audit   # docs are checked claims: product wiring + every path citation
```

`make doc-audit` is why this page stays short: a citation that drifts or a
module mislabelled as wired fails the same gate a broken test does. What is
built, tested and measured today is recorded in `docs/PARITY.md`; the
measurements themselves live in `README.md` §8.

## Where to go deeper

| Page | What it holds |
|---|---|
| [README.md](../README.md) | the full tour: architecture, measurements, boundaries, configuration |
| [ENGINEERING_README.md](../ENGINEERING_README.md) | the engineering thesis, execution model, invariants |
| [docs/KNOWLEDGE.md](KNOWLEDGE.md) | the retrieval path in detail |
| [docs/LEARN.md](LEARN.md) | the learning journal and skill lifecycle |
| [docs/LOGGING.md](LOGGING.md) | the one JSON log |
| [docs/PARITY.md](PARITY.md) | what works today, with line-anchored evidence |

Working in this tree rather than reading it? [AGENTS.md](../AGENTS.md) is the
build, test and honesty protocol.
