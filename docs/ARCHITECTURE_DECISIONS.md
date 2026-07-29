# ssh-server architecture decisions

## 1. Repo boundary

`ssh-server` is a standalone sibling repo in UMRK. Jawaka should discover and
launch it as an app, but Jawaka should not own the SSH server runtime, account
logic, or config model.

## 2. Product target

The first target is Miniloong Pocket 1 Stock OS. This repo is device-first, not
Mac-first, though desktop support can be added later for faster UI iteration.

## 3. Backend

The initial SSH backend is Dropbear, not OpenSSH and not a custom server. The
main reason is size and embedded fit.

## 4. GUI

The app UI should be built with Catastrophe. The GUI owns status display,
settings editing, validation, and start/stop control.

The current UX direction is:

- show a detected reachable `IP:Port` on the main screen
- keep only the port user-editable; the backend still binds safely
- auto-generate host keys on first start instead of exposing a separate key
  generation action
- use explicit keyboard help for username/password and a directory picker for
  Start Folder

## 5. Runtime model

The SSH server is a Jawaka SVC-1 foreground service. The GUI uses CTL-1 for
status, run, stop, and logs; its control client also implements restart,
enable, and disable for release integration. The app does not keep a PID file
or install a boot-time hook.

The service requires Jawaka's generation lease on file descriptor 3 and arms
parent-death protection. The bundled Dropbear patch keeps the daemon,
connection children, and login shells in the supervisor-owned process group so
stop escalation and storage lifecycle barriers cover the complete tree.

## 6. Persistence

App state should live in an app-owned directory under:

```text
$USERDATA_PATH/umrk-ssh-server/
```

This directory holds config, generated host keys, the optional
`authorized_keys` input, and the GUI's local log. Jawaka separately owns
bounded service logs and last-exit state. Passwords are persisted only as
salted SHA-512 hashes. Legacy plaintext config is accepted solely for atomic
one-time migration.

## 7. Default UX contract

The initial defaults are:

- account name: `sshadmin`
- port: `2222`
- starting folder: `/mnt/sdcard`
- startup mode: supervised and disabled by default

## 8. Current auth direction

The current design direction is to offer username/password login without
changing the stock `root` password. The most likely implementation is a
dedicated app-managed admin account with root-equivalent access.

This has now been partially proven on-device:

- adding a second `/etc/passwd` + `/etc/shadow` entry with `uid=0` works
- `su - <alias>` lands in the configured home directory and reports `uid=0`

But the first runtime spike also exposed an important Dropbear limit:

- `dropbear -w` rejects any account whose `pw_uid == 0`

So the current account model can preserve a separate password from stock `root`,
but it cannot also guarantee that all UID 0 logins except the alias are blocked
through Dropbear alone.

## 9. Jawaka integration contract

The deliverable from this repo should be a Jawaka-launchable pak following the
platform-guarded `Apps/<platform>/<Name>.pak/` convention, with this repo owning
the packaged payload layout and launch wrapper. Leaf owns staging the pak into
the correct platform directory.

The manifest service ID is `org.umrk.sshserver`. Its policy ignores game
launches, stops on storage change, and is disabled by default. Jawaka persists
user intent and delays restart until storage is mounted and rescanned.
