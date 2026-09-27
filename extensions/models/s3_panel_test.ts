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

import { assert, assertEquals, assertThrows } from "jsr:@std/assert@1";
import { assertDrawable, model } from "./s3_panel.ts";

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
    "detect",
    "flash",
    "hold",
    "ping",
    "read",
    "release",
    "send",
    "status",
    "write",
  ];
  // Added here: drawing, events, touch, touch screens and approvals.
  const own = [
    "approve",
    "backlight",
    "clear",
    "drain",
    "fill",
    "listen",
    "logo",
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
