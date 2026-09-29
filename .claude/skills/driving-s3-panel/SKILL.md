---
name: driving-s3-panel
description: Use when driving an ESP32-S3 or Guition JC3248W535 touch panel from swamp with the @vcjdeboer/s3-panel or @vcjdeboer/s3-device model types - serial port errors ("cannot open /dev/cu.usbmodem..."), empty or lost replies, the holder, flashing the s3panel firmware, answering a workflow's manual_approval step on the panel, or changing and publishing these extensions.
---

# Driving the S3 panel from swamp

## Overview

The board renders; the host says what to render. Every exchange is one text
command in and one JSON line out over USB serial, and every swamp call records
what happened. Almost every failure is one of three things: the wrong port,
the wrong owner of the port, or the panel busy waiting for a tap.

## Rules

1. **Talk to the board only through swamp model methods.** Never raw
   `arduino-cli`, `screen`, `cat /dev/cu.*` or a serial script: those bypass
   the holder, fight over the port and leave no record. Flash with the model's
   `flash` method.
2. **Run `detect` after any replug**, and repin `device` if the port moved.
3. **Set `holder: true`** for anything interactive, and run one method per
   model at a time.
4. **Start `approve` in the background, then let it finish the job.** It
   approves or rejects the waiting run and resumes it itself. Never follow a
   panel tap with your own `swamp workflow approve|reject`.
5. **Change extension code in its source repo**, never in
   `.swamp/pulled-extensions/`. A pull overwrites that copy and nobody else
   gets the change.

## Quick reference

| Task | Command |
| --- | --- |
| Which port is the board on | `swamp model method run panel detect`, then `swamp data get panel devices-host --json` |
| Is the right firmware answering | `swamp model method run panel ping` (expects `"fw":"s3panel ..."`) |
| Keep the port open | `swamp model method run panel hold` / `release` |
| Touches since last clear / wait for one | `touchclear`, then `touchstate`; or `waittouch --input timeoutMs=60000` |
| Any firmware command, reply recorded | `swamp model method run panel send --input line=status` |
| Flash | `git clone https://github.com/vcjdeboer/s3-panel`, then `swamp model method run panel flash --input sketchPath=<clone>/firmware/s3panel` |
| Answer an approval on the panel | `swamp model method run panel approve --input workflow=<wf> --input step=<step>` (background) |
| What is waiting for approval | `swamp workflow approvals --json` |
| What a run did | `swamp workflow history get <run-id> --json` |

## Symptoms

| Symptom | Cause | Fix |
| --- | --- | --- |
| `cannot open /dev/cu.usbmodemXXXX: NotFound` | Board replugged; the name follows the USB socket | `detect`, then edit the `device:` line in `models/@vcjdeboer/s3-panel/<name>.yaml` |
| `several serial devices found (...)` | Auto-detect with two boards attached | Unplug the other board, `detect`: the remaining port is the S3. Pin it, replug the other |
| `no serial device found` | Nothing plugged in, or a charge-only cable | Replug with a data cable, then `detect` |
| `write` fine, `read` empty | No holder: the port closed before the reply | Use `send` (write and wait), or `holder: true` + `hold` |
| Commands time out right after an `approve`/`zonewait` | Firmware is inside `screen wait` and ignores serial | Tap the panel, wait out `waitMs`, or replug |
| Parallel calls time out | Per-model lock | One call at a time, or one fan-out method |
| `flash` fails at upload | Another holder or monitor owns the port; port moved; panel still in `screen wait` | `release` every model on that port, close monitors, `detect`, tap or replug |
| Touch always `x=0,y=0` | Touch initialised before the display | Display init first in `setup()` |
| Panel shows NOT RECORDED | swamp refused the decision (run no longer waiting) | Check `approval-latest.resolveError` |

## Details

- Serial, ports, holder, locks and the line protocol: `references/serial-and-holder.md`
- Approvals end to end, including the user answering in the terminal: `references/approvals.md`
- Firmware, and changing and publishing the extensions: `references/changing-the-extensions.md`
