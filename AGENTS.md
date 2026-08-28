# Serial2WiFi

Plans are historical reference material only. They are not authority. The owner's request is authority.

## Engineering standard

Optimize for:

- maximum elegance;
- zero cleverness;
- intention stated directly in code;
- one authority for each fact, state, and rule;
- explicit named state;
- no duplicated representations;
- no speculative abstractions.

Before introducing any method, helper, type, field, task, lock, queue, persisted value, module, dependency, or abstraction, ask:

> What concrete requirement or runtime invariant forces this to exist?

Search the repository for the same concept under another name. Extend the existing owner when its responsibility and lifetime match. Create another owner only when a real boundary requires it.

If there is no clear answer, simplify or remove it. Working code and passing tests do not justify unnecessary structure.

## Working safely

The owner may be editing the repository at the same time.

- Preserve all existing and concurrent user changes.
- If a file changes while you work, reread it and incorporate the new state. Never restore your earlier copy over it.
- Inspect the working tree before editing and the complete touched diff before finishing.
- Never run destructive Git commands without explicit instruction. This includes `git restore`, `git reset`, `git clean`, and `git checkout --`.
- Do not stage, commit, pull, push, or rewrite history unless explicitly requested.
- Treat repository constraints as requirements, not obstacles to bypass.
- If no safe compliant solution is available, stop and report the exact blocker and the smallest decision needed from the owner.

## Cohesion

Minimalism applies to responsibility, not file count.

A file may be large if it owns one coherent responsibility.

Do not:

- split files merely to reduce line count;
- add unrelated behavior to an already mixed-responsibility file;
- let `main.cpp` become a dumping ground;
- introduce architecture merely to make the project look organized.

If touched code has accumulated unrelated responsibilities, reduce that mixing when it can be done within the current milestone without introducing new architecture.

## C++ style

Use simple embedded C++.

Prefer:

- structs;
- enums;
- functions;
- fixed-capacity buffers;
- explicit state machines.

Avoid unless a requirement forces them:

- inheritance;
- polymorphism;
- generic managers, services, or controllers;
- factories;
- unnecessary templates;
- dynamic allocation in sustained paths;
- abstractions for hypothetical reuse.

Treat vague names such as `Manager`, `Helper`, `Util`, or generic `Service` as design smells. Prefer the shortest name that states the exact domain responsibility. Boolean names read as facts. Async functions end with `Async` where that convention applies.

## Comments — preserve the why

Comments explain why code must exist or be ordered this way, not what the code does.

Preserve important product, hardware, and runtime rationale when a future maintainer could otherwise “simplify” the implementation incorrectly. Typical cases include:

- ESP32 boot-strap and GPIO0 behavior;
- why UART0 must never be used for logging;
- why a queue is retained or cleared at a transition;
- why a lock must be released before I/O;
- why display loss is acceptable but transport loss is not;
- why configuration is persisted before a runtime transition;
- why ordering is required;
- why a seemingly simpler implementation would violate an invariant.

Do not comment obvious mechanics. A future maintainer should be able to distinguish implementation detail that may change freely from product, hardware, and runtime invariants that must be preserved.

Important rationale from the active implementation plan must not disappear merely because the code can compile without it.

## Ownership, state, and concurrency

Each mutable concern has one authority.

- Compute derived facts from their authority. Do not mirror or persist derived state merely for convenience.
- Make every task, timer, callback registration, session, queue, and subscription's owner and lifetime explicit.
- Sequence all effects of one logical action in one obvious place.
- Never create competing configuration, default, or lifecycle paths.
- Never hold locks during TCP, UART, OLED/I²C, NVS, or other blocking I/O.
- Never perform network, HTTP, NVS, or display work from the UART receive callback.
- Never let display work delay serial transport.
- Add concurrency only when a requirement forces it.

Prefer losing non-critical display history over delaying transport.

## Boundaries, failures, and secrets

Treat HTTP, WebSocket, Wi-Fi, configuration, NVS, and serial-control input as untrusted.

- Normalize and validate external input once at its owning boundary.
- Convert boundary values to explicit internal state before other code consumes them.
- Do not spread raw or partially validated payloads through the system.
- Never log passwords, Wi-Fi credentials, session tokens, CSRF tokens, or terminal payloads.
- Never return stored passwords through an API.
- Never swallow a failure. Handle it completely, propagate it, or expose an explicit state the caller can act on.
- Expected failure must not become an ambiguous boolean, silent no-op, or exception-driven control path.
- Do not claim that state was saved, retained, or applied unless the corresponding state actually survives at the owning boundary.

## Serial integrity

UART traffic is product data.

Never log to UART0.

`Serial.write()` is only for forwarding NET→SER payload.

Do not transform transported bytes:

- no line-ending conversion;
- no protocol framing;
- no timestamps;
- no generated responses;
- no debug output.

## Frontend

Use only:

- semantic HTML;
- CSS;
- vanilla JavaScript.

Do not add Node, Bun, React, TypeScript, Fluent libraries, CDNs, or frontend build tooling.

Implement the specified Fluent interaction and accessibility behavior directly.

### Accessibility

- Prefer native HTML elements and behavior over custom substitutes.
- Preserve keyboard operation, visible focus, associated labels, appropriate control semantics, and screen-reader announcements.
- An icon-only control requires an accessible name.
- Do not use color as the only indication of state; pair it with text or an icon.
- Tooltips may provide secondary explanation but must not contain information required to operate the interface.

