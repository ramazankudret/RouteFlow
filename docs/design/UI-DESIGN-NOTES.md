# Handoff: RouteFlow Console (cluster / jobs / decision / accuracy)

## Overview

RouteFlow is a scheduler that routes LLM inference requests across a small
fleet of heterogeneous local machines (desktop GPU, Jetson SBC, laptop, NUC,
Mac mini). It predicts how long a request will take on each candidate node and
picks the cheapest one. This design is the operator console for that scheduler:
four screens over one shared node graph.

The single idea the whole UI is built to communicate: **a model already resident
in VRAM is worth more than a faster idle machine.** Every screen makes that
difference visible — lit vs. dim model labels on the canvas, warm/cold tags in
the feed, `t_load` in the decision breakdown, warm/cold split in every accuracy
chart.

## About the Design Files

The four files in `design/` are **design references created in HTML** —
prototypes that show the intended look, structure and behaviour. They are not
production code to lift wholesale.

The task is to **recreate these designs in the target codebase's existing
environment** (React, Vue, Svelte, SwiftUI, native, whatever the app already
uses) with its established patterns, state layer and component library. If the
project has no frontend yet, pick the framework that best fits the rest of the
stack and implement there.

Two things about these files are, however, deliberate constraints that came from
the product brief, not accidents of prototyping — carry them over unless the
target codebase makes them impossible:

1. **No frameworks, no CDN, no build step.** The brief specifies the console is
   served by a small C++ static file server on the same box as the scheduler.
   Each file is self-contained: one `<style>` block, plain vanilla JS, no
   external requests, no web fonts. If you are recreating this inside an
   existing SPA, that constraint is already satisfied differently — but do not
   introduce a charting library, an animation library or an icon package to
   reproduce what is currently hand-built. Everything here is CSS + inline SVG
   on purpose.
2. **Every dynamic value carries a `data-rf="<field>"` attribute** naming the
   backend field that fills it, and repeated blocks are duplicated once inside
   a `<template id="rf-tpl-…">`. That is the wiring contract: the markup tells
   you which JSON field goes where. Keep the attribute names when you convert to
   components (they map 1:1 to props/state fields).

## Fidelity

**High-fidelity.** Colors, typography, spacing, motion timings and copy are
final. Recreate pixel-close using the codebase's own primitives. Every value you
need is in the Design Tokens section below; the same `:root` block is byte-identical
in all four files.

Two areas that are intentionally *content* placeholders, not design decisions:
the numbers, node names and job IDs are plausible fixtures, and the six-hour
accuracy window is invented. Wire real data in; don't preserve the fixtures.

## Target viewport

Designed for a desktop operator screen at **1440 × 900** and up. Each file
declares `<meta name="viewport" content="width=1440">`. There is no mobile
layout and the brief does not ask for one. Below ~1180px the canvas keeps
working (it scales and pans) but the side panels stop being comfortable.

---

## Shared shell (all four screens)

### Top bar — 52px tall, `border-bottom: 1px solid var(--hair)`

- Left: wordmark `Route` + `Flow`, `--f-mono`, 12px/600, `letter-spacing: .28em`,
  uppercase. `Flow` is `var(--live)` with `text-shadow: 0 0 10px rgba(51,224,122,.7)`.
- Mode nav: `cluster · jobs · decision · accuracy`, 10px/600 mono,
  `letter-spacing: .16em`, uppercase, `var(--fg-3)`. Active item is `var(--fg)`
  with a 1px `var(--live)` bottom border and a soft green text-shadow. Hover
  lifts to `var(--fg-2)`. In the prototype these are `<a href="…html">`; in the
  app they are routes.
- Right cluster (10px mono, `var(--fg-3)`, values in `var(--fg-2)`):
  `nodes 5` · `engines 4 up` · `warm 3` (value in `var(--live)`) · `inflight 4` ·
  `tick 1.0 s`.

### Background

`body` is `var(--void)`. A fixed `body::before` paints
`radial-gradient(1100px 620px at 44% 42%, #0c141d 0%, #05070a 72%)` behind
everything. `body { overflow-x: hidden }` — the stage is a fixed-height region,
side panels scroll internally.

### The node graph ("canvas") — the piece to get right

All four screens render the same graph. It is the visual spine of the console.

