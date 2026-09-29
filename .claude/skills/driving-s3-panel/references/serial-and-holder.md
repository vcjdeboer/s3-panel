# Serial, ports, holder and locks

## Ports

- The board is native USB (USB-CDC), 115200 baud, no USB-UART bridge.
- macOS names the node after the physical socket (`/dev/cu.usbmodem101`,
  `...1101`, `...2101`); Linux uses `/dev/ttyACM*` in plug-in order. Both change
  when cables move.
- `detect` lists candidates without opening them and records them in
  `devices-host` (`candidates`, `selected`).
- Auto-detect (no `device` set) works only with exactly one candidate. With a
  second ESP32 attached, pin `device`.
- To repin, edit the `device:` line of `models/@vcjdeboer/s3-panel/<name>.yaml`
  (commit it if the repo tracks models). There is no method-run flag to
  override `device` for one call.
- To tell two boards apart, unplug one and run `detect` again: the port that
  disappeared was that board. (`device` cannot be overridden per call, so
  pinging each candidate means editing the YAML each time.) Once pinned, `ping`
  confirms it: the S3 answers `{"ok":true,"fw":"s3panel <version>"}`; match on
  `s3panel`, since versions differ per board.

## One owner per port

A serial port has exactly one owner at a time.

- **`holder: true`** keeps the port open in a background worker; methods talk
  through it. `hold` starts it, `release` stops it. Methods also start it on
  demand ("holder not running; opening the port for this call only" means that
  call opened and closed the port itself).
- **Without a holder**, each method opens and closes the port. `send` still
  works (it writes and waits in one call), but a bare `write` followed by `read`
  loses the reply: it arrived while the port was closed. `write` also sends
  `data` literally; `\n` is not decoded.
- Two model instances pointing at the same port (for example `s3dev` of type
  `s3-device` and `panel` of type `s3-panel`) cannot both hold it. `release` one
  before using the other.
- `flash` releases its own model's holder and re-takes it afterwards. It cannot
  release another instance's holder or an open serial monitor.

## Locks

swamp runs one method per model at a time. While one runs, another
`swamp model method run` on the same model fails with `lock_timeout` (exit 75);
`approve` and `zonewait` can hold it for hours. `swamp model cancel <model>`
ends a running method: swamp signals it, then kills it about 3 s later, so
long-running methods must check the signal often (these two do, between
2-second wait slices). Prefer one call that does the
whole job, or a swamp workflow, over parallel loops. `touchstate` is latched
(count since `touchclear`), so it does not need polling every second.

## Line protocol

One command line in, one JSON object line out.

- `{"ok":false,"error":"..."}` is a command the board understood but refused;
  methods record it as `outcome: error` and throw.
- `text` lines are joined with `|`, so a line may not contain `|` or a newline
  (the `text` method validates this).
- Colour names the firmware knows: black white red green blue yellow cyan
  magenta pink. The typed `fill` method accepts the first eight; layouts
  (`screen add`) and `send` accept all nine.
- `send --input line=<command>` sends any firmware command and records the
  reply; use it for commands without a typed method (`status`, `events on`).
- `screen clear` / `screen add <json>` / `screen show` / `screen wait <ms>`
  build a touch layout (`label`, `button` with an `id`, `gap`).

**Tap feedback is drawn by the firmware** before it replies: a zone with id
`approve` or `reject` flashes a full-screen APPROVED (cyan) / REJECTED (pink)
banner; any other zone is outlined in white and the screen stays until the host
draws the next one. (Firmware before 0.11 showed APPROVED for every id.)

**While `screen wait` or `waittouch` runs, the board only watches touch.** From
firmware 0.11 any serial line ends the wait: the line is consumed (not run) and
the wait answers `{"ok":true,"aborted":true}`. The host methods never ask for
more than 2 s per `screen wait`, so a killed process leaves the board busy for
at most one slice.
