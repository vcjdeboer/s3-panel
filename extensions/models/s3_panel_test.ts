/**
 * Structural tests for `@vcjdeboer/s3-panel`.
 *
 * No board and no swamp needed. The interesting one is `assertDrawable`: the
 * firmware splits lines on `|` and reads one command per line, so text
 * containing either would silently become extra lines or extra commands. That
 * has to be rejected before anything reaches the wire, and it cannot be
 * exercised through a shell (the separator is awkward to quote), so it is
 * pinned here.
 *
 * Run: `~/.swamp/deno/deno test -A extensions/models/s3_panel_test.ts`
 *
 * @module
 */

import {
  assert,
  assertEquals,
  assertRejects,
  assertThrows,
} from "jsr:@std/assert@1";
import {
  assertDrawable,
  findPendingApproval,
  model,
  parseSwampJson,
  type PendingApproval,
  profileFields,
  resolveApproval,
  type SwampRunner,
  swampBinary,
  USERNAME,
  waitForTap,
} from "./s3_panel.ts";

Deno.test("the model type and version are well formed", () => {
  assertEquals(model.type, "@vcjdeboer/s3-panel");
  assert(
    /^\d{4}\.\d{2}\.\d{2}\.\d+$/.test(model.version),
    `version ${model.version} is not CalVer YYYY.MM.DD.MICRO`,
  );
});

Deno.test("the panel inherits the whole base and adds exactly the screen methods", () => {
  const names = Object.keys(model.methods).sort();
  // Inherited from the shared base, unchanged.
  const base = [
    "configure",
    "detect",
    "flash",
    "forget",
    "hold",
    "ping",
    "read",
    "release",
    "send",
    "status",
    "wifi",
    "write",
  ];
  // Added here: drawing, events, touch, touch screens and approvals.
  const own = [
    "approve",
    "backlight",
    "clear",
    "drain",
    "fill",
    "idle",
    "listen",
    "logo",
    "profile",
    "screen",
    "simtouch",
    "text",
    "touch",
    "touchclear",
    "touchstate",
    "waittouch",
    "zonewait",
  ];
  assertEquals(names, [...base, ...own].sort(), "method set changed");
});

Deno.test("the panel adds its own draw spec on top of the base specs", () => {
  const specs = Object.keys(model.resources);
  assert(specs.includes("draw"), "no draw spec");
  for (const base of ["devices", "state", "exchange", "sent", "capture", "holder"]) {
    assert(specs.includes(base), `missing inherited spec ${base}`);
  }
});