**Geometry model (important — this was the source of several bugs):** one model
in both CSS and JS.

- `.canvas` is the absolutely-positioned viewport (`inset: 0`, `overflow: hidden`).
  It may carry `data-reserve-left` / `data-reserve-right` (px) declaring space
  occupied by an overlapping panel, so the fit math keeps devices out from
  under it. cluster.html uses `data-reserve-right="452"`; jobs.html uses
  `data-reserve-left="404"`.
- `#rf-world` is the graph's own coordinate space: **1180 × 760 px**, fixed size,
  `position: absolute; top: 0; left: 0; margin: 0; transform-origin: 0 0`.
- The default view is authored as an **inline `transform: translate(x,y) scale(s)`
  in the markup** — so the correct framing exists with zero JS. Per screen:
  cluster `translate(40px,61px) scale(.770)`, jobs `translate(446px,47px) scale(.808)`,
  decision & accuracy `translate(214px,28px) scale(.857)`.
- JS only *overrides* that: a `fit()` that recomputes the same translate/scale
  from the live box, guarded so it bails when the box measures 0, retried via
  `ResizeObserver` until it lands, and skipped entirely once the user has panned
  or zoomed (`data-touched`). Do not make first paint depend on JS measurement.
- `fit()` math: `bw = max(240, cw - reserveLeft - reserveRight)`,
  `s = max(.2, min(bw/1180, ch/760) * .92)`,
  `x = reserveLeft + (bw - 1180*s)/2`, `y = (ch - 760*s)/2`.

**Node coordinates** (in the 1180 × 760 space; each node is
`transform: translate(-50%,-50%)` around its point, `width: 184px`, centred text):

| node | x | y | device shape |
|---|---|---|---|
| clients | 88 | 366 | phone/tablet |
| routeflow (hub) | 368 | 366 | router |
| desktop-01 | 168 | 130 | tower case |
| jetson-01 | 596 | 118 | SBC / dev board |
| mini-01 | 760 | 336 | mac mini |
| laptop-01 | 200 | 596 | laptop |
| nuc-01 | 646 | 580 | mini PC |

**Device silhouettes** are small hand-built inline SVGs, stroke-only
(`stroke: currentColor`, `fill: none`, `stroke-width: 1.4`, `stroke-linejoin: round`),
46–56px wide. One shape per machine class — a tower with drive slots, an SBC with
a GPIO header and two chips, a clamshell laptop, a rounded NUC with a fan circle,
a flat mini, a phone, a router with two antennae and three LEDs. If your codebase
has an icon system, substitute equivalents of the same weight; do not swap in
filled/duotone icons, the whole canvas reads as line-art.

**Links** are one `<svg class="links" viewBox="0 0 1180 760" preserveAspectRatio="none">`
holding, for each connection, up to three stacked cubic paths with the same `d`:

- `.link-glow` — `stroke-width: 6`, `filter: blur(4px)`, opacity .18–.22, pulsing.
- `.link` — the hairline, `stroke-width` set **per node inline** to encode speed
  (desktop 2.1, mini 2.0, laptop/nuc 1.4, jetson 1.2 — thicker = faster per-token).
- `.link-flow` — travelling dots: `stroke-dasharray: 1 22`, `animation: rf-flow
  linear infinite` with `animation-duration` set inline per node to encode queue
  pressure (jetson 1.3s = 3 inflight, mini 2.4s, desktop 3.4s = idle). Only drawn
  for links whose state is `warm`.

Path shape: `M ax ay C ax+dx ay, bx-dx by, bx by` where `dx = (bx-ax)*0.45`.
Each path carries `data-rf-link="<nodeId>|client"` plus `data-a` / `data-b`
endpoint keys so JS can redraw after a drag.

**Link state → colour** (this is a design rule, not decoration):

| state | stroke | treatment | meaning |
|---|---|---|---|
| `warm` | `var(--live)` #33e07a | glow + pulse + flow dots | engine up and model resident |
| `unknown` | `var(--idle)` #4a545e | flat, opacity .5 | residency or telemetry unknown |
| `offline` | #2a3138 | `stroke-dasharray: 2 7`, no glow | engine down |

