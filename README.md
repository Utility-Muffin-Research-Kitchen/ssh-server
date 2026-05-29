# ssh-server

Standalone SSH server app repo for UMRK. The first target is a device-first,
Dropbear-backed Jawaka app for Miniloong Pocket 1 Stock OS.

## Direction

- separate sibling repo, not Jawaka core
- Catastrophe-powered GUI
- manual start/stop from the app UI
- persistent app-owned settings and runtime state
- package as a Jawaka `Apps/<Name>.pak/` app
- current auth target: a dedicated app-managed admin account with
  root-equivalent access, without changing the stock `root` password

## Current status

The first implementation slice now exists:

- `cmd/ssh-server/main.c` provides a Catastrophe settings shell
- config persists under the app-owned state root
- the app can apply a dedicated UID 0 alias account without changing the stock
  `root` password
- the runtime can generate Dropbear host keys and start/stop Dropbear when the
  bundled binaries are present
- host keys are generated automatically on first start when missing
- the UI now shows a detected reachable `IP:Port`, while only the port remains
  editable
- username/password editors now explain `START = save` and `Y = cancel`
- Start Folder now uses Catastrophe's directory picker
- desktop and MLP1 build targets plus Jawaka pak packaging are in place
- the MLP1 package now bundles `runtime/bin/dropbear` and
  `runtime/bin/dropbearkey`

Current limitation: Dropbear's `-w` root-login block rejects all UID 0 accounts,
not just the literal `root` username. That means the current alias-account model
cannot simultaneously use UID 0 and fully disable root logins.

## Build

```sh
cd /Volumes/Storage/UMRK/ssh-server
make native
make package
make mlp
make package-mlp1
```

## Run desktop preview

```sh
cd /Volumes/Storage/UMRK/ssh-server
make run-native
```

## Stage the pak over ADB

```sh
cd /Volumes/Storage/UMRK/ssh-server
make adb-stage-pak-mlp1
```

## Planned settings

- account name
- account password
- reachable `IP:Port` display with editable TCP port
- starting folder
- automatic host-key creation on first start
- server start/stop state

## Sensible defaults

| Setting | Default |
| --- | --- |
| Account name | `sshadmin` |
| Port | `2222` |
| Starting folder | `/mnt/sdcard` |
| Host keys | `/userdata/umrk-ssh-server/hostkeys/` |
| Config root | `/userdata/umrk-ssh-server/` |
| Startup mode | manual only |

## Repo notes

- `docs/PLAN.md` is the working implementation plan.
- `docs/ARCHITECTURE_DECISIONS.md` tracks the binding repo decisions and risky
  assumptions.
- Jawaka remains responsible only for discovery and launch. This repo owns the
  SSH app runtime, config, auth integration, and packaging.
