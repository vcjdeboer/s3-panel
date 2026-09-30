/**
 * Shared base for swamp model types that drive an ESP32-S3 over USB serial.
 *
 * The contract is a JSON line protocol: the host writes one command line, the
 * board answers with exactly one JSON object on one line. That is all this base
 * assumes, so it serves a bare S3 as well as one with a panel, a sensor, or
 * anything else bolted on. Transport (moving bytes) lives in `serial_link.ts`
 * and `device.ts`; this module gives those lines meaning and turns every
 * exchange into a versioned record with an explicit outcome.
 *
 * Two model types build on this:
 *   - `@vcjdeboer/s3-device` — a plain ESP32-S3. Exactly these methods.
 *   - `@vcjdeboer/s3-panel`  — the same, plus screen methods.
 *
 * swamp resolves extension dependencies only for workflows, so there is no way
 * for one extension's model to import another's at runtime. This file is
 * therefore vendored byte-identically into both extensions, the same way
 * `serial_link.ts` already is. Keep them in sync.
 *
 * @module
 */

import { z } from "npm:zod@4";
import { SerialLink } from "./serial_link.ts";
import {
  type DeviceCtx,
  DevicesSchema,
  HolderSchema,
  holderSocketPath,
  holderState,
  jsonLines,
  lastJsonLine,
  OUTCOME,
  resolveDevice,
  selectDevice,
  stripEscapes,
  withLink,
  WORKER,
} from "./device.ts";
import type { WorkerArduinoFlash } from "./serial_link.ts";

export { jsonLines, stripEscapes, withLink };

/**
 * The `outcome` field every device record carries, re-exported so model types
 * building on this base shape their own records the same way.
 */
export const OUTCOME_FIELD = OUTCOME;

/** Retention for observations that are cheap to regenerate. */
export const OBSERVATIONAL = {
  lifetime: "infinite",
  garbageCollection: 20,
} as const;

/** Retention for exchanges with the board: the record is the point. */
export const EVIDENTIARY = {
  lifetime: "infinite",
  garbageCollection: 10_000,
} as const;

/**
 * Per-instance configuration. Structurally a superset of `DeviceGlobals`, which
 * is what the shared transport glue requires.
 */
export const GlobalArgsSchema = z.object({
  device: z.string().optional().describe(
    "Serial device path: /dev/cu.usbmodemXXXX on macOS, /dev/ttyACM0 on Linux. " +
      "Leave unset to auto-detect when exactly one candidate is attached (see " +
      "the `detect` method). Pin it when more than one board is plugged in, " +
      "and re-check after replugging: the node name follows the USB socket.",
  ),
  baud: z.number().int().positive().default(115200).describe(
    "Line speed. Native USB-CDC ignores it; it matters for UART bridges.",
  ),
  timeoutMs: z.number().int().positive().default(3000).describe(
    "Default hard cap on waiting for the board's reply, per exchange.",
  ),
  idleMs: z.number().int().positive().default(200).describe(
    "End a reply once bytes have arrived and the line has been silent this long.",
  ),
  settleMs: z.number().int().nonnegative().default(100).describe(
    "After opening the port, discard whatever arrives for this long, so a " +
      "previous call's tail or boot banner is not read as this call's reply. " +
      "0 disables.",
  ),
  holder: z.boolean().default(false).describe(
    "Keep the port open between calls in a detached worker (start it with " +
      "`hold`, stop it with `release`). Without it every method opens and " +
      "closes the port, so a reply that arrives after the close is lost — " +
      "which makes separate `write` then `read` calls useless. Leave it on " +
      "unless you have a reason not to.",
  ),
  holderIdleTimeoutMs: z.number().int().positive().default(15 * 60_000)
    .describe("The holder exits after this long without a request."),
  denoPath: z.string().optional().describe(
    "Deno binary used to run the serial worker. Defaults to $SWAMP_DENO, then " +
      "swamp's bundled ~/.swamp/deno/deno.",
  ),
  fqbn: z.string().default(
    "esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB",
  ).describe(
    "Fully Qualified Board Name for arduino-cli. The default is the Guition " +
      "JC3248W535 (ESP32-S3 N16R8). Override per board variant.",
  ),
});

/** Per-instance configuration, resolved. */
export type GlobalArgs = z.infer<typeof GlobalArgsSchema>;

