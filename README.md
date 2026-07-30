# ssh-server

Standalone SSH server app for UMRK. It builds a Catastrophe UI and bundles a
Dropbear runtime so Jawaka can launch SSH as an app instead of carrying SSH
runtime or account logic in the launcher.

Primary target is Miniloong Pocket 1 (MLP1) Stock OS.

## Current Status

- `cmd/ssh-server/main.c` provides a Catastrophe settings UI.
- The app is a Jawaka SVC-1 service (`org.umrk.sshserver`) and controls it over
  CTL-1 instead of owning a PID file.
- The UI can run and stop the supervised service. The control client also
  implements Jawaka's restart, enable, and disable operations for coordinated
  release integration.
- MLP1 packages bundle `runtime/bin/dropbear` and `runtime/bin/dropbearkey`.
- Host keys are generated automatically on first start.
- Config, host keys, optional `authorized_keys`, and the GUI's local log live
  under the app-owned state root. Jawaka separately owns the bounded service
  log and last-exit state.
- The UI shows detected reachable `IP:Port`; only the TCP port is editable.
- Passwords are converted to salted SHA-512 hashes before persistence. Legacy
  plaintext config is migrated atomically on first supervised start. New
  passwords require at least 12 characters; a weaker legacy password is
  removed and must be replaced before password login can start.
- The UI shows authentication mode, the ED25519 host-key fingerprint, last-exit
  status, transition reason, and recent service log lines.
- Start Folder uses Catastrophe's directory picker.
- The app can apply a dedicated UID 0 alias account without changing the stock
  `root` password.
- The bundled Dropbear is patched so its daemon, connection children, and login
  shells remain in Jawaka's service process group and inherit parent-death
  protection. Each connection process is also a Linux child subreaper: login
  shells close every descriptor from 3 upward before `exec`, and the connection
  guardian kills and reaps escaped or double-forked descendants before it
  releases the generation lease. Arbitrary shell descendants therefore cannot
  pin a stale service generation or outlive a supervised connection.

Known limitation: Dropbear's `-w` root-login block rejects every UID 0 account,
not just the literal `root` username. The current alias-account model preserves
a separate password from stock `root`, but it cannot also block all other UID 0
logins through Dropbear alone.

## Build

Native preview:

```sh
make native
make run-native
make package
```

MLP1 build and package:

```sh
make mlp
make package-mlp1
make package-platform PLATFORM=mlp1
```

`package-platform PLATFORM=mlp1` is the target Leaf dispatches when staging the
app to a device.

The MLP1 package builds Dropbear 2025.88 unless the current build output already
contains the bundled binaries.

## Stage To MLP1

Preferred staging is from the sibling `Leaf` repo:

```sh
cd ../Leaf
make stage-app APP=ssh-server DEVICE=mlp1
```

Leaf packages this repo and deploys:

```text
Apps/mlp1/SSHServer.pak/
  bin/ssh-server
  launch.sh
  pak.json
  res/
  runtime/bin/dropbear
  runtime/bin/dropbearkey
```

For targeted repo-local ADB debugging:

```sh
make adb-stage-pak-mlp1
```

## Runtime Paths

`pak/launch.sh` sources the Leaf runtime env when present:

```text
$SDCARD_PATH/.system/leaf/platforms/$PLATFORM/launcher/env.sh
```

The app root is exported as `UMRK_SSH_APP_ROOT`. App state defaults to:

```text
$USERDATA_PATH/umrk-ssh-server/
```

On MLP1, if `USERDATA_PATH` is missing, the fallback is:

```text
$SDCARD_PATH/.userdata/mlp1/umrk-ssh-server/
```

Override the state root explicitly with:

```sh
UMRK_SSH_STATE_DIR=/path/to/state
```

State layout:

```text
config.ini
hostkeys/
authorized_keys       # optional; required for key-only mode
logs/ssh-server.txt   # GUI log; not service lifecycle state
```

The service is foreground-only. It has no app-owned PID file, daemon state, or
persistent `/etc` backup. Jawaka owns service lifetime, stop escalation,
service logs, and last-exit metadata.

The state root is normally on FAT. FAT has no Unix mode bits, so the persisted
SHA-512 password hash and `authorized_keys` input are readable to anyone who
can read the card and remain physically replaceable by someone who controls the
card. Hashing removes plaintext-at-rest and raises the cost of offline recovery;
it does not make a removable card a secret store. Prefer a long unique password
and treat key-only mode as protection against network password guessing, not
against physical card tampering.

Account publication is idempotent: an already-matching `/etc/passwd` and
`/etc/shadow` pair is not rewritten on boot/backoff. Changes are built and
fsynced in same-directory temporary files, renamed atomically one file at a
time, and deterministically converge on the next start if power fails between
the two renames.

## Defaults

| Setting | Default |
| --- | --- |
| Account name | `sshadmin` |
| Bind target | `0.0.0.0:2222` |
| Displayed address | detected reachable device IP plus port |
| Starting folder | `SDCARD_PATH`, then `/mnt/sdcard` on MLP1, then `/` |
| Host keys | `$USERDATA_PATH/umrk-ssh-server/hostkeys/` |
| Authentication | password enabled; key-only is selectable when `authorized_keys` exists |
| Startup mode | supervised, disabled by default |

Useful overrides:

| Variable | Purpose |
| --- | --- |
| `UMRK_SSH_STATE_DIR` | explicit app state root |
| `UMRK_SSH_APP_ROOT` | pak/app root used to find bundled runtime binaries |
| `UMRK_SSH_DROPBEAR_BIN` | override Dropbear executable |
| `UMRK_SSH_DROPBEARKEY_BIN` | override Dropbear host-key tool |
| `UMRK_SSH_DEVICE_IP` | override displayed reachable IP |
| `UMRK_SSH_PRIMARY_IFACE` | preferred interface for IP detection |

## Repo Notes

- Jawaka discovers and launches the pak; this repo owns the SSH runtime,
  account integration, config model, and packaged payload.
- `docs/PLAN.md` and `docs/ARCHITECTURE_DECISIONS.md` record design history and
  unresolved auth tradeoffs.
- The service is disabled by default. Jawaka persists explicit enable/disable
  intent and applies lifecycle policy; the app never installs a boot hook.
- Safe unmount gives interactive shells a 3-second TERM grace before group
  escalation. Game launches deliberately leave SSH running (`game: ignore`),
  so an SSH user must not edit a game's active save/state tree during play.
- The manifest declares `config.ini` and the app-state `authorized_keys` for
  uninstall revocation while retaining host keys and logs. The rootfs UID-0
  alias is outside declarative app state; a package-only removal leaves that
  inert account row in place because no SSH listener remains. A future
  transaction-aware uninstaller must explicitly remove it.
- Removal of the legacy launcher-switcher SSH hook and one-time migration of
  existing installations are coordinated Release A assembly work, not owned by
  this repo.
