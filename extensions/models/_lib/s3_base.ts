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
  lastJsonLine,
  OUTCOME,
  resolveDevice,
  selectDevice,
  stripEscapes,
  withLink,
  WORKER,
} from "./device.ts";
import type { WorkerArduinoFlash } from "./serial_link.ts";

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
