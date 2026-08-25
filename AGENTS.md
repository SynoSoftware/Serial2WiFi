# AGENTS.md

# Serial2WiFi

The implementation plan is the authority for behavior, architecture, milestones, and accepted limitations.

## Engineering standard

Optimize for:

* maximum elegance;
* zero cleverness;
* intention stated directly in code;
* one authority for each fact, state, and rule;
* explicit named state;
* no duplicated representations;
* no speculative abstractions.

Before introducing any method, helper, type, field, task, lock, queue, persisted value, module, or abstraction, ask:

> What concrete requirement or runtime invariant forces this to exist?

If there is no clear answer, simplify or remove it.

Working code and passing tests do not justify unnecessary structure.

## Cohesion

Minimalism applies to **responsibility, not file count**.

A file may be large if it owns one coherent responsibility.

Do not:

* split files merely to reduce line count;
* add unrelated behavior to an already mixed-responsibility file;
* let `main.cpp` become a dumping ground;
* introduce architecture merely to make the project look organized.

If touched code has accumulated unrelated responsibilities, reduce that mixing when it can be done within the current milestone without introducing new architecture.

## C++ style

Use simple embedded C++.

Prefer:

* structs;
* enums;
* functions;
* fixed-capacity buffers;
* explicit state machines.

Avoid unless a requirement forces them:

* inheritance;
* polymorphism;
* generic managers/services/controllers;
* factories;
* unnecessary templates;
* dynamic allocation in sustained paths;
* abstractions for hypothetical reuse.

Treat vague names such as `Manager`, `Helper`, `Util`, or generic `Service` as design smells. Prefer names that state the exact responsibility.

## Comments

Comments explain **why this code must exist or be ordered this way**, not what the code does.

A future maintainer should be able to distinguish:

* arbitrary implementation detail that can be changed freely;
* product/hardware/runtime invariants that must be preserved.


## Comments — preserve the why

Comments explain **why this code must exist or be ordered this way**, not what the code does.

Preserve important product, hardware, and runtime rationale in the code when it would otherwise be easy for a future maintainer to “simplify” the implementation incorrectly.

Typical cases include:

* ESP32 boot-strap and GPIO0 behavior;
* why UART0 must never be used for logging;
* why a queue is retained or cleared at a transition;
* why a lock must be released before I/O;
* why display loss is acceptable but transport loss is not;
* why configuration is persisted before runtime transition;
* why ordering is required;
* why a seemingly simpler implementation would violate an invariant.

Do not comment obvious mechanics.

A future maintainer should be able to distinguish:

* implementation detail that can be changed freely;
* product, hardware, and runtime invariants that must be preserved.

Important rationale from the implementation plan must not disappear merely because the code can compile without it.

## Ownership and concurrency

Each mutable concern has one authority.

Never:

* create competing configuration or lifecycle paths;
* hold locks during TCP, UART, OLED/I²C, or NVS operations;
* perform network, HTTP, NVS, or display work from the UART receive callback;
* let display work delay serial transport.

Prefer losing non-critical display history over delaying transport.

Add concurrency only when a requirement forces it.

## Serial integrity

UART traffic is product data.

Never log to UART0.

`Serial.write()` is only for forwarding NET→SER payload.

Do not transform transported bytes:

* no line-ending conversion;
* no protocol framing;
* no timestamps;
* no generated responses;
* no debug output.

## Frontend

Use only:

* semantic HTML;
* CSS;
* vanilla JavaScript.

Do not add Node, Bun, React, TypeScript, Fluent libraries, CDNs, or frontend build tooling.

Implement the specified Fluent interaction and accessibility behavior directly.

## Scope

Do not add:

* speculative future features;
* unrelated refactors;
* compatibility behavior not required by the plan;
* abstractions for anticipated reuse;
* protocol-specific behavior.

Implement milestones in plan order.

## Verification

There is currently no physical device available.

Run every check that does not require hardware:

* builds;
* unit/native tests;
* static checks;
* deterministic state-machine/configuration/UI tests.

Mark hardware-dependent checks as pending.

Never claim hardware verification without hardware.

Tests exist to catch regressions, not to maximize coverage. Do not build elaborate mocks or test infrastructure unless a concrete risk requires it.

**Review model policy**

* Routine milestone reviews: **Luna or Haiku**.
* Use **Terra or Sonnet** only when the reviewer finds something ambiguous, architectural, contradictory, or difficult to resolve from the plan and code.
* Final integrated review may use **Terra or Sonnet** for at least one pass.

After each milestone:

1. Run a lightweight **correctness / plan-compliance** review.
2. Run a lightweight **elegance / zero-cleverness** review.
3. Run a lightweight **embedded-runtime** review only when the milestone changes concurrency, UART/TCP, queues, timing, persistence, reset, Wi-Fi lifecycle, or other hardware-facing behavior.
4. Fix verified findings.
5. Run one lightweight convergence review.

Escalate a finding to Terra or Sonnet only when it cannot be resolved confidently from the plan and code.

After the final milestone, run all three reviews across the integrated system, with at least one medium-model review.

That is much more cost-efficient. Luna/Haiku should be perfectly adequate for “find obvious violation / unnecessary abstraction / missing branch” passes. Terra/Sonnet should be reserved for actual reasoning disputes.


## Repository safety

Preserve local work.

Never run without explicit user approval:

```text
git restore
git reset
git clean
git checkout --
```

Do not delete build/dependency caches as a first troubleshooting step. Prefer diagnosis and targeted repair.

## Commands

Development is Windows-first.

Use simple, repeatable Windows-safe commands.

Do not assume Linux tooling or shell behavior.

Avoid fragile nested one-liners when a small script is clearer.

## Stop conditions

Stop and report instead of inventing a solution if:

* the plan contradicts itself;
* a required decision is missing;
* an API cannot support the required behavior;
* a hardware assumption invalidates the design;
* requirements conflict.

State the exact conflict and the minimum decision needed to continue.

## Final standard

The code should make the product obvious:

```text
serial ⇄ transport ⇄ TCP
        +
      display
        +
   configuration
```

If understanding the code requires an abstraction the product does not require, simplify it.



