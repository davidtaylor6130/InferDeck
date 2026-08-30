# Wayfinder: Dashboard UX overhaul

## Route status

`ROUTE_CLEAR`

## Destination

At 320px, 768px, and 1440px widths, an InferDeck operator can identify the loaded model, live processing state, hardware pressure, combined usage, and recent activity from Home; reach every current or planned capability from one navigation control; and find, review, name, install, and track a compatible model without losing context or opening a browser prompt.

## Constraints

- Preserve the existing restrained dark control-room visual language.
- Keep the dashboard an administration surface; do not add media capture, playback, fake jobs, or unsupported backend controls.
- Use the existing control API and real server data. No new service or dependency.
- Image, Music, and Post Training are honest preview sections until their runtimes exist.
- Preserve authentication, SSE state, cancellation, destructive confirmations, configuration recovery, and the user's unstaged `opencode.json` preferences.
- Do not deploy, push, or change production.

## Current frontier

- None. The route is ready for implementation.

## Fog

- Real Windows hardware data and long production model catalogues are unavailable locally; verify typical, empty, error, and long-name fixtures instead.
- The in-app browser currently exposes no controllable browser, so rendered visual balance remains `INCONCLUSIVE` until browser access returns.

## Baseline UX audit

- [P1] Home / current workload — the loaded model is a small header label that disappears on narrow screens, while eight equal counters lead the page → make runtime state, loaded model, running work, and queue the first reading order.
- [P1] Mobile / navigation — two horizontally scrolling tab rows conceal destinations and do not scale to three planned capability areas → replace them with one native grouped page selector.
- [P1] Model Store / install — catalogue, artefacts, naming prompt, downloads, and installed models are separated by long scroll distances → make `find → review fit → name and install → track` one continuous flow.
- [P1] Model Store / mobile — 760px and 980px minimum-width tables make the primary acquisition journey depend on horizontal scrolling → use responsive lists for artefacts and installed models.
- [P2] Home / summary — split LLM and Dictation summaries repeat request, model, and cost totals and compete with live health → remove the service split and keep four durable lifetime facts.
- [P2] Home / activity — combined usage is nested inside Live system, making both sections visually long → give live signals, combined usage, and activity separate compact regions.
- [P2] Planned capabilities — Image, Music, and Post Training have no information-architecture location → add dedicated preview routes with explicit planned status and no fake controls.
- [P2] Model Store / filters — runtime, modality, recommendation, VRAM, and sorting controls all compete with the search action → disclose advanced filters only when requested.
- [P2] Shared headings / narrow widths — headings and action groups assume one row → allow wrapping without separating actions from their section.
- [P3] Model Store / status — loading and errors lack a nearby retry path → keep the query, state what is happening, and expose retry beside the result region.

Applicable De-vibe checks: 27, 29, 35, 44, 71, 74, 82, 83, 95, 97, 99, and 100.

## Decisions

### Q-001 — What must Home answer before the first scroll?

- Status: resolved
- Why now: The user explicitly needs live processing and the current model at a glance.
- Blocked by: Nothing.
- Evidence: `OverviewPage.tsx` leads with eight historical counters and places the current model only in `App.tsx`'s small top bar; `docs/v2-cleanup-report.md` originally required current model and state first.
- Decision: Lead with a runtime summary containing model, state, running and queued work, GPU, and VRAM. Follow with four combined lifetime facts, then independent Live system, Combined usage, and Recent activity regions.
- Consequences: Delete the two service summary panels and token in/out split. Keep detailed service economics on Usage pages.
- Revealed questions: None.

### Q-002 — How should current and planned capabilities fit desktop and mobile navigation?

- Status: resolved
- Why now: Adding three areas makes the existing two-row mobile tab scroller worse.
- Blocked by: Nothing.
- Evidence: `App.tsx` renders separate horizontally scrolling section and page rows below `md`; the current page registry already provides one source for navigation.
- Decision: Keep the desktop sidebar, add a clearly separated Planned group, and use one native grouped page selector on mobile. Image, Music, and Post Training each receive one preview route.
- Consequences: Every destination remains reachable with one mobile control; preview pages state that no runtime action is available.
- Revealed questions: None.

### Q-003 — What is the shortest safe Model Store journey?

- Status: resolved
- Why now: The current flow requires scanning all filters, selecting a repository, scrolling to a table, and answering `window.prompt` before the download appears at the bottom.
- Blocked by: Nothing.
- Evidence: `ModelStorePanel.tsx`; model-store endpoints already return repository popularity, compatible artefacts, size, estimated VRAM, background progress, cancellation, and resume.
- Decision: Use `find → select result → select artefact → confirm generated name → install`, with recommendations near search, advanced filters in `details`, a responsive ranked result list, an inline naming confirmation, and active downloads above the secondary installed-library disclosure.
- Consequences: No API change and no speculative recommendation engine. Preserve explicit destructive confirmations for archive and delete.
- Revealed questions: None.

### Q-004 — How far should mobile restructuring extend?

- Status: resolved
- Why now: Dense diagnostics and usage tables cannot all become simplified cards without losing comparison value.
- Blocked by: Q-001, Q-002, and Q-003 are resolved.
- Evidence: Home and Model Store are the primary monitoring/acquisition journeys; Usage and Diagnostics contain genuinely comparative tables and logs.
- Decision: Remove horizontal dependence from Home and Model Store. Keep dense secondary tables inside labelled local overflow regions, while improving shared wrapping, control height, focus, and narrow-screen spacing across every page.
- Consequences: The critical mobile tasks become linear; advanced comparison data remains intact rather than duplicated into mobile-only markup.
- Revealed questions: None.

### Q-005 — What is enough to stop?

- Status: resolved
- Why now: The user delegated the completion boundary.
- Blocked by: Q-001 through Q-004 are resolved.
- Evidence: Baseline dashboard suite is 7 files and 42 tests; the repository requires committed production assets after dashboard builds.
- Decision: Stop after the affected journeys have focused assertions, the complete dashboard suite passes with exact counts, the production build succeeds and refreshes static assets, Git has no conflict or whitespace errors, and representative desktop/tablet/mobile journeys are either browser-verified or explicitly reported `INCONCLUSIVE` with the unavailable-browser evidence.
- Consequences: No unrelated gateway redesign, backend feature implementation, or deployment.
- Revealed questions: None.

## Next workflow

Implement the resolved decisions in `apps/dashboard/src`, update the focused Vitest journeys, run `pnpm --filter dashboard test` and `pnpm --filter dashboard build`, then execute the Review Site verification against the local Vite surface.

## Verification evidence

- `pnpm --filter dashboard typecheck`: passed.
- `pnpm --filter dashboard test`: 7 of 7 files and 47 of 47 tests passed after the production build.
- `pnpm --filter dashboard build`: passed; Vite transformed 447 modules and emitted one current JavaScript bundle and one current stylesheet alongside the existing logo.
- `git diff --check`: passed.
- Primary mobile paths are structurally linear: one grouped navigation select, no Home overflow table, and no Model Store minimum-width table or browser prompt.
- Follow-up control pass: Settings is globally visible, connection state opens Health & alerts, warnings and errors precede diagnostics, and the full gateway log is collapsed until requested.
- Rendered desktop, tablet, and mobile visual balance: `INCONCLUSIVE`. The in-app Browser was retried against the running local Vite URL and reported `No browser is available`.
