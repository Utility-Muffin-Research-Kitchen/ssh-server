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
stop escalation and storage lifecycle barriers cover the complete first-party
tree. Each managed connection process is a Linux child subreaper and retains
descriptor 3. The login child re-arms parent-death protection after its
credential change and closes every descriptor from 3 upward before `exec`.
Before releasing the lease, the connection guardian kills and reaps any escaped
or double-forked descendants that reparent to it. Interactive programs therefore
cannot inherit and pin the service generation or outlive the connection.

## 6. Persistence

App state should live in an app-owned directory under:

```text
$USERDATA_PATH/umrk-ssh-server/
```

This directory holds config, generated host keys, the optional
`authorized_keys` input, and the GUI's local log. Jawaka separately owns
bounded service logs and last-exit state. Passwords are persisted only as
salted SHA-512 hashes. Legacy plaintext config is accepted solely for atomic
one-time migration. New passwords have a 12-character minimum; a weaker legacy
password is erased and must be reset before password authentication can start.

MLP1 state lives on removable FAT, where requested `0600`/`0700` modes cannot
provide confidentiality. Mode-change failures that mean "filesystem has no
Unix mode bits" are tolerated, but config and `authorized_keys` publication is
still no-follow, same-directory temporary + fsync + rename. Anyone with the
card can read the password hash or replace authorized keys. This design removes
plaintext-at-rest and protects against network guessing; physical possession
of the card remains a credential-compromise boundary.

The UID-0 alias is published idempotently. Matching passwd/shadow rows are a
no-op; changes use complete, fsynced `/etc/*.umrk.XXXXXX` files and atomic
per-file renames, with shadow first. A power loss between the two valid renames
is repaired on the next supervised start without a truncating rollback copy.

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
user intent and delays restart until storage is mounted and rescanned. The
3-second stop grace is intentional for interactive sessions; games leave SSH
running, so concurrent edits to active game data remain a user responsibility.

The manifest revokes `config.ini` and app-state `authorized_keys` on a future
TXN-1 uninstall and retains host keys/logs. `/etc/passwd` and `/etc/shadow` are
outside that declarative root. Removing only the package leaves an inert alias
with no listener; transaction-aware uninstall work must remove it explicitly.
