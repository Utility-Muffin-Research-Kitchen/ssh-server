# ssh-server

This repo is planned as a standalone SSH server app for UMRK.

## Direction

The first implementation should be a **Dropbear-based, Catastrophe-powered,
device-first** app for Miniloong Pocket 1 Stock OS.

## Role in the workspace

- separate repo
- launchable from Jawaka
- app-focused, not launcher-focused

## Why separate

Keeping SSH server logic in its own repo preserves Jawaka's focus on launcher
state, discovery, and app launch orchestration.

## Initial bias

Prefer the smallest path that gets a useful SSH server app running on-device,
but treat the account model as the first technical spike because Stock OS does
not ship an SSH server and the desired username/password UX is more invasive
than a key-only design.
