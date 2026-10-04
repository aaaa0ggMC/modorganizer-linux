# mo-linux for agents

mo-linux is built so that an AI agent can set up and maintain a modded Skyrim SE on Linux end to end, with a human only where a human is genuinely needed. This page is the contract. Everything is a plain CLI call with `--json`; there is no hidden state outside the instance directory.

## The five ideas

1. **Self-describing.** `mo-linux -j schema` lists every command, option, positional, **effects** (what it touches), **needs** (Nexus key, host library, instance), whether it needs **confirmation**, whether it is idempotent, plus all error codes with default hints. Read it first; do not guess options.
2. **One envelope, stable codes.** With `-j`, stdout is exactly one JSON object `{schema_version, ok, command, data, warnings[], errors[]}`; logs go to stderr (`-q` silences them). Every error has a stable `code`, a `message`, an optional `path`, and a `hint` (what to do next).
3. **Idempotent and resumable.** Re-running a command is always safe and converges. Long operations (`collection install`, `nexus install`) persist progress as they go.
4. **Exit codes carry meaning.** `0` ok · `1` error · `2` usage error · `3` drift (`status`) or a `doctor` error · **`4` incomplete: progress was made but a decision is needed** (`data.pending`).
5. **`next` tells you what to do.** `mo-linux -j next` inspects the instance and returns ordered `steps` with the exact argv, why, effects, and flags `blocking` / `needs_human` / `confirm`. When `ready` is true, the only remaining step is `run`.

## The loop

```
mo-linux -j next                      # what should happen now?
  → run each step with  needs_human=false  and  confirm=false  yourself
  → ask the user for steps with needs_human=true or confirm=true
  → repeat until ready=true
```

## Setting up from nothing

```
mo-linux -j instance init                       # auto-detects Steam, game dir, prefix, Proton
mo-linux -j doctor                              # what is wrong, with machine-readable fixes
mo-linux -j nexus whoami                        # is there an API key? (see "credentials")
mo-linux -j skse install                        # the SKSE64 build matching the game version
mo-linux -j collection search "essential"       # or: nexus search "skyui"
mo-linux -j collection install SLUG             # resumable; exit 4 means pending decisions
mo-linux -j plugins list                        # load order + master problems
mo-linux -j apply && mo-linux -j plugins sync
mo-linux -j run --skse --detach                 # CONFIRM WITH THE USER FIRST
```

Pass `-i DIR` (or set `MOL_INSTANCE`) to choose the instance directory, `-p NAME` to choose a profile (created on demand by `collection install`/`instance init`).

## Exit code 4: the "incomplete" protocol

mo-linux never asks questions mid-run. Anything that needs a decision is recorded as *pending* while everything else continues. You get exit 4 and `data.pending[]`:

| `kind` | Meaning | You do |
|---|---|---|
| `fomod_choices` | The archive has a FOMOD installer and no choices were supplied | `mo-linux -j fomod inspect ARCHIVE` → pick (or accept defaults) → `collection resolve … --fomod FILE` / `--fomod-defaults`, or re-run `nexus install … --fomod FILE` |
| `manual_download` | Free Nexus account, browser-only or manual source | Give the user `url`; they click download and send you an `nxm://` link → `collection resolve … --nxm LINK` (or `--archive FILE`) |
| `unsupported` | Binary patches or bundled sources | `collection resolve … --skip`, or supply an archive with `--archive` |

Then run the **same command again**. Finished mods are not redone; downloaded archives are found by size + md5.

FOMOD choices file: `{"steps":{"<step>":{"<group>":["<plugin>", …]}}}`. `fomod inspect --choices FILE` re-evaluates later steps for the choices so far (steps and plugin types depend on flags set earlier), so loop on it until every visible group has an entry.

## Wabbajack mod lists

```
mo-linux -j wabbajack search "essentials"       # smallest first; NSFW hidden unless --nsfw
mo-linux -j wabbajack inspect MACHINE_URL       # verdict + sources + unsupported directives — decide before downloading GBs
mo-linux -j -i ~/lists/NAME wabbajack install MACHINE_URL   # -i is the OUTPUT instance; exit 4 = pending
```

Same incomplete protocol: `manual_download` rows (Mega, MediaFire, Google Drive, free-account Nexus…) carry the archive name and page `url`; the user puts the file into `<instance>/downloads/` (any file name: it is matched by size + xxh64) and you run `install` again. `game_file_missing` / hash mismatches on `gamefile` archives mean the game version differs from the one the list was built for.

## Where a human must be involved

- **Starting the game** (`run`): always confirm. It launches a real process and writes the user's Wine prefix.
- **`overwrite promote --yes`**: moves files into the real game directory, irreversibly. Preview first (no `--yes`), show the user the list.
- **Nexus API key**: only the user can create it (nexusmods.com → Account → API). Never invent, print, log or store it anywhere except through `mo-linux nexus login` (key on stdin). If a key was pasted into a chat, tell the user to rotate it afterwards.
- **Free Nexus accounts**: downloads need an `nxm://` link from the website.
- **Game or Proton problems** that `doctor` marks as errors without a `fix` (missing game, no prefix): the user must install/run the game from Steam once.

## Safety rules

- Do not edit instance files by hand (`modlist.txt`, `plugins.txt`, `meta.ini`, `state.json`); use the commands.
- Do not run commands marked `confirm` without asking.
- Never try to bypass `farm_busy` — the game is running. Ask the user to close it.
- Do not loop on `nexus_rate_limited`; wait.
- Do not use another application's Nexus identity (for example Mod Organizer 2's) — Nexus forbids it. A personal API key is fine for personal use.
- The real game directory is only written by `overwrite promote --yes`. Everything else works on symlinks in the farm. If something went wrong with the game files, Steam's "Verify integrity" restores them.

## Diagnosing a bad launch

1. `mo-linux -j doctor` — masters, SKSE vs game version, farm drift, plugin link.
2. `mo-linux -j logs` — list log files in the prefix' `My Games/Skyrim Special Edition/SKSE`; `logs --file skse64.log --tail 200` reads one. Look for `disabled, address library needs to be updated` (missing/mismatched Address Library), `loaded correctly` counts, and the per-plugin logs.
3. `mo-linux -j plugins list` → `issues[]` (`missing` / `disabled` / `after`); fix with `plugins enable|sort`.
4. `mo-linux -j nexus info --mod ID` shows a mod's declared requirements; `nexus install --mod ID --requirements` installs them.

## Environment

| Variable | Use |
|---|---|
| `MOL_INSTANCE` | default instance directory |
| `MOL_GAME_LIB` | path to `libmo-game.so` (game plugin host) when it is not next to the binary |
| `NEXUS_API_KEY` | API key (overrides the stored one; prefer `nexus login`) |
| `https_proxy` etc. | honoured by all downloads |

## Known limits

LOOT-style sorting is approximate (`plugins sort --loot` applies the community masterlist's after/req rules and groups; conditions, userlists and overlap heuristics are not implemented). Plain `plugins sort` only puts masters first. Wabbajack: `CreateBSA`/`TransformedTexture`/`MergedPatch` directives are not executed (reported as `unsupported`); `wabbajack inspect` tells you up front whether a list is `full`/`partial`/`none` installable. Collections: `requires`/`conflicts` rules are reported, not enforced; bundled sources and binary patches are `unsupported`. Writes by the game *through* an existing symlink land in the original file (see HANDBOOK R13).
