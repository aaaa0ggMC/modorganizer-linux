# mo-linux

A Linux-native Mod Organizer 2 style launcher CLI for Skyrim Special Edition: symlink-farm "virtual" game directory, Proton launch, MO2 on-disk formats, FOMOD, Nexus Mods (search, download, collections) — designed to be driven by people, GUIs, or AI agents.

- `docs/GUIDE.md` — 操作指南（中文）：安装、日常使用、集合、在虚拟目录里运行工具（COW）、降级、排错
- `docs/AGENT.md` — how an agent sets up and maintains a game (start here for automation)
- `docs/CLI.md` — the full CLI contract (commands, JSON data, exit codes)
- `HANDBOOK.md` — architecture, decisions, verification status, risks

Quick start: `mo-linux instance init` → `mo-linux doctor` → `mo-linux skse install` → `mo-linux collection install SLUG` → `mo-linux run --skse`. Run `mo-linux next` any time to be told what to do.

All of these documents are compiled into the binary: `mo-linux docs` lists them, `mo-linux docs guide` prints one in full.
