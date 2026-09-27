/**
 * `@vcjdeboer/s3-panel` — an ESP32-S3 with a screen, driven from swamp.
 *
 * Everything `@vcjdeboer/s3-device` does, plus first-class methods for the
 * display (`text`, `fill`, `clear`, `backlight`) and touch controller (`touch`,
 * `touchstate`, `touchclear`, `waittouch`, `simtouch`). The board renders and
 * latches touches; the host says what to render and reads what was touched.
 * Pixels are never pushed over the wire, which is what makes a 320x480 panel
 * usable over a 115200-baud-shaped link at all.
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
  jsonLines,
  type MethodContext,
  OBSERVATIONAL,
  OUTCOME_FIELD,
  replyFailed,
  sendLine,
  stripEscapes,
  withLink,
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

/** A touch event recorded from the panel's touch controller or simulated. */
const TouchSchema = z.object({
  command: z.string().describe("The command that produced this reading"),
  points: z.number().int().describe("Number of touch points detected (0 or 1)"),
  x: z.number().int().describe("X coordinate of the touch (0-319)"),
  y: z.number().int().describe("Y coordinate of the touch (0-479)"),
  gesture: z.number().int().optional().describe(
    "Gesture code from the controller",
  ),
  simulated: z.boolean().optional().describe(
    "True when the touch was simulated",
  ),
  waitMs: z.number().optional().describe(
    "How long waittouch blocked before the touch",
  ),
  outcome: OUTCOME_FIELD,
  observedAt: z.iso.datetime(),
  elapsedMs: z.number(),
});

/** The latched touch state: count since last clear, last coordinates. */
const TouchStateSchema = z.object({
  count: z.number().int().describe("Touches since last touchclear"),
  x: z.number().int().describe("X coordinate of the last touch"),
  y: z.number().int().describe("Y coordinate of the last touch"),
  t: z.number().describe("millis() timestamp of the last touch on the board"),
  outcome: OUTCOME_FIELD,
  observedAt: z.iso.datetime(),
  elapsedMs: z.number(),
});

