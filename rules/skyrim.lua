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

    -- (5) 注入地点与能力面（ctx.impact，由 C++ 静态分析 + 缓存提供）。
    -- 只做「告知」，不做任何停用建议：影响 ≠ 责任。
    local proxies, packed, chain, net_mods = {}, {}, {}, {}
    for _, im in ipairs(ctx.impact or {}) do
      local has_proxy, has_chain = false, false
      for _, inj in ipairs(im.injections or {}) do
        if inj.kind == "proxy_dll" then has_proxy = true end
        if inj.kind == "engine_dll" then has_chain = true end
      end
      if has_chain then chain[#chain + 1] = im.mod end
      if has_proxy then proxies[#proxies + 1] = im.mod end
      if im.packed_suspect then packed[#packed + 1] = im.mod end
      if im.caps and im.caps.network then net_mods[#net_mods + 1] = im.mod end
    end
    local function names(list)
      if #list == 0 then return "" end
      local s = list[1]
      for i = 2, math.min(#list, 4) do s = s .. ", " .. list[i] end
      if #list > 4 then s = s .. ", … (" .. (#list - 4) .. " more)" end
      return s
    end
    if #proxies > 0 then
      out[#out + 1] = {
        id = "injection.proxy_dll",
        level = "ok",
        message = #proxies .. " mod(s) install a proxy DLL, which every process that loads the game EXE also loads: " .. names(proxies),
        hint = "proxy DLLs (d3d11.dll, version.dll, winhttp.dll, …) run in the launcher and Steam overlay too; when something misbehaves outside the game, look here first",
      }
    end
    if #chain > 0 then
      out[#out + 1] = {
        id = "injection.chain_dll",
        level = "ok",
        message = #chain .. " mod(s) ship a DLL outside SKSE/Plugins, so the engine or another plugin loads it: " .. names(chain),
        hint = "these DLLs are chain-loaded at runtime; a mod can pull in code you did not explicitly install",
      }
    end
    if #net_mods > 0 then
      out[#out + 1] = {
        id = "injection.network",
        level = "ok",
        message = #net_mods .. " mod(s) contain code that imports networking APIs: " .. names(net_mods),
        hint = "this is an upper bound from the PE import table (dynamic resolution hides behavior); it does not mean the mod phones home",
      }
    end
    if #packed > 0 then
      out[#out + 1] = {
        id = "injection.packed",
        level = "warn",
        message = #packed .. " mod(s) ship a DLL that cannot be analyzed statically (writable+executable section or no import table): " .. names(packed),
        hint = "the capability profile of these DLLs is unknown, not empty; treat their import table as unreliable",
      }
    end

    return out
  end,
}
