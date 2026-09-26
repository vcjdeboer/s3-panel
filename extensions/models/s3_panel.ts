/**
 * `@vcjdeboer/s3-panel` — an ESP32-S3 with a screen, driven from swamp.
 *
 * Everything `@vcjdeboer/s3-device` does, plus first-class methods for the
 * display: `text`, `fill`, `clear` and `backlight`. The board renders; the host
 * says what to render. Pixels are never pushed over the wire, which is what
 * makes a 320x480 panel usable over a 115200-baud-shaped link at all.
 *
 * The screen commands are reachable from the plain `s3-device` type too, via its
 * generic `send`. What this type adds is a typed surface: a colour that must be
 * one the firmware knows, lines validated before they can corrupt the protocol,
 * and a record shaped like a drawing operation rather than an opaque exchange.
 *
 * swamp resolves extension dependencies only for workflows, so a model cannot
 * import another extension's code at runtime. The shared base therefore lives in
 * `_lib/s3_base.ts`, vendored byte-identically here and in `@vcjdeboer/esp32-s3`
 * — the same arrangement `serial_link.ts` already has. Keep them in sync.
 *
 * Built for the Guition JC3248W535 (3.5" 320x480 AXS15231B panel) running the
 * `s3panel` firmware, but nothing here is specific to that panel: any firmware
 * answering the same commands will do.
 *
 * Verified 2026-09-26 against that board over its native USB-Serial/JTAG port.
 *
 * @module
 */

import { z } from "npm:zod@4";
import {
  baseMethods,
  baseResources,
  EVIDENTIARY,
  GlobalArgsSchema,
  type MethodContext,
  OUTCOME_FIELD,
  replyFailed,
  sendLine,
} from "./_lib/s3_base.ts";

/** Colours the reference firmware knows by name. */
const COLORS = [
  "black",
  "white",
  "red",
  "green",
  "blue",
  "yellow",
  "cyan",
  "magenta",
] as const;

/** The firmware splits lines on this character, so a line may not contain it. */
const LINE_SEPARATOR = "|";

/** One drawing operation and what the board reported back. */
const DrawSchema = z.object({
  command: z.string().describe("The command line sent, without the newline"),
  operation: z.string().describe(
    "Which drawing method ran: text, fill, clear or backlight",
  ),
  result: z.record(z.string(), z.unknown()).describe(
    "The JSON object the board answered with",
  ),
  raw: z.string().describe("Everything the board printed, escapes stripped"),
  outcome: OUTCOME_FIELD,
  observedAt: z.iso.datetime(),
  elapsedMs: z.number(),
});

/**
 * Run one drawing command: send it, judge the reply, record it as a drawing
 * operation. A missing reply is recorded as a timeout before it is thrown, so
 * the evidence survives a wedged board.
 */
async function draw(
  ctx: MethodContext,
  operation: string,
  line: string,
  timeoutMs: number,
): Promise<{ dataHandles: { name: string }[] }> {
  const t0 = performance.now();
  const r = await sendLine(ctx, line, timeoutMs);
  const failed = replyFailed(r.response);
  const outcome = !r.response ? "timeout" : (failed ? "error" : "ok");
  ctx.logger.info("{operation}: {outcome}", { operation, outcome });
  const handle = await ctx.writeResource("draw", "draw-latest", {
    command: line,
    operation,
    result: r.response ?? {},
    raw: r.raw,
    outcome,
    observedAt: new Date().toISOString(),
    elapsedMs: Math.round(performance.now() - t0),
  });
  if (!r.response) {
    throw new Error(
      `no reply to ${JSON.stringify(line)} within ${timeoutMs} ms ` +
        `(got ${JSON.stringify(r.raw.slice(-160))}; recorded as draw-latest, ` +
        "outcome=timeout). Is the firmware running, and is `holder` on?",
    );
  }
  if (failed) {
    throw new Error(
      `the board refused ${JSON.stringify(line)}: ` +
        `${JSON.stringify(r.response?.error ?? "no reason given")} ` +
        "(recorded as draw-latest, outcome=error)",
    );
  }
  return { dataHandles: [handle] };
}

