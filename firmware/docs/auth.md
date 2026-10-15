# Authentication

PrintDrop's web UI and JSON API can be protected with HTTP Basic auth. Credentials live in NVS (`printdrop` namespace `auth_user` / `auth_hash` as SHA-256 hex), never in the source tree.

## Build-time credentials

There is no default password. The `photoframe` build runs
`tools/build_secrets.py`, which reads two environment variables and fails the
build if either is missing:

- `PHOTOFRAME_WEB_PASSWORD`: the web UI and OTA password for user `admin`. Only
  its SHA-256 is compiled in, as `PRINTDROP_AUTH_PASS_HASH`.
- `PHOTOFRAME_SETUP_PASSWORD`: the WPA2 password of the `PhotoFrame-Setup`
  network, compiled in as `AP_PASSWORD`.

`src/printdrop/config.h` stops the build with `#error` if either define is
missing, so a board cannot ship with a publicly known password.
`PRINTDROP_AUTH_REQUIRED=0` still turns authentication off entirely.

NVS keeps the hash it was seeded from as `auth_seed`. While the stored hash
still equals that seed, a reflash with a different build password replaces
it, so rotating `PHOTOFRAME_WEB_PASSWORD` takes effect on the next flash or OTA
update. A password changed on the device (`/api/auth/set`) differs from its
seed and survives reflashes until a factory reset. A stored hash with no seed
record, such as upstream PrintDrop's default, is replaced.

## Setting at runtime

**Serial** (`COM5` 115200):
```
auth <user> <pass>      # set and reboot
passwd <newPass>        # keep user, change pass
clear-auth              # reset to build-flag defaults
status                  # shows auth user / on|off
```

**Web UI** `Settings → Web UI Login`: user + new pass + confirm → `POST /api/auth/set` (requires current auth).

**Button** long-press 5 s → factory reset clears Wi-Fi *and* auth (`src/printdrop/button.cpp:44`, `main.cpp:140`).

**API** `GET /api/auth/status` is public; all mutations (`/api/list`, `/api/delete`, `/api/upload`, `/api/wifi`, `/api/ota`…) answer 401 without `WWW-Authenticate` (the photo frame page sends the `Authorization` header itself, and the header made browsers show their own login box on top of it) and check `Authorization: Basic <b64>` via `src/printdrop/auth.cpp:44`.

## Disabling

For a trusted LAN: `-D PRINTDROP_AUTH_REQUIRED=0` or via NVS key `auth_required`. The header is still accepted but not required. Not recommended on shared networks.
