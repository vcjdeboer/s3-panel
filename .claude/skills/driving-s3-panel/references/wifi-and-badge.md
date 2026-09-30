# Wi-Fi and the swamp badge (firmware s3panel 0.12)

The panel can join Wi-Fi and show a swamp-club profile by itself: logo idle,
a tap cycles profile, badges, activity, logo. Swamp still drives it over USB.
The panel fetches the profile over HTTPS itself; USB is only swamp's control
channel. Once set up, a USB charger is enough.

## States

| State | Meaning | Screen |
| --- | --- | --- |
| `setup` | no Wi-Fi or profile stored, after `forget`, or a 5 s hold on the logo | setup QR codes |
| `connecting` | configured, not on Wi-Fi, no cached profile | logo + "connecting..." |
| `badge` | on Wi-Fi, or has a cached profile | logo; taps cycle the profile screens |
| `host` | a host command drew on or is waiting on the screen | whatever the host drew |

`host` ends on `idle`, or 5 minutes after the last host command. `wifi`
reports the state.

On the badge, taps cycle logo, profile, badges, activity; a page stays until
the next tap. Activity is listed oldest to newest (newest at the bottom), and
rows that arrive while the page is open flash cyan for 2 s; its ages refresh
every 10 s. After 5 minutes
without a touch the backlight dims to 15 %, after 10 minutes it goes dark, on
any page; only a tap wakes it (the first tap just wakes), never new activity.

## Set up

- **From a phone (a shipped panel):** power it. Scan the first QR code to join
  `swamp-xxxx` (a fresh password each time, shown only on the panel). The
  setup page may open by itself; iOS often does not raise it after a QR join,
  so tap the network in Wi-Fi settings, scan the second QR code, or open
  `http://192.168.4.1/`. Pick the network from the list (its exact spelling),
  type its password and the swamp username. The panel joins and checks the
  username before it saves anything.
- **From swamp (over USB):** keep the credentials in a vault and pass them as
  vault expressions in a workflow step, never as literals (a one-off
  `--input` is not evaluated):
  ```yaml
  - task:
      type: model_method
      modelIdOrName: <panel>
      methodName: configure
      inputs:
        ssid: ${{ vault.get("<vault>", "wifi-ssid") }}
        password: ${{ vault.get("<vault>", "wifi-password") }}
  ```
  then `swamp model method run <panel> profile --input username=<name>`.
- **Factory reset:** `swamp model method run <panel> forget --input confirm=true`.

The radio is 2.4 GHz only. Names are compared byte for byte: a curly `’`
typed on a Mac or phone is not the router's straight `'`. Enter an SSID
with an apostrophe as `printf 'Name\047s Network' | swamp vault put <vault> wifi-ssid`.

## Methods

| Method | Layer | Records |
| --- | --- | --- |
| `wifi` | base | `wifi-latest`: state, connected, ip, rssi, mac |
| `configure` | base | `config-latest`: key names stored, joined, error. Never values |
| `forget` | base | `config-latest` with `forgotten`; needs `confirm=true` |
| `profile` | panel | `profile-latest`: username, points, rank, tier, badgeCount, activityCount, fetchedAt, ok, error |
| `idle` | panel | a `draw` record; hands the screen back |

## Line commands

| Command | Reply |
| --- | --- |
| `wifi status` | `{"ok":true,"state":"badge","connected":true,"ip":"...","rssi":-68,"mac":"..."}` |
| `wifi scan` | `{"ok":true,"count":N,"networks":[["name",channel,rssi],...]}` (debug aid) |
| `config set <json>` | fields `ssid`, `pass`, `profile`, `api`, each optional, validated before saving: `{"ok":true,"stored":["ssid","pass"],"joined":true}` or `{"ok":false,"error":"join: network not found"}` (also `join: wrong password`, `join: refused by the router`, `not found`, `unreachable: ...`, `invalid`) |
| `config show` | `{"ok":true,"ssidSet":true,"passSet":true,"profile":"...","api":"..."}` |
| `config forget` | `{"ok":true,"forgotten":true}`, then the panel enters setup |
| `profile status` | `{"ok":true,"fetchedAt":<epoch s>,"error":null,"username":"...","points":N,"rank":"...","tier":N,"badges":N,"activity":N,"cached":true}` |
| `profile refresh` | fetches now (~11 s), then replies as `profile status` |
| `setup status` | the setup page's phase, last message and last 8 requests (debug aid) |
| `idle` | `{"ok":true,"state":"badge"}` |

`status` also reports `state` and `heap`. Replies never contain a password or
an SSID value. Scheduled fetches run in the background and do not block USB
commands: the whole profile every 15 min (or on a tap when it is over 5 min
old), and the activity log alone every 10 s (not cached, so no flash wear);
`profile refresh` and `config set` with a profile wait for the answer.

## Symptoms

| Symptom | Cause | Fix |
| --- | --- | --- |
| Stuck on "connecting..." | stored network out of range, 5 GHz only, or password changed | hold 5 s for setup, or run `configure` |
| `configure` fails with `join: network not found` | name mismatch (case, apostrophe style) or the network is not on 2.4 GHz near the panel | `send line=wifi scan` to see what the panel hears |
| `configure` fails with `join: wrong password` | the password; nothing was saved | fix the vault value, rerun |
| `profile` fails with `not found` | no such swamp-club username | check the spelling; the panel shows "user not found" with no stale numbers |
| Pink "offline - 2h ago" bar | no Wi-Fi or swamp-club unreachable; showing the cache | it refreshes by itself once reachable |
| Pink "data error" bar | swamp-club changed its JSON | update the firmware's parser |
| `wifi` says `unknown command` | firmware older than 0.12 | flash `firmware/s3panel` |

The data comes from swamp-club's public JSON (`/api/v1/users/<name>` and its
`/combat-log`), which its own pages use. It is not a documented API.
