-- Skyrim SE compatibility rules (api_version 1).
--
-- Pure evaluation only: the host hands this script a read-only context snapshot
-- (game, game_version, facts, mods) and the script returns structured diagnostics
-- {id, level, message, hint?, action?}. It never touches the filesystem, spawns a
-- process, or invents paths — the C++ host (mol::rules) collected the
-- enb.compiler_log evidence and the preference facts before calling check().
--
-- The only action this host accepts is {type="disable_mod", name=...}, and it is
-- honoured solely when the player explicitly opted in (both the "show player
-- position" and "allow disable mod" preferences are "true"). Nothing here picks a
-- side between mutually exclusive optional packs or claims a missing fx file.
return {
  api_version = 1,
  id = "skyrim",
  check = function(ctx)
    -- Only Skyrim SE; every other game reports nothing.
    if ctx.game ~= "skyrimse" then return {} end

    local facts = ctx.facts or {}
    local out = {}

    -- Case-insensitive plain substring test (no Lua patterns, so names are literal).
    local function has(haystack, needle)
      if type(haystack) ~= "string" or needle == nil then return false end
      return string.find(string.lower(haystack), string.lower(needle), 1, true) ~= nil
    end

    -- Snapshot the enabled, on-disk mods once (separators/disabled/absent excluded).
    local mods = {}
    for _, mod in ipairs(ctx.mods or {}) do
      if mod.enabled and mod.exists and type(mod.name) == "string" then mods[#mods + 1] = mod end
    end
    local function alternatives(a, b, c, d)
      local left, right
      for _, mod in ipairs(mods) do
        if has(mod.name, a) and (b == nil or has(mod.name, b)) then
          if right and right ~= mod.name then return true end
          left = mod.name
        end
        if has(mod.name, c) and (d == nil or has(mod.name, d)) then
          if left and left ~= mod.name then return true end
          right = mod.name
        end
      end
      return false
    end

    -- (1) Get Lost for Anniversary Edition hides the player world-map marker. The
    -- "Unknown" position it produces is intentional, so this is an explanation, not
    -- an error. We only offer to disable it when the player both wants the marker
    -- shown and allows mo-linux to disable mods; otherwise the gameplay mod is left
    -- untouched (no preference-driven action).
    local lost
    for _, mod in ipairs(mods) do
      if string.lower(mod.name) == "get lost for anniversary edition" then lost = mod break end
    end
    if lost then
      local row = {
        id = "map.player_location",
        level = "warn",
        message = "Get Lost for Anniversary Edition is enabled; it intentionally hides the player position, so the world map shows 'Unknown' (by design, not a fault)",
        hint = "Get Lost is working as intended; the marker stays hidden while the mod is enabled",
      }
      if facts["preferences.show_player_worldmap_position"] == "true"
         and facts["preferences.allow_disable_mod"] == "true" then
        row.action = { type = "disable_mod", name = lost.name }
        row.hint = "you prefer to show the player position; disabling Get Lost restores the marker"
      end
      out[#out + 1] = row
    end

    -- (2) ENB shader compile failure: the native D3D compiler emitted E5020 while
    -- building the fx_5_0 target. This is evidence-based and advisory only — no
    -- automatic fix, and no claim that any fx file is merely missing.
    local enb = facts["enb.compiler_log"]
    if has(enb, "E5020") and has(enb, "fx_5_0") then
      out[#out + 1] = {
        id = "compiler_target",
        level = "warn",
        message = "the ENB shader compiler reported error E5020 while building the fx_5_0 target",
        hint = "validate the native compiler install, or set PreferNewestD3DCompiler=false in enbseries/enbfxcompiler.ini; no automatic fix is applied",
      }
    end

    -- (3) Mutually exclusive optional packs. We only surface that both alternatives
    -- are active and never choose for the player.
    if alternatives("ENB", "Performance", "ENB", "Ultra") then
      out[#out + 1] = {
        id = "conflicting_options.performance_ultra",
        level = "warn",
        message = "both an 'ENB … Performance' and an 'ENB … Ultra' preset are enabled; they are alternative presets and cannot both take effect",
        hint = "these are alternative options, not an error; keep the one you prefer and disable the other yourself",
      }
    end
    if alternatives("Story Mode", nil, "Hard Mode", nil) then
      out[#out + 1] = {
        id = "conflicting_options.story_hard",
        level = "warn",
        message = "both a 'Story Mode' and a 'Hard Mode' option are enabled; they are alternative gameplay modes",
        hint = "these are alternative options, not an error; choose the one you want yourself",
      }
    end

    return out
  end,
}