/** The slice of swamp's method context these methods use. */
export interface MethodContext extends DeviceCtx {
  globalArgs: GlobalArgs;
  writeResource: (
    specName: string,
    instanceName: string,
    data: Record<string, unknown>,
  ) => Promise<{ name: string }>;
}

/** One command whose reply is read as the board's current state. */
export const StateSchema = z.object({
  command: z.string().describe("The command line sent, without the newline"),
  state: z.record(z.string(), z.unknown()).describe(
    "The JSON object the board answered with",
  ),
  raw: z.string().describe("Everything the board printed, escapes stripped"),
  outcome: OUTCOME,
  observedAt: z.iso.datetime(),
  elapsedMs: z.number(),
});

/** One arbitrary command line and the single JSON line it produced. */
export const ExchangeSchema = z.object({
  command: z.string().describe("The command line sent, without the newline"),
  response: z.record(z.string(), z.unknown()).describe(
    "The JSON object the board answered with, or {} when none arrived",
  ),
  raw: z.string().describe("Everything the board printed, escapes stripped"),
  timedOut: z.boolean().describe("True when no reply arrived within timeoutMs"),
  reason: z.string().describe("Why the read ended: until, idle or timeout"),
  outcome: OUTCOME,
  observedAt: z.iso.datetime(),
  elapsedMs: z.number(),
});

/** Raw bytes written to the board, with no reply awaited. */
export const SentSchema = z.object({
  data: z.string().describe("The bytes written, exactly as given"),
  bytes: z.number().int().nonnegative(),
  outcome: OUTCOME,
  observedAt: z.iso.datetime(),
});

/** Whatever the board printed during a listening window. */
export const CaptureSchema = z.object({
  timeoutMs: z.number().int().positive(),
  data: z.string().describe("Everything captured, escapes stripped"),
  bytes: z.number().int().nonnegative(),
  reason: z.string().describe("Why the read ended: idle or timeout"),
  outcome: OUTCOME,
  observedAt: z.iso.datetime(),
  elapsedMs: z.number(),
});

/** One arduino-cli compile + upload cycle. */
export const FlashSchema = z.object({
  sketchPath: z.string().describe("Path to the Arduino sketch directory"),
  fqbn: z.string().describe("Fully Qualified Board Name"),
  device: z.string().describe("Serial device used for upload"),
  compileOutput: z.string().describe("Tail of arduino-cli compile output"),
  uploadOutput: z.string().describe("Tail of arduino-cli upload output"),
  portBackAfterMs: z.number().describe(
    "How long the port took to reappear after the board reset",
  ),
  outcome: OUTCOME,
  observedAt: z.iso.datetime(),
  elapsedMs: z.number(),
});

/** The board's Wi-Fi link as it reports it. Never carries a credential. */
export const WifiSchema = z.object({
  state: z.string().describe(
    "The firmware's own state name (s3panel: setup, connecting, badge, host)",
  ),
  connected: z.boolean(),
  ip: z.string(),
  rssi: z.number(),
  mac: z.string().describe("The station MAC"),
  outcome: OUTCOME,
  observedAt: z.iso.datetime(),
  elapsedMs: z.number(),
});

/** A change to the board's stored settings: which keys, never their values. */
export const ConfigSchema = z.object({
  operation: z.enum(["configure", "forget"]),
  stored: z.array(z.string()).describe("Names of the keys the board stored"),
  joined: z.boolean().nullable().describe(
    "Whether the board is on Wi-Fi afterwards; null when it did not say",
  ),
  forgotten: z.boolean(),
  error: z.string().nullable().describe("The board's reason for refusing"),
  outcome: OUTCOME,
  observedAt: z.iso.datetime(),
  elapsedMs: z.number(),
});

/**
 * A Wi-Fi password the board accepts: empty for an open network, else 8 to 63
 * characters. Sensitive: pass it from a vault expression, never inline.
 */
export const WifiPasswordSchema = z.string().max(63).refine(
  (p) => p.length === 0 || p.length >= 8,
  "a Wi-Fi password is empty (open network) or 8 to 63 characters",
).meta({
  sensitive: true,
  description:
    "Wi-Fi password; use ${{ vault.get(<vault>, <key>) }}, never a literal",
});

/**
 * The `config set` line for Wi-Fi credentials. JSON keeps quotes, spaces,
 * separators and even newlines inside one protocol line, byte for byte.
 */