Deno.test("every write targets a declared spec with a <spec>-latest record name", () => {
  const src = Deno.readTextFileSync(new URL("./s3_panel.ts", import.meta.url));
  const writes = [...src.matchAll(/writeResource\(\s*"(\w+)",\s*"([\w-]+)"/g)];
  assert(writes.length > 0, "no writeResource calls found");
  // All text/fill/clear/backlight/logo writes funnel through one `draw` helper.
  assertEquals(writes.filter(([, spec]) => spec === "draw").length, 1);
  const specs = Object.keys(model.resources);
  for (const [, spec, instance] of writes) {
    assert(specs.includes(spec), `writes to undeclared spec ${spec}`);
    assertEquals(instance, `${spec}-latest`);
  }
});

Deno.test("a line containing the separator is refused before it reaches the wire", () => {
  const err = assertThrows(() => assertDrawable(["fine", "not|fine"]));
  assert(
    (err as Error).message.includes("line separator"),
    "the error should explain why the separator is refused",
  );
});

Deno.test("a line containing a newline or carriage return is refused", () => {
  assertThrows(() => assertDrawable(["a\nb"]));
  assertThrows(() => assertDrawable(["a\rb"]));
});

Deno.test("no lines at all is refused, pointing at clear instead", () => {
  const err = assertThrows(() => assertDrawable([]));
  assert((err as Error).message.includes("clear"));
});

Deno.test("ordinary text is allowed, including spaces and punctuation", () => {
  assertDrawable(["S3 PANEL", "ready: 3 of 3", "temp 23.4 C"]);
  // An empty line is a legitimate blank row, not an error.
  assertDrawable(["top", "", "bottom"]);
});

// ── workflow gate ────────────────────────────────────────────────────────────

const WAITING: PendingApproval[] = [
  { workflowName: "deploy", runId: "run-a", stepName: "gate", prompt: "Ship it?" },
  { workflowName: "deploy", runId: "run-b", stepName: "other", prompt: "x" },
  { workflowName: "backup", runId: "run-c", stepName: "gate", prompt: "y" },
];

/** A fake swamp CLI: records every call, answers approvals from a list. */
function fakeSwamp(approvals: PendingApproval[], code = 0) {
  const calls: string[][] = [];
  const run: SwampRunner = (args) => {
    calls.push(args);
    const stdout = args[1] === "approvals"
      ? `[INF] noise\n${JSON.stringify({ approvals })}\n`
      : JSON.stringify({ ok: code === 0 });
    return Promise.resolve({ code, stdout, stderr: code ? "refused" : "" });
  };
  return { run, calls };
}

Deno.test("the waiting run is found by workflow and step", async () => {
  const { run } = fakeSwamp(WAITING);
  const p = await findPendingApproval(run, "deploy", "gate");
  assertEquals(p.runId, "run-a");
  assertEquals(p.prompt, "Ship it?");
});

Deno.test("no waiting run is refused before anything is drawn", async () => {
  const { run } = fakeSwamp(WAITING);
  await assertRejects(() => findPendingApproval(run, "deploy", "nope"));
});

Deno.test("two waiting runs are refused unless a run id picks one", async () => {
  const two = [...WAITING, { ...WAITING[0], runId: "run-z" }];
  const { run } = fakeSwamp(two);
  const err = await assertRejects(() => findPendingApproval(run, "deploy", "gate"));
  assert((err as Error).message.includes("run-z"), "should list the candidates");
  const p = await findPendingApproval(run, "deploy", "gate", "run-z");
  assertEquals(p.runId, "run-z");
});

Deno.test("a run id that is not waiting at that step is refused", async () => {
  const { run } = fakeSwamp(WAITING);
  await assertRejects(() => findPendingApproval(run, "deploy", "gate", "run-c"));
});

Deno.test("a tapped decision becomes exactly that swamp verb for that run", async () => {
  for (const [decision, verb] of [["approved", "approve"], ["rejected", "reject"]] as const) {
    const { run, calls } = fakeSwamp(WAITING);
    const err = await resolveApproval(run, WAITING[0], decision, "why");
    assertEquals(err, undefined);
    assertEquals(calls[0].slice(0, 6), ["workflow", verb, "deploy", "gate", "--run", "run-a"]);
    assertEquals(calls[0].slice(6), ["--reason", "why", "--json"]);
  }
});

Deno.test("swamp refusing the decision is reported, not swallowed", async () => {
  const { run } = fakeSwamp(WAITING, 1);
  assertEquals(await resolveApproval(run, WAITING[0], "approved", "why"), "refused");
});

Deno.test("swamp JSON is found among log lines", () => {
  assertEquals(parseSwampJson('[INF] x\n{"a":1}\n').a, 1);
  assertThrows(() => parseSwampJson("[INF] nothing"));
});

Deno.test("an explicit swamp path wins; plain deno falls back to PATH", () => {
  assertEquals(swampBinary("/opt/swamp"), "/opt/swamp");
  assertEquals(swampBinary(), "swamp");
});

// ── waiting for a tap ────────────────────────────────────────────────────────

/** A fake board: answers each `screen wait` from a script, records the lines. */
function fakeBoard(replies: (Record<string, unknown> | null)[]) {
  const lines: string[] = [];
  const send = (line: string, _timeoutMs: number) => {
    lines.push(line);
    const r = replies.shift();
    return Promise.resolve(r === undefined ? { ok: true, timeout: true } : r);
  };
  return { send, lines };
}

Deno.test("a tap ends the wait with the zone that was hit", async () => {
  const { send } = fakeBoard([{ ok: true, timeout: true }, { ok: true, id: "go", x: 5, y: 9 }]);
  assertEquals(await waitForTap(send, 60_000), { ended: "tap", id: "go", x: 5, y: 9 });
});

Deno.test("the board is never asked to wait longer than one slice", async () => {
  const { send, lines } = fakeBoard([]);
  const w = await waitForTap(send, 50, undefined, 20);
  assertEquals(w.ended, "timeout");
  for (const l of lines) {
    assert(Number(l.split(" ")[2]) <= 20, `${l} exceeds the slice`);
  }
});

Deno.test("swamp model cancel (the abort signal) ends the wait between slices", async () => {
  const ac = new AbortController();
  let calls = 0;
  const send = () => {
    if (++calls === 2) ac.abort();
    return Promise.resolve({ ok: true, timeout: true } as Record<string, unknown>);
  };
  const w = await waitForTap(send, 60_000, ac.signal);
  assertEquals(w.ended, "cancelled");
  assertEquals(calls, 2);
});

Deno.test("serial input on the board aborts the wait", async () => {
  const { send } = fakeBoard([{ ok: true, aborted: true }]);
  assertEquals((await waitForTap(send, 60_000)).ended, "aborted");
});

Deno.test("a lost link is reported, not waited out", async () => {
  const { send } = fakeBoard([null]);
  assertEquals((await waitForTap(send, 60_000)).ended, "no-reply");
});

Deno.test("usernames are swamp-club's characters only", () => {
  assert(USERNAME.test("example"));
  assert(USERNAME.test("ex.am_ple-1"));
  assert(!USERNAME.test(""));
  assert(!USERNAME.test("has space"));
  assert(!USERNAME.test("a".repeat(40)));
  assert(!USERNAME.test('x"y'));
});

Deno.test("profileFields converts epoch seconds and defaults missing fields", () => {
  const f = profileFields({
    ok: true,
    fetchedAt: 1790766990,
    error: null,
    username: "example",
    points: 1234567,
    rank: "Bog Keeper",
    tier: 12,
    badges: 19,
    activity: 8,
  });
  assertEquals(f, {
    username: "example",
    points: 1234567,
    rank: "Bog Keeper",
    tier: 12,
    badgeCount: 19,
    activityCount: 8,
    fetchedAt: "2026-09-30T11:16:30.000Z",
    ok: true,
    error: null,
  });
  const failed = profileFields({
    ok: false,
    fetchedAt: null,
    error: "not found",
  });
  assertEquals(failed.fetchedAt, null);
  assertEquals(failed.error, "not found");
  assertEquals(profileFields(null).ok, false);
});