/**
 * Reject lines that would corrupt the wire format before anything is sent. The
 * firmware splits on `|` and reads one command per line, so either character
 * inside a caller's text would silently turn into extra lines or extra commands.
 */
export function assertDrawable(lines: string[]): void {
  if (lines.length === 0) {
    throw new Error("text needs at least one line (use `clear` for a blank screen)");
  }
  for (const line of lines) {
    if (line.includes(LINE_SEPARATOR)) {
      throw new Error(
        `a line may not contain ${JSON.stringify(LINE_SEPARATOR)}: it is the ` +
          `line separator on the wire. Offending line: ${JSON.stringify(line)}`,
      );
    }
    if (/[\r\n]/.test(line)) {
      throw new Error(
        "a line may not contain a carriage return or newline: it would end " +
          `the command. Offending line: ${JSON.stringify(line)}`,
      );
    }
  }
}

/** Model definition for an ESP32-S3 carrying a display. */
export const model = {
  type: "@vcjdeboer/s3-panel",
  version: "2026.09.26.3",
  globalArguments: GlobalArgsSchema,
  resources: {
    ...baseResources(),
    "draw": {
      description: "One drawing operation and what the board reported back",
      schema: DrawSchema,
      ...EVIDENTIARY,
    },
  },
  methods: {
    ...baseMethods(),

    text: {
      description:
        "Draw lines of text, replacing whatever was on the screen. Pass one " +
        "string per line; they are joined with the firmware's separator for " +
        "you, and rejected up front if they contain a character that would " +
        "corrupt the wire format. The firmware draws the first 12 lines and " +
        "truncates each to the panel width.",
      arguments: z.object({
        lines: z.array(z.string()).min(1).describe(
          "One string per screen line, drawn top to bottom",
        ),
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (
        args: { lines: string[]; timeoutMs?: number },
        ctx: MethodContext,
      ) => {
        assertDrawable(args.lines);
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        return await draw(
          ctx,
          "text",
          "text " + args.lines.join(LINE_SEPARATOR),
          timeoutMs,
        );
      },
    },

    fill: {
      description:
        "Flood the whole screen with one colour. The cheapest way to see, " +
        "across a room, whether something happened.",
      arguments: z.object({
        color: z.enum(COLORS).describe("A colour the firmware knows by name"),
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (
        args: { color: typeof COLORS[number]; timeoutMs?: number },
        ctx: MethodContext,
      ) => {
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        return await draw(ctx, "fill", "fill " + args.color, timeoutMs);
      },
    },

    clear: {
      description: "Blank the screen to black, leaving the backlight alone.",
      arguments: z.object({
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (args: { timeoutMs?: number }, ctx: MethodContext) => {
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        return await draw(ctx, "clear", "clear", timeoutMs);
      },
    },

    backlight: {
      description:
        "Turn the backlight on or off. Off leaves the framebuffer intact, so " +
        "turning it back on shows the same screen: a dark panel is not " +
        "evidence that drawing failed.",
      arguments: z.object({
        on: z.boolean().describe("True for lit, false for dark"),
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (
        args: { on: boolean; timeoutMs?: number },
        ctx: MethodContext,
      ) => {
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        return await draw(
          ctx,
          "backlight",
          "backlight " + (args.on ? "on" : "off"),
          timeoutMs,
        );
      },
    },

    logo: {
      description:
        "Show the Swamp Club logo on screen and enter touch-reactive mode. " +
        "Touching the screen triggers a particle explosion, after which the " +
        "logo redraws. Any other drawing command exits logo mode.",
      arguments: z.object({
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (args: { timeoutMs?: number }, ctx: MethodContext) => {
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        return await draw(ctx, "logo", "logo", timeoutMs);
      },
    },
  },
};