export function configureLine(ssid: string, password: string): string {
  return "config set " + JSON.stringify({ ssid, pass: password });
}

type Reply = Record<string, unknown> | null;

/** The Wi-Fi fields of a `wifi status` reply, with defaults. */
export function wifiFields(response: Reply) {
  const r = response ?? {};
  return {
    state: typeof r.state === "string" ? r.state : "",
    connected: r.connected === true,
    ip: typeof r.ip === "string" ? r.ip : "",
    rssi: typeof r.rssi === "number" ? r.rssi : 0,
    mac: typeof r.mac === "string" ? r.mac : "",
  };
}

const CONFIG_KEYS = ["ssid", "pass", "profile", "api"];

/**
 * The fields of a `config` reply worth recording. A whitelist: whatever else a
 * board might say is dropped, so a value can never leak into the datastore.
 */
export function configFields(response: Reply) {
  const r = response ?? {};
  return {
    stored: Array.isArray(r.stored)
      ? r.stored.filter((s): s is string =>
        typeof s === "string" && CONFIG_KEYS.includes(s)
      )
      : [],
    joined: typeof r.joined === "boolean" ? r.joined : null,
    forgotten: r.forgotten === true,
    error: typeof r.error === "string" ? r.error : null,
  };
}

/** The resource specs every S3 model type shares. */
export function baseResources(): Record<string, unknown> {
  return {
    "devices": {
      description:
        "Serial device nodes on this host and which one this model would use",
      schema: DevicesSchema,
      ...OBSERVATIONAL,
    },
    "state": {
      description: "A state-reading command and the state the board returned",
      schema: StateSchema,
      ...EVIDENTIARY,
    },
    "exchange": {
      description: "One arbitrary command line and the board's JSON reply",
      schema: ExchangeSchema,
      ...EVIDENTIARY,
    },
    "sent": {
      description: "Raw bytes written to the board, with no reply awaited",
      schema: SentSchema,
      ...EVIDENTIARY,
    },
    "capture": {
      description: "Whatever the board printed during a listening window",
      schema: CaptureSchema,
      ...EVIDENTIARY,
    },
    "holder": {
      description:
        "Whether a detached worker is keeping the port open, and what it holds",
      schema: HolderSchema,
      ...OBSERVATIONAL,
    },
    "flash": {
      description: "One arduino-cli compile + upload cycle and its result",
      schema: FlashSchema,
      ...EVIDENTIARY,
    },
    "wifi": {
      description: "The board's Wi-Fi link: state, connected, address, signal",
      schema: WifiSchema,
      ...OBSERVATIONAL,
    },
    "config": {
      description:
        "A change to the board's stored settings (names of keys, never values)",
      schema: ConfigSchema,
      ...EVIDENTIARY,
    },
  };
}

/** The outcome of one line-protocol exchange. */
export interface LineResult {
  response: Record<string, unknown> | null;
  raw: string;
  reason: string;
}

/**
 * Send one command line and wait for the single JSON object the board answers
 * with. Ends on the closing brace, on an idle line, or at `timeoutMs` —
 * whichever comes first. A missing reply is returned, never thrown, so the
 * caller can record the evidence before deciding what it means.
 */
export async function sendLine(
  ctx: MethodContext,
  line: string,
  timeoutMs: number,
): Promise<LineResult> {
  const g = ctx.globalArgs;
  return await withLink(ctx, async ({ link }) => {
    const r = await link.query(line + "\n", "}", timeoutMs, g.idleMs);
    const raw = stripEscapes(r.data ?? "");
    return { response: lastJsonLine(raw), raw, reason: r.reason ?? "timeout" };
  });
}

/**
 * Read the board's own report of a failure. The firmware answers
 * `{"ok":false,"error":"..."}` for a command it understood but could not
 * carry out, which is an error outcome even though the exchange succeeded.
 */
export function replyFailed(response: Record<string, unknown> | null): boolean {
  return response !== null && response.ok === false;
}

/**
 * Build a state-reading method: send one fixed command, record the reply as the
 * board's state. Used for `ping` and `status`, and reusable by any model type
 * whose firmware answers a similar one-shot query.
 */
