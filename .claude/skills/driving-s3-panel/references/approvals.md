# Approvals on the panel

## swamp's side (native)

- `manual_approval` is a built-in workflow step type. When a run reaches it,
  `swamp workflow run` returns with the run `suspended`.
- `swamp workflow approvals --json` lists waiting runs:
  `workflowName`, `runId`, `stepName`, `prompt`.
- `swamp workflow approve <wf> <step> --run <id>` records the approval; the run
  stays suspended until `swamp workflow resume <wf> --run <id>` runs the
  remaining steps. `--reason` is accepted but not stored for an approval.
- `swamp workflow reject <wf> <step> --run <id> --reason "..."` ends the run as
  `failed`; the reason is kept.
- swamp keeps no reason for an approval: the run record holds only status,
  approver (the OS user) and time. Where an approval came from is recorded by
  the panel, not by swamp.
- `swamp workflow history get <run-id> --json` shows each step's `approval`.

## The panel's `approve` method

```bash
swamp model method run panel approve --input workflow=<wf> --input step=<step>
```

1. Looks up the waiting run. Fails before drawing if none waits at that step,
   or if several do (then pass `--input run=<id>`).
2. Shows workflow, step, the step's prompt (or `prompt`), APPROVE / REJECT.
3. After a tap, runs `swamp workflow approve` or `reject` for that run, with a
   reason naming the panel.
4. After an approve, starts `swamp workflow resume` detached, last, so resumed
   steps that use the same panel are not blocked.
5. Writes `approval-latest`: `workflow`, `step`, `runId`, `prompt`, `decision`
   (`approved`, `rejected`, `timeout`, `cancelled`, `aborted`, `no-reply`),
   `source: panel`,
   `resolved`, `resolveError`, `resumed`, `decidedAt`.

`resumed: true` means the resume was started (detached, output discarded),
not that it succeeded: confirm with `swamp workflow history get <run-id> --json`.
A timeout, lost link or stray tap resolves nothing; the run stays suspended.
`resolve=false` only records a tap; `resume=false` approves without resuming.

## As an agent

1. `swamp workflow run <wf>`; it returns when the run suspends.
2. Start `approve` **in the background**. It blocks until a tap (default `waitMs`
   is 12 h; pass less when the user is present).
3. Tell the user an approval is on the panel, and that they may also answer in
   the terminal.
4. **Panel tap:** the method has already resolved and resumed the run. Read
   `swamp data get panel approval-latest --json` and report `decision`,
   `resolved`, `resumed`, `resolveError`. Confirm with
   `swamp workflow history get <run-id> --json`. Run nothing else.
5. **User answers in the terminal instead:** `swamp model cancel panel`. The
   background `approve` ends within about 2 s, records `decision: cancelled`
   (resolving nothing) and puts the panel back on the logo. Then
   `swamp workflow approve|reject <wf> <step> --run <id>`, and
   `swamp workflow resume <wf> --run <id>` after an approve. Do not kill the
   background task instead: that skips the record and leaves the approve screen
   up.

The decision passed to swamp must always be the one tapped or typed. Never
hardcode a verb, and never chain an approve after reading a panel result.