### Theme and color

- Define colors once as semantic CSS custom properties.
- Components consume semantic roles; raw color literals belong only in the central theme definitions.
- Do not introduce feature-local or one-off color variables when an existing semantic role fits.
- Define every new semantic role for every supported theme in the same change.
- Never repair one theme with a literal override at the call site.

### Buttons

A button is a Windows command. Label it the way Windows labels it.

- Use the Windows word. `Sign in`, not `Login`. `Retry`, not `Try again`. `Forget`, not `Forget network`. `Add`, not `Enter manually`. If Windows or the Microsoft Writing Style Guide has a word for the command, that word wins.
- Use one word: `Save`, `Cancel`, `Connect`, `Retry`, `Scan`, `Add`, `Change`, `Forget`, `Back`, `Done`, `Close`, `Send`, `Clear`, `Pause`, `Resume`, `Create`.
- Use two words only where the Microsoft term is two words and has no one-word form: `Sign in`, `Sign out`. Never add words merely to disambiguate; the heading, row label, or accessible name identifies the object, and the button identifies the command.
- Command buttons share one height and one `min-width`, defined once on the `button` rule in `style.css`. Icon-only buttons, tabs, list rows, and the smaller terminal command bar may opt out explicitly.
- Labels never wrap or clip. Layouts must allow localized labels to expand. A label that changes at runtime reserves enough width for its longest localized state so the change does not move the layout.
- Every command button carries a Lucide icon. A button with no visible label is icon-only and needs an `aria-label`.
- One icon means one thing. When a label changes at runtime, choose its label and icon in the same expression so they cannot drift.
- Icons come from lucide.dev. Add the symbol to the sprite in `index.html` using path data from the Lucide source. Never draw one by hand.

### Product text

Follow the Microsoft Writing Style Guide.

- Sentence case. Second person. Plain verbs.
- Say what happened, then what the user does about it. One idea per sentence.
- Name the thing on screen: the network, the address, the setup network. Never use “here,” “either,” or “afterwards” where a noun fits.
- State a rule the firmware follows, not a promise the page cannot keep.
- Use `Wi-Fi`, `sign in`, and `sign out`. Never use `WiFi`, `login`, or `logout` in product text.
- In every rendered state, each fact, status, instruction, and action has one visible owner. No screen repeats the same visible meaning twice. A title names the state; supporting text adds only a cause, consequence, useful detail, or necessary next step; a button names the action, and no text beside it paraphrases that action.
- When two elements say the same thing, delete the redundant element, key, or visibility branch. Never reword both so they can coexist, and never compare rendered strings or add a deduplication mechanism.
- Never shorten text to the point where it needs a neighbouring element to be understood, and never leave a meaning to colour, position, punctuation, timing, or implementation knowledge alone. Accessibility text may repeat visible meaning where assistive technology needs it.

### Localization

Locale catalogs are the sole authority for frontend-owned user-facing text. English (`en`) is the complete default locale.

- Never hardcode visible or accessibility text in HTML, JavaScript, or CSS. This includes labels, buttons, headings, notices, errors, validation messages, placeholders, tooltips, document titles, `aria-label` values, and live-region announcements.
- Add or change every supported locale in the same change. Every catalog must contain exactly the same keys and placeholders.
- Keep complete messages in the catalogs. Never construct sentences by joining translated fragments.
- Catalog values are text, not HTML. Insert them with `textContent` or the appropriate DOM property or attribute, never `innerHTML`.
- Generated locale assets are derived artifacts, not a second authority. Never edit them by hand. Their generation must be deterministic.
- Every generated locale asset referenced by the page must exist in `data/`, be current, and be included in the LittleFS image before the change is complete.
- Missing catalogs, missing or orphaned keys, placeholder mismatches, stale generated assets, and frontend-owned hardcoded text must fail an automated verification check.
- Never silently render an empty string or expose a localization key. An unsupported or unavailable locale falls back to the complete default catalog.
- Set the document `lang` attribute to the active locale.
- Localization must not depend on a network service, CDN, or asset unavailable from the device.
- Protocol values, API identifiers, user-provided content, and terminal data are not localized.

## Scope

Make the smallest complete change that satisfies the request.

Do not add:

- speculative future features;
- unrelated refactors;
- compatibility behavior not required by an external contract or active requirement;
- abstractions for anticipated reuse;
- protocol-specific behavior not requested by the product;
- dependencies not forced by a current requirement.

If the task exposes or would worsen competing implementations of the same behavior in the same subsystem, converge them on the existing owner within the task. Report duplication outside the touched subsystem instead of widening scope.

Implement milestones in plan order.

## Verification

There is currently no physical device available.

Run every relevant check that does not require hardware:

- builds;
- unit and native tests;
- static checks;
- deterministic state-machine and configuration tests;
- frontend syntax, localization, accessibility, and asset checks.

Additionally:

- Do not run overlapping builds for the same PlatformIO environment.
- Do not leave mock servers, monitors, or background builds running.
- Finish with no new warnings, errors, or active diagnostics caused by the change.
- Review the complete touched diff for unintended files, stale generated assets, hardcoded frontend text, raw component colors, duplicated ownership, and unnecessary structure.
- Report exactly which checks ran and passed.
- Mark hardware-dependent checks as pending and state the behavior they leave unproven.
- Never weaken, suppress, or bypass a check merely to obtain a passing result.
