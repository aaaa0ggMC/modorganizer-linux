# mo-linux

A Linux-native Mod Organizer 2 style launcher CLI for Skyrim Special Edition: symlink-farm "virtual" game directory, Proton launch, MO2 on-disk formats, FOMOD, Nexus Mods (search, download, collections) — designed to be driven by people, GUIs, or AI agents.

- `docs/AGENT.md` — how an agent sets up and maintains a game (start here for automation)
- `docs/CLI.md` — the full CLI contract (commands, JSON data, exit codes)
- `HANDBOOK.md` — architecture, decisions, verification status, risks

Quick start: `mo-linux instance init` → `mo-linux doctor` → `mo-linux skse install` → `mo-linux collection install SLUG` → `mo-linux run --skse`. Run `mo-linux next` any time to be told what to do.
