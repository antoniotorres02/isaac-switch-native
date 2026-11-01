-- Benchmark load for the native host (host, --bench).
-- The host prepends BENCH_SCENARIO = "<load>[:<stage>]" to this file.
-- Loads, all in the starting room of the floor, with the autotest's scripted
-- walk and shots:
--   room    nothing added (the baseline)
--   horde   36 gapers
--   swarm   72 attack flies
--   tears   36 gapers, and Isaac fires four tears at once
-- The stage is the debug console's: 1 Basement, 1c Downpour, 1d Dross,
-- 3b Flooded Caves, 3c Mines, 3d Ashpit, 5c Mausoleum, 5d Gehenna, 7c Corpse,
-- ... Without one the run stays on the first floor.
-- The player takes no damage and the enemies have a million hit points, so
-- the load stays the same for the whole measured window. The run's seed is
-- the one the Switch's start always gets (the PC one follows the clock), so
-- every floor is the same layout on both.
local mod = RegisterMod("isaac-bench", 1)
local load_name, stage = (BENCH_SCENARIO or "room"):match("^([^:]+):?(.*)$")

local LOADS = {
  room  = { },
  horde = { type = EntityType.ENTITY_GAPER, count = 36 },
  swarm = { type = EntityType.ENTITY_ATTACKFLY, count = 72 },
  tears = { type = EntityType.ENTITY_GAPER, count = 36, item = CollectibleType.COLLECTIBLE_MUTANT_SPIDER },
}
local load = LOADS[load_name] or LOADS.room
local SEED = "GZQW B4T8"
local moved = stage == ""       -- on the requested floor
local spawned = false

local function log(msg) Isaac.DebugString("[bench] " .. msg) end

mod:AddCallback(ModCallbacks.MC_POST_GAME_STARTED, function()
  local seed = Game():GetSeeds():GetStartSeedString()
  log("load " .. load_name .. ", stage " .. (stage ~= "" and stage or "start") .. ", seed " .. seed)
  if seed ~= SEED then
    log("restarting with seed " .. SEED)
    Isaac.ExecuteCommand("seed " .. SEED)
  end
end)

mod:AddCallback(ModCallbacks.MC_POST_NEW_LEVEL, function()
  local level = Game():GetLevel()
  log("level " .. level:GetName() .. " (stage " .. level:GetStage() .. ", type " .. level:GetStageType() .. ")")
end)

mod:AddCallback(ModCallbacks.MC_ENTITY_TAKE_DMG, function() return false end, EntityType.ENTITY_PLAYER)

-- A grid over the room, two tiles in from the walls.
local function spawn_load()
  local room = Game():GetRoom()
  local tl, br = room:GetTopLeftPos(), room:GetBottomRightPos()
  local cols = 9
  local rows = math.ceil(load.count / cols)
  local n = 0
  for r = 0, rows - 1 do
    for c = 0, cols - 1 do
      if n == load.count then break end
      local x = tl.X + 80 + (br.X - tl.X - 160) * c / (cols - 1)
      local y = tl.Y + 60 + (br.Y - tl.Y - 120) * r / math.max(rows - 1, 1)
      local e = Isaac.Spawn(load.type, 0, 0, Vector(x, y), Vector.Zero, nil)
      e.MaxHitPoints = 1000000
      e.HitPoints = 1000000
      n = n + 1
    end
  end
  log("spawned " .. n .. " entities")
end

-- The scripted walk would take Isaac through a door, into rooms that change
-- with the seed; he stays inside the starting room, clear of the doors.
local function keep_inside()
  local room = Game():GetRoom()
  local tl, br = room:GetTopLeftPos(), room:GetBottomRightPos()
  local p = Isaac.GetPlayer(0)
  local x = math.max(tl.X + 60, math.min(br.X - 60, p.Position.X))
  local y = math.max(tl.Y + 60, math.min(br.Y - 60, p.Position.Y))
  if x ~= p.Position.X or y ~= p.Position.Y then p.Position = Vector(x, y) end
end

mod:AddCallback(ModCallbacks.MC_POST_UPDATE, function()
  keep_inside()
  if not moved then
    if Game():GetFrameCount() < 10 then return end
    moved = true
    Isaac.ExecuteCommand("stage " .. stage)
    return
  end
  if spawned or Game():GetRoom():GetFrameCount() < 30 then return end
  spawned = true
  if load.item then Isaac.GetPlayer(0):AddCollectible(load.item, 0, false) end
  if load.count then spawn_load() end
end)