/** Unsolicited events captured during a listen window. */
const EventsSchema = z.object({
  events: z.array(z.record(z.string(), z.unknown())).describe(
    'Unsolicited event lines (JSON with an "event" field) captured in the window',
  ),
  count: z.number(),
  timeoutMs: z.number(),
  raw: z.string(),
  outcome: OUTCOME_FIELD,
  observedAt: z.iso.datetime(),
  elapsedMs: z.number(),
});

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
    throw new Error(
      "text needs at least one line (use `clear` for a blank screen)",
    );
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
  version: "2026.09.27.3",
  globalArguments: GlobalArgsSchema,
  resources: {
    ...baseResources(),
    "events": {
      description:
        "Unsolicited events the device pushed during a listen window",
      schema: EventsSchema,
      ...EVIDENTIARY,
    },
    "draw": {
      description: "One drawing operation and what the board reported back",
      schema: DrawSchema,
      ...EVIDENTIARY,
    },
    "touch": {
      description:
        "A touch event from the panel's touch controller or simulated",
      schema: TouchSchema,
      ...EVIDENTIARY,
    },
    "touchstate": {
      description:
        "The latched touch state: count since last clear, last coordinates",
      schema: TouchStateSchema,
      ...OBSERVATIONAL,
    },
  },
  methods: {
    ...baseMethods(),

    listen: {
      description:
        "Listen for unsolicited event lines for timeoutMs. The board pushes " +
        '{"event":"tap",...} lines when events are enabled (`events on`). ' +
        "Each tap carries its sequence number, coordinates and timestamp. " +
        "Returns all captured events as an array.",
      arguments: z.object({
        timeoutMs: z.number().int().positive().optional().describe(
          "How long to listen; defaults to the global timeoutMs",
        ),
      }),
      execute: async (args: { timeoutMs?: number }, ctx: MethodContext) => {
        const g = ctx.globalArgs;
        const timeoutMs = args.timeoutMs ?? g.timeoutMs;
        const t0 = performance.now();
        const r = await withLink(ctx, ({ link }) => link.read(timeoutMs));
        if (!r.ok) throw new Error(`listen failed: ${r.error}`);
        const raw = stripEscapes(r.data ?? "");
        const events = jsonLines(raw).filter((o) => "event" in o);
        ctx.logger.info("captured {n} event(s) in {ms} ms", {
          n: events.length,
          ms: timeoutMs,
        });
        const handle = await ctx.writeResource("events", "events-latest", {
          events,
          count: events.length,
          timeoutMs,
          raw,
          outcome: "ok",
          observedAt: new Date().toISOString(),
          elapsedMs: Math.round(performance.now() - t0),
        });
        return { dataHandles: [handle] };
      },
    },

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

    touch: {
      description:
        "Read the touch controller right now: is a finger on the screen? " +
        "Returns the coordinates if yes, points=0 if no. Also latches the " +
        "touch into the firmware's counter so touchstate can report it later.",
      arguments: z.object({
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (args: { timeoutMs?: number }, ctx: MethodContext) => {
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        const t0 = performance.now();
        const r = await sendLine(ctx, "touch", timeoutMs);
        const failed = replyFailed(r.response);
        const points = (r.response?.points as number) ?? 0;
        const outcome = !r.response ? "timeout" : (failed ? "error" : "ok");
        ctx.logger.info("touch: {outcome}, points={points}", {
          outcome,
          points,
        });
        const handle = await ctx.writeResource("touch", "touch-latest", {
          command: "touch",
          points,
          x: (r.response?.x as number) ?? 0,
          y: (r.response?.y as number) ?? 0,
          gesture: (r.response?.gesture as number) ?? undefined,
          outcome,
          observedAt: new Date().toISOString(),
          elapsedMs: Math.round(performance.now() - t0),
        });
        if (!r.response) {
          throw new Error(
            `no reply to touch within ${timeoutMs} ms (recorded as ` +
              "touch-latest, outcome=timeout)",
          );
        }
        return { dataHandles: [handle] };
      },
    },

    touchstate: {
      description:
        "Read the latched touch state: how many touches since the last " +
        "touchclear, and where the last one landed. The count never resets " +
        "on its own, so the host never misses a touch.",
      arguments: z.object({
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (args: { timeoutMs?: number }, ctx: MethodContext) => {
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        const t0 = performance.now();
        const r = await sendLine(ctx, "touchstate", timeoutMs);
        const failed = replyFailed(r.response);
        const outcome = !r.response ? "timeout" : (failed ? "error" : "ok");
        const handle = await ctx.writeResource(
          "touchstate",
          "touchstate-latest",
          {
            count: (r.response?.count as number) ?? 0,
            x: (r.response?.x as number) ?? 0,
            y: (r.response?.y as number) ?? 0,
            t: (r.response?.t as number) ?? 0,
            outcome,
            observedAt: new Date().toISOString(),
            elapsedMs: Math.round(performance.now() - t0),
          },
        );
        if (!r.response) {
          throw new Error(
            `no reply to touchstate within ${timeoutMs} ms (recorded as ` +
              "touchstate-latest, outcome=timeout)",
          );
        }
        return { dataHandles: [handle] };
      },
    },

    touchclear: {
      description:
        "Reset the touch counter to zero. Returns how many touches were " +
        "accumulated. Use this after reading touchstate to start a fresh " +
        "counting window.",
      arguments: z.object({
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (args: { timeoutMs?: number }, ctx: MethodContext) => {
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        const t0 = performance.now();
        const r = await sendLine(ctx, "touchclear", timeoutMs);
        const failed = replyFailed(r.response);
        const outcome = !r.response ? "timeout" : (failed ? "error" : "ok");
        ctx.logger.info("touchclear: cleared {n}", {
          n: r.response?.cleared ?? 0,
        });
        const handle = await ctx.writeResource(
          "touchstate",
          "touchstate-latest",
          {
            count: 0,
            x: 0,
            y: 0,
            t: 0,
            outcome,
            observedAt: new Date().toISOString(),
            elapsedMs: Math.round(performance.now() - t0),
          },
        );
        if (!r.response) {
          throw new Error(
            `no reply to touchclear within ${timeoutMs} ms`,
          );
        }
        return { dataHandles: [handle] };
      },
    },

    waittouch: {
      description:
        "Block until the screen is touched, then return the coordinates. " +
        "The firmware polls the touch controller internally and replies the " +
        "instant a finger lands. Use for compliance acknowledgments, button " +
        "presses, or any flow where you show a screen and wait for input. " +
        "The touch is also latched into the counter.",
      arguments: z.object({
        timeoutMs: z.number().int().positive().default(30_000).describe(
          "How long to wait for a touch before giving up (default 30s)",
        ),
      }),
      execute: async (args: { timeoutMs?: number }, ctx: MethodContext) => {
        const timeoutMs = args.timeoutMs ?? 30_000;
        const t0 = performance.now();
        const r = await sendLine(
          ctx,
          `waittouch ${timeoutMs}`,
          timeoutMs + 2000,
        );
        const failed = replyFailed(r.response);
        const points = (r.response?.points as number) ?? 0;
        const timedOut = !!(r.response?.timeout);
        const outcome = !r.response
          ? "timeout"
          : (timedOut ? "timeout" : (failed ? "error" : "ok"));
        ctx.logger.info("waittouch: {outcome}, points={points}", {
          outcome,
          points,
        });
        const handle = await ctx.writeResource("touch", "touch-latest", {
          command: `waittouch ${timeoutMs}`,
          points,
          x: (r.response?.x as number) ?? 0,
          y: (r.response?.y as number) ?? 0,
          gesture: (r.response?.gesture as number) ?? undefined,
          waitMs: (r.response?.waitMs as number) ?? undefined,
          outcome,
          observedAt: new Date().toISOString(),
          elapsedMs: Math.round(performance.now() - t0),
        });
        if (!r.response) {
          throw new Error(
            `no reply to waittouch within ${timeoutMs + 2000} ms (recorded ` +
              "as touch-latest, outcome=timeout)",
          );
        }
        return { dataHandles: [handle] };
      },
    },

    simtouch: {
      description:
        "Simulate a touch at the given coordinates without physically " +
        "touching the screen. The firmware latches it exactly as a real " +
        "touch: the counter increments, touchstate sees it, and any " +
        "waittouch that is blocking will NOT see it (it polls the real " +
        "controller). Use for testing flows end-to-end.",
      arguments: z.object({
        x: z.number().int().min(0).max(319).describe("X coordinate (0-319)"),
        y: z.number().int().min(0).max(479).describe("Y coordinate (0-479)"),
        timeoutMs: z.number().int().positive().optional().describe(
          "Overrides the instance's timeoutMs for this call",
        ),
      }),
      execute: async (
        args: { x: number; y: number; timeoutMs?: number },
        ctx: MethodContext,
      ) => {
        const timeoutMs = args.timeoutMs ?? ctx.globalArgs.timeoutMs;
        const t0 = performance.now();
        const r = await sendLine(
          ctx,
          `simtouch ${args.x} ${args.y}`,
          timeoutMs,
        );
        const failed = replyFailed(r.response);
        const outcome = !r.response ? "timeout" : (failed ? "error" : "ok");
        ctx.logger.info("simtouch: ({x},{y}) {outcome}", {
          x: args.x,
          y: args.y,
          outcome,
        });
        const handle = await ctx.writeResource("touch", "touch-latest", {
          command: `simtouch ${args.x} ${args.y}`,
          points: 1,
          x: args.x,
          y: args.y,
          simulated: true,
          outcome,
          observedAt: new Date().toISOString(),
          elapsedMs: Math.round(performance.now() - t0),
        });
        if (!r.response) {
          throw new Error(
            `no reply to simtouch within ${timeoutMs} ms (recorded as ` +
              "touch-latest, outcome=timeout)",
          );
        }
        return { dataHandles: [handle] };
      },
    },
  },
};