export function stateMethod(command: string, description: string): unknown {
  return {
    description,
    arguments: z.object({
      timeoutMs: z.number().int().positive().optional().describe(
        "Overrides the instance's timeoutMs for this call",
      ),
    }),
    execute: async (
      args: { timeoutMs?: number },
      ctx: MethodContext,
    ) => {
      const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
      const t0 = performance.now();
      const r = await sendLine(ctx, command, timeoutMs);
      const outcome = !r.response
        ? "timeout"
        : (replyFailed(r.response) ? "error" : "ok");
      ctx.logger.info("{command}: {outcome}", { command, outcome });
      const handle = await ctx.writeResource("state", "state-latest", {
        command,
        state: r.response ?? {},
        raw: r.raw,
        outcome,
        observedAt: new Date().toISOString(),
        elapsedMs: Math.round(performance.now() - t0),
      });
      if (!r.response) {
        throw new Error(
          `no JSON reply to ${
            JSON.stringify(command)
          } within ${timeoutMs} ms ` +
            `(got ${
              JSON.stringify(r.raw.slice(-160))
            }; recorded as state-latest, ` +
            "outcome=timeout). Is the firmware running, and is `holder` on?",
        );
      }
      return { dataHandles: [handle] };
    },
  };
}

/**
 * The methods every S3 model type shares: find the board, ask it how it is,
 * send it anything, and manage the held port.
 */
