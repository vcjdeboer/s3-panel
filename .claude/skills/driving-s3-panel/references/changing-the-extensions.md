# Firmware, and changing the extensions

## Firmware

- Sketch: `firmware/s3panel/s3panel.ino` in github.com/vcjdeboer/s3-panel.
  It is not in the installed extension (packages may only carry
  `.ts/.json/.md/.yaml/.yml/.txt` files): clone the repo and flash from there.
- Needs `arduino-cli` with core `esp32:esp32@3.3.12`,
  "GFX Library for Arduino" **1.6.8** (1.5.x does not compile on core 3.3.x) and
  ArduinoJson.
- The model's `fqbn` global argument defaults to this board:
  `esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB`.
  `CDCOnBoot=cdc` puts the console on native USB; `PSRAM=opi` is needed for the
  320x480 canvas. Override it only for another board variant.
- Flash through the model, then check:
  ```bash
  swamp model method run panel flash --input sketchPath=<sketch dir>
  swamp model method run panel ping
  ```
- On this board, initialise the display before touch (one AXS15231B chip), and
  use the `axs15231b_320480_type1` init sequence.
- Bump `#define FW` when changing the firmware so `ping` shows what runs.

## What lives where

| On the host (`extensions/models/s3_panel.ts`) | In the firmware (`s3panel.ino`) |
| --- | --- |
| The approve screen: layout, texts, button colours, the decision screen | The tap banner (APPROVED / REJECTED), colour names, the logo, touch handling |
| Which run is resolved and how | The line protocol and every reply |

A host change needs publish and pull; no reflash. A firmware change needs a
`FW` bump, a commit to the repo and a reflash. Flash from the
repo commit that published the installed version when the host code relies
on new firmware commands.

## Where code lives

| Package | Model type | Source repo |
| --- | --- | --- |
| `@vcjdeboer/esp32-s3` | `@vcjdeboer/s3-device` | github.com/vcjdeboer/esp32-s3 |
| `@vcjdeboer/s3-panel` | `@vcjdeboer/s3-panel` | github.com/vcjdeboer/s3-panel |

- A model type cannot import another extension's code at runtime, so the shared
  base `extensions/models/_lib/` is vendored into both packages. A fix there
  goes into both repos, and the two copies should be diffed afterwards.
- `.swamp/pulled-extensions/` is swamp's installed copy. Do not edit it.

## Changing and publishing

1. Clone the source repo and change it there.
2. Try it before publishing: in a swamp repo,
   `swamp extension source add <path-to-clone>` loads the type from the clone;
   `swamp extension source rm <path>` undoes it. Check which version is active
   with `swamp model type describe @vcjdeboer/s3-panel --json`.
3. `swamp extension version --manifest manifest.yaml --json` gives
   `nextVersion`. Set it in `manifest.yaml` **and** `model.version`, and add an
   `upgrades` entry for it (`upgradeAttributes: (old) => old` when global
   arguments are unchanged; a real transform when they change).
4. `swamp extension fmt manifest.yaml`, the unit tests
   (`~/.swamp/deno/deno test -A extensions/models/*_test.ts`, swamp's bundled deno), and
   `swamp extension quality manifest.yaml`.
5. `swamp extension push manifest.yaml --dry-run --json` prints the path for
   the adversarial review report and a skeleton. Review, write the report there,
   re-run the dry run.
6. Commit and push the repo, then `swamp extension push manifest.yaml --yes`.
   Publishing is public.
7. In each consuming repo: `swamp extension pull <package> --yes` and commit
   `extensions/models/upstream_extensions.json`. The next method run on each
   instance rewrites only its `typeVersion:` line; commit that too.
