# @vcjdeboer/s3-panel

Drive an ESP32-S3 **with a screen** from [swamp](https://github.com/swamp-club/swamp).

![Swamp Club logo on the JC3248W535 panel](logo-on-screen.jpg)

One model type, `@vcjdeboer/s3-panel`. It is everything
[`@vcjdeboer/s3-device`](https://github.com/vcjdeboer/esp32) does — `detect`,
`ping`, `status`, `send`, `write`, `read`, `hold`, `release` — plus typed methods
for the display: `text`, `fill`, `clear`, `backlight` and `logo`.

The board renders and the host says what to render. No pixels cross the wire,
which is what makes a 320x480 panel usable over a serial link at all.

## Why not just use `send`

The screen commands are reachable from the plain `s3-device` type through its
generic `send`. What this type adds is a typed surface:

- **A colour must be one the firmware knows.** `fill --input color=purple` is
  refused by argument validation before the board is touched.
- **Lines are validated, then joined for you.** Pass an array of strings; the
  wire format's separator is applied internally. Text containing that separator,
  or a newline, is rejected up front instead of silently becoming extra lines or
  extra commands.
- **Records are shaped like drawing operations**, with the operation named, so
  `draw-latest` reads as a display history rather than an opaque exchange log.

## Install

```bash
swamp extension pull @vcjdeboer/s3-panel
swamp model create @vcjdeboer/s3-panel panel
```

Set `holder: true` and pin `device` if more than one board is attached. All
global arguments are the same as `s3-device`; see its README.

## Screen methods

| Method | Arguments | Notes |
| --- | --- | --- |
| `text` | `lines` (array of strings) | Replaces the screen. First 12 lines drawn, each truncated to the panel width. |
| `fill` | `color` | One of black, white, red, green, blue, yellow, cyan, magenta. |
| `clear` | none | Blanks to black, leaves the backlight alone. |
| `backlight` | `on` (boolean) | Off keeps the framebuffer, so a dark panel is not evidence that drawing failed. |
| `logo` | none | Show the Swamp Club logo. Touch the screen to trigger an explosion; the logo redraws after. |

Each writes a `draw-latest` record carrying the command sent, the board's reply,
`outcome`, `observedAt` and `elapsedMs`.

## Touch screens and approvals

| Method | Arguments | Notes |
| --- | --- | --- |
| `screen` | `elements` (array), `timeoutMs` | Pushes a layout: `label`, `button` (with an `id`, which makes it a touch zone) and `gap` elements, stacked below a mini logo. Writes `screen-latest`. |
| `zonewait` | `waitMs` (default 12 h) | Blocks until a button is tapped and writes `zone-latest` with its `id`, or a timeout. |
| `approve` | `workflow`, `step`, `prompt`, `waitMs` | Renders APPROVE / REJECT for a workflow's `manual_approval` step, waits for a tap, flashes the decision, returns to the logo. Writes an evidentiary `approval-latest` record (`decision`, `source: panel`, `decidedAt`). |

`approve` records the decision; it does not resume the workflow. Feed it to
`swamp workflow approve` / `swamp workflow reject` yourself. It blocks until a
tap, so run it in the background from an agent or script.

```bash
swamp model method run panel approve --input workflow=deploy \
  --input step=approve-deploy --input 'prompt=Deploy firmware update to panel?'
swamp data get panel approval-latest --json
```

## Use

```bash
swamp model method run panel hold
swamp model method run panel text --input 'lines=["S3 PANEL","ready"]'
swamp model method run panel fill --input color=green
swamp model method run panel clear
swamp model method run panel release
```

Release the holder before flashing the board: a held port blocks the upload.

## Firmware contract

Answer one JSON object per command line. The commands this type sends:

| Sent | Expected reply |
| --- | --- |
| `text a\|b\|c` | `{"ok":true,"lines":3}` |
| `fill red` | `{"ok":true,"color":"red"}` |
| `clear` | `{"ok":true}` |
| `backlight on` | `{"ok":true,"backlight":true}` |
| `logo` | `{"ok":true}` |
| `screen clear` | `{"ok":true}` |
| `screen add {"type":"button","id":"approve","text":"APPROVE"}` | `{"ok":true,"index":6}` |
| `screen show` | `{"ok":true,"elements":7,"zones":2}` |
| `screen wait 60000` | `{"ok":true,"id":"approve","x":160,"y":400}` on a tap, `{"ok":true,"timeout":true}` otherwise |

A reply with `"ok":false` is recorded as `outcome=error` and then thrown, with
the board's own `error` string in the message.

The reference firmware is in the `esp32` repo under `firmware-s3/s3panel/`,
written for the Guition JC3248W535.

## Relationship to `@vcjdeboer/esp32-s3`

swamp resolves extension dependencies only for workflows, so a model type cannot
import another extension's code at runtime. The shared base therefore lives in
`extensions/models/_lib/s3_base.ts`, vendored byte-identically in both
extensions — the same arrangement the serial transport already has. Runtime
composition belongs in workflows, which can span both types.

## Licence

MIT.