`cold` (#d95926) exists in the palette for links whose model is on disk but not
resident; in the current fixture no node is in that state.

**Per-device live layers** (all children of `.dev`, which is
`position: relative; display: inline-grid; width: fit-content`):

- `.dev-lvl` — VRAM residency fills the device body from the bottom:
  `left/right: 9px; bottom: 7px; height: var(--vram)`,
  `linear-gradient(180deg, rgba(51,224,122,.4), rgba(51,224,122,.05))`,
  1px top edge `rgba(51,224,122,.85)`, `box-shadow: 0 0 14px rgba(51,224,122,.4)`,
  breathing `rf-lvl 5.4s`. `--vram` per node: desktop 55%, jetson 42%, mini 14%,
  laptop 0, nuc 0 (`.is-zero` hides it). The SVG sits above at `z-index: 2`.
- `.heat` — temperature ring: 76px circle (92px and opacity .6 when `.is-hot`,
  i.e. ≥70 °C), `border: 1px solid currentColor`, `animation: rf-heat var(--heat-ms)`.
  Colour and period by temperature: ≥70 → #d95926 / 2.2s, 56–69 → #c98500 / 3.2s,
  else #2f8f63 / 4.6s. **Nodes with no telemetry backend get no ring at all**
  (laptop-01) — absence of data must not render as a cool reading.
- `.shock` ×2 — eviction shockwave on nodes that would evict:
  `border: 1px solid var(--t-evict)`, `rf-shock 7s` scaling .5→3.1 and fading,
  second copy delayed .35s. Currently on desktop-01.
- `.spark` ×2 — flicker at the broken end of an offline link, 3px #d95926 dots
  with `rf-spark 1.7s steps(1,end)`. Currently on nuc-01.

**Node label stack**, top to bottom, all `--f-mono`:
device → `node-id` (11px/600, `letter-spacing: .18em`, uppercase) →
`node-sub` (9px, `var(--fg-3)`: ip · short GPU name) →
`node-tel` (9px row: util %, temp°, watts — each renders as `—` in `var(--fg-3)`
when telemetry is absent) →
`node-vram` (132 × 3px bar: `#121a21` track, `#2a3540` used, `var(--live)` resident
with `box-shadow: 0 0 7px`) →
`.shelf`: **resident models in `var(--live)` with a green text-shadow and a `●`
prefix, models that are only on disk in #424d56.** That contrast is the single
most important thing on the canvas. When residency is unreportable the shelf says
`residency unknown`; when nothing is resident it says `all cold · nothing resident`.
Below that, optional flags: `queue N · 3/2 slots` in `var(--t-queue)` when
`inflight > slots`, `engine down` in `var(--cold)`.

Offline nodes get `.is-offline`: device colour #39434b, no breathing animation,
node-id drops to `var(--fg-3)`.

**Canvas interactions** (all pointer-event based, one handler on `.canvas`):

- **Drag a device** — repositions it (`left`/`top` in %, clamped 4–96% / 6–94%)
  and redraws every attached path from the endpoint map. `.is-drag` boosts the
  device glow.
- **Drag empty space** — pans the world.
- **Wheel** — zooms about the cursor, `scale` clamped 0.5–1.9, factor 1.09 per notch.
- **Hover a device** — focus mode: `.canvas.is-focusing` drops other nodes to
  opacity .28 and unrelated links to .08, and thickens the hovered node's link
  to `stroke-width: 3` with a glow.
- **Arrow keys** — step through nodes, applying focus and firing the node's click.
- **`reset view`** button (bottom right, 9px mono, hairline border, green on hover)
  → refit.
- **Minimap** (154 × 82px, bottom right, `rgba(5,7,10,.75)`, hairline border,
  label `view`) — one dot per node mirroring its `left`/`top`, synced on
  pointermove. Warm dots green with glow, hub bigger and brighter, offline dot
  #3a2420.
- A drag must not count as a click: the pan/drag handler sets `data-dragged` on
  the world and the selection handler consumes and ignores that one event.

**Boot sequence** (once, on load): a full-canvas `.boot` sweep —
`linear-gradient(90deg, transparent, rgba(51,224,122,.11) 46%, rgba(51,224,122,.5) 50%, rgba(51,224,122,.11) 54%, transparent)`
translating -100% → 100% over 1.15s; nodes fade/rise in with
`animation-delay: calc(var(--i) * .09s + .12s)`; the link layer fades in over
.9s at .5s. Numeric readouts (`.tel-v`, `.tt`, `.node-tel span`, `.s-v`) count up
from 0 to their value over 780ms with a cubic ease-out, mutating only the first
text node so units and markup survive.

Motion is continuous by design (the user asked for "everything breathing") and
there is no off switch in this version. If your app has a reduced-motion
preference, honour it there — the natural hook is to gate the `rf-pulse`,
`rf-flow`, `rf-lvl`, `rf-heat`, `rf-shock`, `rf-spark` and `rf-scan` animations.

---

## Screen 1 — cluster.html

**Purpose:** see the whole fleet and inspect one machine in depth.

**Layout:** top bar, then `.stage` (`height: calc(100vh - 52px)`, `min-height: 680px`).
Full-bleed canvas; a 452px inspector panel pinned right
(`rgba(5,7,10,.9)`, `backdrop-filter: blur(12px)`, `border-left: 1px solid var(--hair)`,
`overflow-y: auto`, 22px padding). Bottom-left: a three-row link legend (9px mono,
`letter-spacing: .09em`) explaining lit / grey / dashed. Bottom-left above the
minimap: the one-line thesis — *"click a device to inspect it · lit labels under a
device are resident in VRAM, dim labels are on disk only — the whole scheduler
rests on that difference"*.

**Inspector panel** — one `<section data-insp="<nodeId>">` per node, all in the
markup, only the selected one visible (`hidden` on the rest). Sections, separated
by `border-top: 1px solid var(--hair)` with a 9px uppercase `.lbl` heading:

1. **Header** — node id 17px/600 mono `letter-spacing: .14em` uppercase, IP at
   10px `var(--fg-3)`, full GPU name at 11px `var(--fg-2)`.
2. **Status LEDs** — three 7px dots: engine (`.is-on` green + glow / `.is-off`
   #3a2420 + orange glow), telemetry backend (`nvml` / `tegra` / `metal` /
   `level_zero` / `none`), residency known/unknown. Unlit = `var(--idle)`.
3. **VRAM** — `used %` and `free of total`, then a 6px bar (`#111820` track,
   `#28323c` used, `var(--live)` + glow resident), then a three-item key
   (resident / other / free with GiB values).
4. **Telemetry** — 2 × 2 grid: gpu util, temperature, power (`x / cap W`),
   inflight (`n / slots`). 17px mono values; `is-hot` (`var(--cold)`) when util
   ≥50% or temp ≥70; missing values render `—` in `var(--fg-3)`.
5. **Models** — two columns split by a 1px divider: **on disk** (names in
   `var(--fg-2)`, size) and **resident in VRAM** (names in `var(--live)`, VRAM
   bytes + "used Ns ago"). Column header carries the count, or `?` when residency
   is unknown. Empty states are sentences, not dashes: *"residency unknown — this
   engine does not report loaded models"*, *"nothing resident — every request here
   pays a cold load"*.
6. **Why it matters** — one plain-language sentence per node tying its state to a
   scheduling consequence (e.g. jetson: *"slow per token, but two models stay
   resident. inflight 3 > 2 slots, so t_queue is non-zero for the next request."*).

**Panel footer** — `state reference — static markup, not live`: a line naming
which node demonstrates each edge case, plus the **empty state** card
(`border: 1px dashed var(--hair-2)`, 22px padding): *"No nodes registered —
RouteFlow reads nodes.toml at start and accepts /register at runtime."*

**Interaction:** clicking a device selects it (`.is-selected` → node-id turns
green with glow), swaps the visible inspector section and resets panel scroll.

**Fixture data** (5 nodes) — exact values live in the file; the shape per node is:
`id, ip, gpu_name, engine (ollama | lm-studio), engine_healthy, telemetry_backend
(nullable), residency_known, vram_total/free bytes, gpu_util, temperature_c,
power_watts + cap, engine_slots, inflight, models_on_disk[{name, disk_bytes}],
resident[{name, vram_bytes, last_used}]`.

---

## Screen 2 — jobs.html

**Purpose:** the request log, and where each request actually ran.

**Layout:** 404px feed pinned left (same translucent panel treatment, `border-right`),
full-bleed canvas behind and to the right. Bottom-left note: *"canvas shows the
route: the lit link is the selected job's path from clients through the scheduler
to its node."*

**Feed header** (sticky, `rgba(5,7,10,.96)`): `jobs · last 12` label, then
`9 ok · 3 failed · warm on 7 of 11 dispatched · median total 4.26 s` and
*"select a job to light its route on the canvas"*.

**Job row** — `padding: 12px 22px`, `border-bottom: 1px solid #101720`,
`cursor: pointer`; hover `rgba(51,224,122,.045)`; selected `rgba(51,224,122,.08)`
plus a 2px `var(--live)` left bar with `box-shadow: 0 0 10px`. Four lines:

1. model name (11px/600 mono) + right-aligned job id (9px `var(--fg-3)`).
2. `requested from clients · running on <node> · <warm|cold|no node> · <role_hint>`
   — 9px, `warm` green, `cold` `var(--cold)`, `no node` `var(--idle)`.
3. `ttft · total · pred · OUTCOME` — 9px tabular; predicted is dimmed; outcome is
   9px/600 uppercase `letter-spacing: .12em`, green for `ok`, `var(--cold)` for
   failures, `var(--fg-3)` for `client_abort`.
4. `decided by <term> · started <time>` and, when a decision trace exists,
   `open decision →` in `var(--live)`.

All twelve outcome/decision combinations from the brief appear at least once:
`ok`, `no_candidate`, `dispatch_failed`, `stream_failed`, `client_abort`,
`timeout` × `t_queue`, `t_load`, `t_prefill`, `t_decode`, `t_evict`,
`within_noise`, `single_candidate`. A failed job's `total` is time-until-failure,
never a completion — the footer says so. `no_candidate` rows carry no node and no
timings.

**Interaction:** selecting a row lights that job's route — its link and the
client→hub link get `.is-lit` (`stroke-width: 2.8`, full opacity, drop-shadow),
everything else gets `.is-mute` (link opacity .12, node opacity .34). Clicking
the `open decision →` affordance navigates to the decision screen anchored at
that job.

**Feed footer:** the outcome and decided_by vocabularies as static reference, plus
the empty state *"No jobs yet — requests arriving on /v1/chat/completions appear
here, newest first."*

---

## Screen 3 — decision.html

**Purpose:** the core explanatory screen — why this node won, for one request.

**Layout:** dimmed canvas (`.is-dim`, opacity .5) with a scrolling overlay on top
(`rgba(5,7,10,.87)`, `backdrop-filter: blur(8px)`), content max-width 1560px,
padded 22/32px. The minimap is raised above the overlay (`z-index: 7`, enlarged to
196 × 104, label `node map`) because it becomes the canvas reference for this screen.

**Job tabs** — three traces, each a button with job id (10px/600 uppercase) over a
plain-language name: `hot beats idle`, `within noise`, `eviction cost`. Active tab
id turns `var(--live)` with glow. Deep-linkable via `#job-1|2|3`.

**Decision header** — job id label, model name at 22px/600 mono, meta row
(`role_hint`, prompt tokens, candidates evaluated, admitted), and on the right:
`decided by` + the deciding term at 17px/600 uppercase in that term's colour, or
`≈ within_noise` in plain `var(--fg)` when the margin is inside the noise floor.
Below it the verdict sentence with the actual numbers:
*"margin 4.23 s against the largest σ of 2.40 s — the ranking survives the noise."*

**Thesis paragraph** — 14px `--f-ui`, `var(--fg-2)`, max 104ch, one per job. This
is the sentence a human reads instead of the chart.

**Legend + axis row** — the five term colours, the σ band glyph, the "selected
total" guide, and the note that rejected candidates get no bar. Then a shared axis
row: label left, ticks (0 / 25 / 50 / 75 / 100% of the job's scale) centre,
`predicted total` right.

**Candidate row** — `grid-template-columns: 296px 1fr 140px`:

- *Left:* node id (14px/600 uppercase; green + glow when winner), `selected` /
  `warm` / `cold` / `residency unknown` / `rejected` tag (9px/600 uppercase in the
  matching colour), short GPU + engine + slot line, then state at decision
  (`vram_free`, `util`, `inflight`) and `conf converged|learning|seeded · N samples`
  (converged green, learning `var(--fg-2)`, seeded `var(--idle)`).
- *Middle:* the estimate. A 24px `.track` with faint gridlines at 25/50/75/100%,
  a vertical `var(--live)` guide at the winning total, and a flex `.bar` of five
  segments — `t_queue` #3987e5, `t_load` #d95926, `t_prefill` #199e70,
  `t_decode` #c98500, `t_evict` #d55181 — each width `%` of the job's max, `gap: 2px`,
  zero-value segments hidden. Segments grow in with
  `rf-grow .62s cubic-bezier(.2,.75,.25,1)`, staggered per row so the winner lands
  last; the σ band and mark fade in at .78s. Below: a 9px hatched σ band
  (`repeating-linear-gradient(90deg, rgba(147,163,172,.85) 0 1px, transparent 1px 4px)`
  with 1px end caps) spanning ±1σ, and a 1px `var(--fg)` mark at the predicted
  total. Then the term readout — `t_queue 0 ms`, `t_load 9.50 s`, … — where
  **zero and omitted are visually distinct**: zero is dimmed #414c55, omitted is
  `var(--fg-3)` with a hollow swatch and reads the word `omitted`. A node that
  would evict shows a `var(--t-evict)`-bordered note: *"would evict
  llama3.2:3b-instruct-q4_K_M · 1.88 GiB · last used 40 s ago — t_evict prices what
  that costs the next request for it, not this one."*
- *Right:* predicted total at 30px/600 tabular (green + glow for the winner),
  `± 2.40 s · 1σ` beneath. Rejected candidates show `—` at 22px in `var(--fg-3)`
  and `not scored`.

**Rejected candidates** get no bar at all — instead the reason (9px/600 uppercase
`var(--cold)`) and a sentence: `insufficient_vram` (needs 4.42 GiB, 3.00 GiB free),
`model_missing`, `engine_down` (unreachable 3 min 12 s), `node_stale`
(telemetry 94 s old, limit 15 s), `excluded` (operator rule). The winner row gets
`.is-winner`: a left-to-right green wash plus a slow scanline overlay
(`repeating-linear-gradient` at .055 alpha, `rf-scan 5.5s linear infinite`).

**Row → node hint lines:** an SVG layer above the overlay draws a dashed cubic
from each candidate row to that node's minimap dot — `var(--live)` at .5 opacity
for the winner, `var(--fg-3)` at .4 for the others, with a 2.5px dot at the row
end. Recomputed on overlay scroll, resize, canvas drag and tab change; rows
scrolled out of view are skipped.

**Three traces to reproduce exactly** (they are the teaching set):

1. `j-7f31c2` — jetson-01 wins on residency at 8.82 s ± 2.40 despite being the
   slower machine; desktop-01 is idle but pays `t_load 9.50 s` → 13.05 s.
   Decided by `t_load`, margin 4.23 s vs largest σ 2.40 s.
2. `j-7f31be` — both nodes warm, margin 250 ms against σ of 520/610 ms →
   `within_noise`, desktop-01 taken on the fewer-inflight tiebreak.
3. `j-7f31b4` — nothing warm; desktop-01 pays `t_evict 1400 ms` to drop a model
   used 40 s ago and still beats jetson-01, whose slots are full (`t_queue 4200 ms`,
   `t_evict` omitted). Decided by `t_evict`.

---

## Screen 4 — accuracy.html

**Purpose:** does the scheduler's prediction hold up? Five views, and timing error
is never averaged together with length error.

**Layout:** dimmed canvas + scrolling overlay (same as decision). Header: label,
`predicted vs measured` at 22px/600 mono, and a right-aligned framing paragraph.
Then `.acc`, a two-column grid with 32px gaps; the histogram spans both columns
(`.span2`), the last two sit side by side.

Panel chrome, shared: `h2` 17px/600 mono uppercase `letter-spacing: .1em`; a 9px
`n 26 · last 6 h` note; a 10px description paragraph (max 64ch); a 9px
`var(--fg-2)` field-mapping line (e.g. `predicted_total_ms → total_ms`); a bottom
legend row; then a stats block of `label / value / note` rows where the headline
value is `var(--live)`.

1. **total duration** — scatter, 360px plot, x `predicted_total_ms`, y `total_ms`,
   both 0–16 s with ticks at 0/4/8/12/16. Axes are 1px `var(--hair-2)` with
   `#131b23` gridlines at 25/50/75/100%; plot fill `rgba(255,255,255,.012)`.
   A `#93a3ac` 1:1 diagonal (inline SVG, `preserveAspectRatio: none`,
   `vector-effect: non-scaling-stroke`) labelled `1:1`. 26 dots, 8px, `translate(-50%,50%)`:
   **warm = `var(--t-queue)` blue with a blue glow, cold = `var(--t-load)` orange
   with an orange glow.** One annotated outlier chip (`j-7f2ea4 · cold · +25%`,
   9px mono on `rgba(5,7,10,.9)` with a hairline border). Stats: all traces median
   |err| 9.4% / p90 28.1% / bias +6.2%; warm n 14 median 4.1%; cold n 12 median
   16.8%; worst +25.4%.
2. **output length** — same construction, 250px plot, 0–1200 tokens, dots neutral
   `#8d9aa3`. Annotated outlier `j-7e90c1 · 240 → 880 tok`. Stats: median 8.9%,
   p90 61.7%, 11 over-runs, 2 early stops, worst +267%.
3. **signed error spread** — full-width stacked histogram, 190px tall, seven 10%
   buckets from −30% to +40%, `grid-auto-flow: column` with 7px gaps, bars anchored
   to a 1px `var(--hair-2)` baseline. Warm (blue) stacked under cold (orange),
   each with a soft glow; zero-count segments hidden; the bucket total floats above
   each column. Footer: `median +6.1% · p90 +28.1% · 8 of 26 finished early`.
4. **error by node** — dumbbell rows, `grid-template-columns: 104px 1fr 128px`,
   axis 0–30%. Each row: node id, a 16px track with a `#131b23` rule and gridlines
   at 10/20/30%, a warm→cold gradient connector, an 11px blue dot (warm median)
   and an 11px orange dot (cold median), then the two values with sample counts.
   desktop-01 3.8/18.2 (5/4), jetson-01 4.6/12.9 (6/2), mini-01 3.1/21.4 (3/3),
   laptop-01 cold-only 24.6 (3) with the note *"no warm traces — this engine never
   reports residency, so every job here is scored cold"*, nuc-01 `no data` with a
   hollow dot and *"no traces in this window — engine has been down 3 min 12 s"*.
5. **calibration drift** — 180px time series over 12 half-hour steps, y 0–24% with
   labels outside the plot at 0/6/12/18/24. Inline SVG, `viewBox="0 0 100 100"`,
   `preserveAspectRatio: none`: a `rgba(57,135,229,.13)` band for the cold ±4.5%
   spread, the cold rolling median as a dashed `var(--t-load)` line with glow, the
   warm rolling median as a solid `var(--t-queue)` line with glow. A vertical
   `var(--t-evict)` marker at 63.6% labelled `eviction storm · 04:41 PM`. X labels
   `11:20 AM / 02:20 PM / 05:04 PM`. Footer: *"cold error nearly doubled across the
   window."*

Closing note and the `No traces yet` empty state card follow.

---

## Interactions & Behavior — summary

| where | trigger | result |
|---|---|---|
| canvas | drag device | reposition, redraw links, boost glow |
| canvas | drag background | pan world |
| canvas | wheel | zoom about cursor, 0.5–1.9 |
| canvas | hover device | focus mode (others dim, link thickens) |
| canvas | arrow keys | step nodes, apply focus + select |
| canvas | `reset view` | refit |
| cluster | click device | select node, swap inspector section, reset scroll |
| jobs | click row | select, light that route, mute the rest |
| jobs | click `open decision →` | navigate to that job's trace |
| decision | click tab | swap panel, replay bar animation, relight winner link, redraw hint lines |
| decision | scroll overlay | redraw hint lines |
| all | load | boot sweep, staggered node entry, count-up numbers |

Transitions: node/link opacity `.22s ease`; bar grow `.62s cubic-bezier(.2,.75,.25,1)`;
σ band/mark fade `.5s ease-out` at `.78s`; count-up 780ms cubic ease-out.
Loops: `rf-pulse` 3.8s/4.6s, `rf-flow` 1.3–3.4s linear, `rf-breathe` 5.2s,
`rf-lvl` 5.4s, `rf-heat` 2.2–4.6s, `rf-shock` 7s, `rf-spark` 1.7s, `rf-scan` 5.5s.

## State Management

Per screen:

- **cluster** — `selectedNodeId` (default `desktop-01`).
- **jobs** — `selectedJobId` (default newest) → derives the lit link and muted set.
- **decision** — `activeJobId` from the route hash; derives the winner node for
  link lighting and the hint-line targets.
- **accuracy** — no interactive state in this version.
- **canvas (shared)** — `view {x, y, scale}`, `nodePositions {id: {left%, top%}}`,
  `focusedNodeId`, `hasUserAdjustedView`. Node positions are per-session in the
  prototype; if operators should keep their layout, persist them.

Data the console needs from the scheduler (names match the `data-rf` attributes):

- `GET /nodes` → the node objects described under cluster.
- `GET /jobs?limit=N` → `job_id, model, role_hint, node_id, was_resident, ttft_ms,
  total_ms, predicted_total_ms, outcome, decided_by, started_at`.
- `GET /jobs/:id/decision` → `candidates[{node_id, admitted, reason, was_resident,
  vram_free, gpu_util, inflight, t_queue, t_load, t_prefill, t_decode, t_evict,
  predicted_total_ms, sigma_ms, conf, samples, would_evict}]`, plus `decided_by`
  and `margin_ms`.
- `GET /accuracy?window=6h` → traces with predicted/actual pairs, and the derived
  aggregates the panels display.

Polling cadence in the design is the `tick 1.0 s` shown in the top bar.
Three nullable-field rules the UI depends on: a **missing telemetry value renders
`—`**, never 0; **`residency_known: false`** must not be shown as "nothing
resident"; and an **omitted term is not a zero term** (different colour and the
literal word `omitted`).

## Design Tokens

Copy verbatim — identical in all four files.

```css
--void:#05070a;  --void-2:#0a0e13; --void-3:#101720;   /* surfaces */
--hair:#18202a;  --hair-2:#26313d;                     /* borders */
--fg:#e6efe9;    --fg-2:#93a3ac;   --fg-3:#5b6971;     /* text */
--live:#33e07a;  --live-2:#1d7d47;                     /* resident / healthy */
--cold:#d95926;  --idle:#4a545e;   --alert:#d55181;    /* cold / unknown / alert */
--t-queue:#3987e5; --t-load:#d95926; --t-prefill:#199e70;
--t-decode:#c98500; --t-evict:#d55181;                 /* the five terms — fixed */
--f-mono: ui-monospace, "SF Mono", "Cascadia Mono", Menlo, Consolas,
          "Liberation Mono", monospace;
--f-ui: -apple-system, BlinkMacSystemFont, "Segoe UI", Ubuntu,
        "Helvetica Neue", Helvetica, Arial, sans-serif;
--fs-0:9px; --fs-1:10px; --fs-2:11px; --fs-3:12px;
--fs-4:14px; --fs-5:17px; --fs-6:22px; --fs-7:30px;
--s-1:4px; --s-2:8px; --s-3:12px; --s-4:16px; --s-5:22px; --s-6:32px;
```

Colour rules that are not negotiable: the **five term colours are fixed** and mean
the same thing on every screen; **warm is blue `--t-queue` and cold is orange
`--t-load` in all data graphics**, while green `--live` is reserved for chrome,
links and residency. Border radius is effectively 0 (1–3px only on tiny bars and
pills) — this UI is square on purpose. No drop shadows for elevation; depth comes
from glow and translucency.

Typography: monospace everywhere except the decision thesis paragraph and body
copy, which use the UI stack. No web fonts — system stacks only, per the brief.

## Assets

None. No images, no icon font, no external files. Every glyph is either a
hand-built inline SVG (device silhouettes, chart lines and reference diagonals)
or a CSS shape. Nothing loads from the network, which is what lets the static C++
server host it as-is.

## Files

```
design/cluster.html    — fleet canvas + node inspector
design/jobs.html       — request feed + route lighting
design/decision.html   — per-request candidate scoring
design/accuracy.html   — five prediction-accuracy views
design/UIBRIEF.md      — the original product brief these were built from
```

Each file is standalone: open it in a browser and it runs. Read `UIBRIEF.md` first
if anything above seems arbitrary — most of it is a constraint from there.