export function baseMethods(): Record<string, unknown> {
  return {
    detect: {
      description:
        "List serial device nodes on this host without opening any, and record " +
        "which one this model would use.",
      arguments: z.object({}),
      execute: async (_args: Record<string, never>, ctx: MethodContext) => {
        const g = ctx.globalArgs;
        const link = await SerialLink.create({
          workerPath: ctx.extensionFile(WORKER),
          denoPath: g.denoPath,
        });
        let d;
        try {
          d = await link.detect();
        } finally {
          await link.close();
        }
        const selected = selectDevice(g.device, d.candidates);
        const handle = await ctx.writeResource("devices", "devices-host", {
          os: d.os,
          candidates: d.candidates,
          selected,
          outcome: "ok",
          observedAt: new Date().toISOString(),
        });
        return { dataHandles: [handle] };
      },
    },

    ping: stateMethod(
      "ping",
      "Ask the board to identify itself and record the firmware name it " +
        "reports. The cheapest proof that the right firmware is running and " +
        "the link works.",
    ),

    status: stateMethod(
      "status",
      "Read the board's self-reported status and record it. What the fields " +
        "mean is up to the firmware.",
    ),

    wifi: {
      description:
        "Ask the board for its Wi-Fi link: its state, whether it is connected, " +
        "its address and signal. Records wifi-latest. Never reports a credential.",
      arguments: z.object({
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (args: { timeoutMs?: number }, ctx: MethodContext) => {
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        const t0 = performance.now();
        const r = await sendLine(ctx, "wifi status", timeoutMs);
        const outcome = !r.response
          ? "timeout"
          : (replyFailed(r.response) ? "error" : "ok");
        const fields = wifiFields(r.response);
        ctx.logger.info("wifi: {outcome} state={state} connected={connected}", {
          outcome,
          state: fields.state,
          connected: fields.connected,
        });
        const handle = await ctx.writeResource("wifi", "wifi-latest", {
          ...fields,
          outcome,
          observedAt: new Date().toISOString(),
          elapsedMs: Math.round(performance.now() - t0),
        });
        if (!r.response) {
          throw new Error(
            `no reply to wifi status within ${timeoutMs} ms ` +
              "(recorded as wifi-latest, outcome=timeout)",
          );
        }
        if (outcome === "error") {
          throw new Error(
            `the board refused wifi status: ${
              JSON.stringify(r.response.error ?? "no reason given")
            } (firmware older than s3panel 0.12?)`,
          );
        }
        return { dataHandles: [handle] };
      },
    },

    configure: {
      description:
        "Store Wi-Fi credentials on the board over USB. The board joins the " +
        "network before saving and refuses (error=join) if it cannot, so a " +
        "wrong password never replaces a working one. Pass the password from " +
        "a vault expression; it is never recorded or logged.",
      arguments: z.object({
        ssid: z.string().min(1).max(32).describe("Wi-Fi network name"),
        password: WifiPasswordSchema,
        joinMs: z.number().int().positive().default(30_000).describe(
          "Wait for the board to join and answer; named joinMs so the global " +
            "timeoutMs does not clobber this default",
        ),
      }),
      execute: async (
        args: { ssid: string; password: string; joinMs: number },
        ctx: MethodContext,
      ) => {
        const t0 = performance.now();
        const r = await sendLine(
          ctx,
          configureLine(args.ssid, args.password),
          args.joinMs,
        );
        const outcome = !r.response
          ? "timeout"
          : (replyFailed(r.response) ? "error" : "ok");
        const fields = configFields(r.response);
        ctx.logger.info(
          "configure: {outcome} stored={stored} joined={joined}",
          {
            outcome,
            stored: fields.stored.join(","),
            joined: fields.joined,
          },
        );
        const handle = await ctx.writeResource("config", "config-latest", {
          operation: "configure",
          ...fields,
          outcome,
          observedAt: new Date().toISOString(),
          elapsedMs: Math.round(performance.now() - t0),
        });
        if (!r.response) {
          throw new Error(
            `no reply to config set within ${args.joinMs} ms ` +
              "(recorded as config-latest, outcome=timeout)",
          );
        }
        if (outcome === "error") {
          throw new Error(
            "the board did not store the Wi-Fi settings: " +
              `${
                fields.error ?? "no reason given"
              } (recorded as config-latest)`,
          );
        }
        return { dataHandles: [handle] };
      },
    },

    forget: {
      description:
        "Factory-reset the board's stored settings (Wi-Fi and anything else " +
        "the firmware keeps) so it boots into setup. Refuses unless confirm=true.",
      arguments: z.object({
        confirm: z.boolean().default(false).describe(
          "Must be true: this wipes the board's settings",
        ),
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (
        args: { confirm: boolean; timeoutMs?: number },
        ctx: MethodContext,
      ) => {
        if (!args.confirm) {
          throw new Error(
            "forget wipes the board's stored settings; pass --input confirm=true to do it",
          );
        }
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        const t0 = performance.now();
        const r = await sendLine(ctx, "config forget", timeoutMs);
        const outcome = !r.response
          ? "timeout"
          : (replyFailed(r.response) ? "error" : "ok");
        const fields = configFields(r.response);
        ctx.logger.info("forget: {outcome}", { outcome });
        const handle = await ctx.writeResource("config", "config-latest", {
          operation: "forget",
          ...fields,
          outcome,
          observedAt: new Date().toISOString(),
          elapsedMs: Math.round(performance.now() - t0),
        });
        if (!r.response) {
          throw new Error(
            `no reply to config forget within ${timeoutMs} ms ` +
              "(recorded as config-latest, outcome=timeout)",
          );
        }
        if (outcome === "error") {
          throw new Error(
            `the board refused config forget: ${
              fields.error ?? "no reason given"
            }`,
          );
        }
        return { dataHandles: [handle] };
      },
    },

    send: {
      description:
        "Send one arbitrary command line and record the single JSON object the " +
        "board answers with. The escape hatch for firmware commands this model " +
        "type has no dedicated method for. No reply within timeoutMs is " +
        "recorded as outcome=timeout, then thrown.",
      arguments: z.object({
        line: z.string().describe("The command line, without the newline"),
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (
        args: { line: string; timeoutMs?: number },
        ctx: MethodContext,
      ) => {
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        const t0 = performance.now();
        const r = await sendLine(ctx, args.line, timeoutMs);
        const timedOut = !r.response;
        const outcome = timedOut
          ? "timeout"
          : (replyFailed(r.response) ? "error" : "ok");
        ctx.logger.info("send {line}: {outcome}", {
          line: args.line,
          outcome,
        });
        const handle = await ctx.writeResource("exchange", "exchange-latest", {
          command: args.line,
          response: r.response ?? {},
          raw: r.raw,
          timedOut,
          reason: r.reason,
          outcome,
          observedAt: new Date().toISOString(),
          elapsedMs: Math.round(performance.now() - t0),
        });
        if (timedOut) {
          throw new Error(
            `no JSON reply to ${JSON.stringify(args.line)} within ` +
              `${timeoutMs} ms (got ${
                JSON.stringify(r.raw.slice(-160))
              }; recorded ` +
              "as exchange-latest, outcome=timeout).",
          );
        }
        return { dataHandles: [handle] };
      },
    },

    write: {
      description:
        "Write bytes to the board and record them, without waiting for a " +
        "reply. Sent exactly as given: escape sequences are NOT decoded, so " +
        "pass a real newline, not a backslash-n. Prefer `send` for anything " +
        "that answers.",
      arguments: z.object({
        data: z.string().describe("The bytes to write, verbatim"),
      }),
      execute: async (args: { data: string }, ctx: MethodContext) => {
        const r = await withLink(ctx, ({ link }) => link.write(args.data));
        if (!r.ok) throw new Error(`write failed: ${r.error ?? "unknown"}`);
        const handle = await ctx.writeResource("sent", "sent-latest", {
          data: args.data,
          bytes: new TextEncoder().encode(args.data).length,
          outcome: "ok",
          observedAt: new Date().toISOString(),
        });
        return { dataHandles: [handle] };
      },
    },

    read: {
      description:
        "Listen for up to timeoutMs and record whatever the board prints, " +
        "without sending anything. Stops early once output has arrived and the " +
        "line has been silent for idleMs. Useful for firmware that streams " +
        "events, or for reading a boot banner.",
      arguments: z.object({
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
        idleMs: z.number().int().positive().optional().describe(
          "Overrides the instance's idleMs for this call",
        ),
      }),
      execute: async (
        args: { timeoutMs?: number; idleMs?: number },
        ctx: MethodContext,
      ) => {
        const g = ctx.globalArgs;
        const timeoutMs = args.timeoutMs ?? g.timeoutMs;
        const idleMs = args.idleMs ?? g.idleMs;
        const t0 = performance.now();
        const r = await withLink(
          ctx,
          ({ link }) => link.read(timeoutMs, idleMs),
        );
        if (!r.ok) throw new Error(`read failed: ${r.error ?? "unknown"}`);
        const data = stripEscapes(r.data ?? "");
        const handle = await ctx.writeResource("capture", "capture-latest", {
          timeoutMs,
          data,
          bytes: new TextEncoder().encode(data).length,
          reason: r.reason ?? "timeout",
          outcome: "ok",
          observedAt: new Date().toISOString(),
          elapsedMs: Math.round(performance.now() - t0),
        });
        return { dataHandles: [handle] };
      },
    },

    hold: {
      description:
        "Start a detached worker that keeps the port open between calls " +
        "(idempotent: reports the running one). Methods use it automatically " +
        "while holder is true. It exits on `release` or after " +
        "holderIdleTimeoutMs without a request.",
      arguments: z.object({}),
      execute: async (_args: Record<string, never>, ctx: MethodContext) => {
        const g = ctx.globalArgs;
        const socket = await holderSocketPath(ctx.modelId);
        if (!(await SerialLink.holderStatus(socket))) {
          const pid = await SerialLink.spawnHolder({
            workerPath: ctx.extensionFile(WORKER),
            socketPath: socket,
            denoPath: g.denoPath,
            idleTimeoutMs: g.holderIdleTimeoutMs,
          });
          ctx.logger.info("holder started, pid {pid}", { pid });
        }
        const link = await SerialLink.create({ socketPath: socket });
        try {
          const st = await link.status();
          if (!st.open) {
            const device = await resolveDevice(ctx, link);
            const opened = await link.open(device, g.baud);
            if (!opened.ok) {
              throw new Error(
                `cannot open ${device}: ${opened.error ?? "unknown"}`,
              );
            }
          }
        } finally {
          await link.close();
        }
        const handle = await ctx.writeResource(
          "holder",
          "holder-current",
          await holderState(ctx),
        );
        return { dataHandles: [handle] };
      },
    },

    release: {
      description:
        "Stop the holder: close the port and let the detached worker exit. " +
        "Safe when none is running. Do this before flashing the board, since " +
        "a held port blocks the upload.",
      arguments: z.object({}),
      execute: async (_args: Record<string, never>, ctx: MethodContext) => {
        const socket = await holderSocketPath(ctx.modelId);
        const before = await holderState(ctx);
        if (before.live) {
          const link = await SerialLink.create({ socketPath: socket });
          await link.release();
        }
        const deadline = Date.now() + 3000;
        while (Date.now() < deadline && await SerialLink.holderLive(socket)) {
          await new Promise((r) => setTimeout(r, 50));
        }
        const live = await SerialLink.holderLive(socket);
        const handle = await ctx.writeResource("holder", "holder-current", {
          ...before,
          live,
          outcome: live ? "error" : "ok",
          observedAt: new Date().toISOString(),
        });
        return { dataHandles: [handle] };
      },
    },

    flash: {
      description:
        "Compile and upload an Arduino sketch with arduino-cli, then wait " +
        "for the port to come back. Runs arduino-cli inside the serial " +
        "worker (a real deno child), because its esptool backend stalls " +
        "when spawned from swamp's own runtime. Auto-releases the holder " +
        "before flashing and re-holds after. Records compile output, " +
        "upload output, and timing. Needs arduino-cli on PATH with the " +
        "esp32 core installed.",
      arguments: z.object({
        sketchPath: z.string().describe(
          "Path to the Arduino sketch directory, relative to the repo or absolute",
        ),
        fqbn: z.string().optional().describe(
          "Override the instance's FQBN for this flash",
        ),
        arduinoCliPath: z.string().default("arduino-cli").describe(
          "arduino-cli binary",
        ),
        flashTimeoutMs: z.number().int().positive().default(180_000).describe(
          "Timeout for compile + upload combined; named flashTimeoutMs so " +
            "the global timeoutMs does not clobber it",
        ),
      }),
      execute: async (
        args: {
          sketchPath: string;
          fqbn?: string;
          arduinoCliPath: string;
          flashTimeoutMs: number;
        },
        ctx: MethodContext,
      ) => {
        const g = ctx.globalArgs;
        const t0 = performance.now();
        const fqbn = args.fqbn ?? g.fqbn;

        // The holder must be running: arduino-cli is spawned as its child, so
        // it runs outside swamp's process tree (same reason as esptool).
        const socket = await holderSocketPath(ctx.modelId);
        let holder = await SerialLink.holderStatus(socket);
        if (!holder) {
          ctx.logger.info(
            "starting a detached holder to run arduino-cli outside swamp's process tree",
          );
          await SerialLink.spawnHolder({
            workerPath: ctx.extensionFile(WORKER),
            socketPath: socket,
            denoPath: g.denoPath,
            idleTimeoutMs: g.holderIdleTimeoutMs,
          });
          holder = await SerialLink.holderStatus(socket);
        }
        if (!holder) {
          throw new Error("could not start a holder for flashing");
        }

        // Resolve the device before releasing the port.
        const deviceLink = await SerialLink.create({ socketPath: socket });
        let device: string;
        try {
          device = await resolveDevice(ctx, deviceLink);
        } finally {
          await deviceLink.close();
        }

        ctx.logger.info("flashing {sketch} to {device} (fqbn: {fqbn})", {
          sketch: args.sketchPath,
          device,
          fqbn,
        });

        const link = await SerialLink.create({
          socketPath: socket,
          callTimeoutMs: args.flashTimeoutMs + 20_000,
        });
        let r: WorkerArduinoFlash;
        try {
          r = await link.arduinoFlash({
            device,
            sketchPath: args.sketchPath,
            fqbn,
            arduinoCliPath: args.arduinoCliPath,
            timeoutMs: args.flashTimeoutMs,
          });
        } finally {
          await link.close();
        }
        if (!r.ok) throw new Error(`flash failed: ${r.error}`);

        ctx.logger.info("flashed in {elapsed} ms, port back in {portBack} ms", {
          elapsed: r.elapsedMs,
          portBack: r.portBackAfterMs,
        });

        // Re-hold so the board is ready for the next method call.
        const reLink = await SerialLink.create({ socketPath: socket });
        try {
          const st = await reLink.status();
          if (!st.open) {
            const opened = await reLink.open(device, g.baud);
            if (!opened.ok) {
              ctx.logger.warning("re-hold after flash failed: {err}", {
                err: opened.error ?? "unknown",
              });
            }
          }
        } finally {
          await reLink.close();
        }

        const handle = await ctx.writeResource("flash", "flash-latest", {
          sketchPath: args.sketchPath,
          fqbn,
          device,
          compileOutput: r.compileOutput,
          uploadOutput: r.uploadOutput,
          portBackAfterMs: r.portBackAfterMs,
          outcome: "ok",
          observedAt: new Date().toISOString(),
          elapsedMs: Math.round(performance.now() - t0),
        });
        return { dataHandles: [handle] };
      },
    },
  };
}
