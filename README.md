# ssh-server

Standalone SSH server app for UMRK. It builds a Catastrophe UI and bundles a
Dropbear runtime so Jawaka can launch SSH as an app instead of carrying SSH
runtime or account logic in the launcher.

Primary target is Miniloong Pocket 1 (MLP1) Stock OS.

## Current Status

- `cmd/ssh-server/main.c` provides a Catastrophe settings UI.
- The app can start/stop Dropbear manually from the UI.
- MLP1 packages bundle `runtime/bin/dropbear` and `runtime/bin/dropbearkey`.
- Host keys are generated automatically on first start.
- Config, host keys, logs, backups, and run state live under the app-owned state
  root.
- The UI shows detected reachable `IP:Port`; only the TCP port is editable.
- Username/password editors use Catastrophe keyboard prompts.
- Start Folder uses Catastrophe's directory picker.
- The app can apply a dedicated UID 0 alias account without changing the stock
  `root` password.

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
logs/ssh-server.txt
run/dropbear.pid
backups/
```

## Defaults

| Setting | Default |
| --- | --- |
| Account name | `sshadmin` |
| Bind target | `0.0.0.0:2222` |
| Displayed address | detected reachable device IP plus port |
| Starting folder | `SDCARD_PATH`, then `/mnt/sdcard` on MLP1, then `/` |
| Host keys | `$USERDATA_PATH/umrk-ssh-server/hostkeys/` |
| Startup mode | manual only |

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
- Version 1 is manual start/stop only. Boot-time autostart is out of scope for
  the current app.
