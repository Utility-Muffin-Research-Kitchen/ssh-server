# ssh-server plan

This plan covers the first UMRK SSH server app: a Dropbear-based, Catastrophe
GUI for Miniloong Pocket 1 Stock OS, packaged as a Jawaka app.

## 1. Mission

Build a simple end-user SSH server app that:

- launches from Jawaka
- presents a Catastrophe GUI for configuration and control
- persists settings across launches
- starts and stops Dropbear on demand
- exposes a friendly username/password login flow
- avoids changing the stock `root` password

## 2. Device facts already confirmed over ADB

- the launcher/app context runs as `root`
- `/etc/passwd` exposes `root`
- `/etc/shadow` exists and root already has a password hash
- `adduser` and `passwd` exist on-device
- `sudo` does not exist
- Dropbear is not installed by default
- a second `uid=0` alias account works when written directly to
  `/etc/passwd` and `/etc/shadow`
- `su - <alias>` lands in the alias home directory, so the configured start
  folder can map cleanly to the alias account home
- Dropbear's root-login block checks `pw_uid == 0`, so it cannot disable the
  stock `root` account while still allowing a UID 0 alias account

These facts make account management possible, but they also make the auth model
the highest-risk part of the plan.

## 3. Working defaults

- account name: `sshadmin`
- port: `2222`
- starting folder: `/mnt/sdcard`
- config root: `/userdata/umrk-ssh-server/`
- startup mode: manual only

## 4. Proposed persisted state

```text
/userdata/umrk-ssh-server/
  config.ini
  hostkeys/
  logs/
  run/
```

Expected config fields:

- `username`
- `bind_address` (stored as the backend bind target; currently wildcard bind
 plus the configured port)
- `start_dir`
- `last_applied_username`

The password itself should only be persisted if the final account-management
approach truly requires it. Prefer writing the system account password and not
storing the raw secret long-term if the device flow allows that cleanly.

## 5. Milestones

### Phase 0 - auth spike

Status: **partially completed**

Validate the least-bad way to provide a dedicated admin login without changing
the stock `root` password.

Tasks:

1. Test creating a dedicated app-managed account on-device.
2. Determine whether the account must be a UID 0 alias or whether another
   root-equivalent mechanism is practical on this firmware.
3. Confirm home directory, shell, cleanup, and password update behavior.
4. Write down the exact risk profile before UI work assumes the model is safe.

Exit condition:

- one concrete, device-proven account model is chosen

Current outcome:

- the repo implementation now targets a dedicated UID 0 alias account
- the account-apply path is implemented in C
- the "disable root but keep UID 0 alias" goal remains unresolved because of
  Dropbear's UID-based root-login check

### Phase 1 - backend bring-up

Status: **started**

Bring a Dropbear build into this repo and define the runtime bundle layout.

Tasks:

1. Add the chosen build inputs for Miniloong Pocket 1.
2. Produce Dropbear and host-key generation binaries needed by the app.
3. Define launch arguments, pidfile path, log path, and host-key path.
4. Verify manual start/stop from an app-owned wrapper script.

Exit condition:

- Dropbear can be started and stopped on-device from a controlled wrapper

Current outcome:

- `build/mlp1/runtime/bin/` is now populated by a pinned Dropbear 2025.88 build
- `SSHServer.pak` now bundles `dropbear` and `dropbearkey`
- direct on-device smoke confirmed host-key generation and daemon startup work

### Phase 2 - Catastrophe app shell

Status: **started**

Build the first real GUI.

Tasks:

1. Show server status, account name, port, and start directory.
2. Add editable settings rows and validation.
3. Persist config under `/userdata/umrk-ssh-server/`.
4. Surface clear status and failure messages.

Exit condition:

- the GUI can edit and persist the initial settings set

Current outcome:

- the GUI now exposes a compact settings screen with:
  - Server On/Off toggle
  - detected reachable `IP:Port` display
  - Username
  - Password
  - Start Folder
- host keys are auto-generated on first start rather than managed by a separate
  action row
- only the port is user-editable; the displayed IP is detected from live network
  interfaces
- username/password keyboard prompts now explain `START = save` and
  `Y = cancel`
- Start Folder selection now uses Catastrophe's directory picker

### Phase 3 - account management

Status: **started**

Wire the chosen auth model into the GUI flow.

Tasks:

1. Create or update the dedicated admin account.
2. Apply password changes from the GUI.
3. Keep the stock `root` password untouched.
4. Document recovery/cleanup steps.

Exit condition:

- the app-owned account can authenticate successfully over SSH

### Phase 4 - Jawaka packaging

Turn the app into a launchable Jawaka pak.

Tasks:

1. Define `pak.json` metadata and icon/art inputs.
2. Add `launch.sh` and payload layout.
3. Decide how bundled binaries, config bootstrap, and logs are staged.
4. Verify the app launches cleanly from Jawaka.

Exit condition:

- the app is launchable through the normal Jawaka `Apps/` flow

### Phase 5 - hardening

Reduce surprises before real usage.

Tasks:

1. Improve validation and error messaging.
2. Add safe defaults for bind address, log handling, and stale pid cleanup.
3. Document uninstall and account cleanup behavior.
4. Decide whether SFTP/SCP support is explicitly in or out for v1.

## 6. Open technical questions

1. What is the cleanest root-equivalent account model on this firmware?
2. Should the account name stay fully editable, or should the first version
   keep `sshadmin` fixed?
3. Does the chosen start-directory behavior need shell wrapping to guarantee the
   initial working directory for login sessions?
4. Is SFTP required in v1, or is shell-only SSH sufficient for the first slice?
5. If disabling stock `root` login is mandatory, should the repo pivot to a
   non-UID-0 account plus a privileged helper instead of the alias-account
   model?

## 7. Non-goals for v1

- boot-time autostart
- remote package management
- sharing Jawaka's SQLite DB for app settings
- replacing Dropbear with a custom SSH daemon unless the auth spike forces it
