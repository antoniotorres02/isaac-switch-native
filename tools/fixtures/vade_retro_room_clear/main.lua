local test = RegisterMod("Vade Retro room-clear test", 1)
local item = CollectibleType.COLLECTIBLE_VADE_RETRO
local greedier = BENCH_SCENARIO:sub(1, 9) == "greedier-"
local mode = greedier and Difficulty.DIFFICULTY_GREEDIER or Difficulty.DIFFICULTY_GREED
local final_wave = greedier and 12 or 11
local should_use = BENCH_SCENARIO:match("use$") ~= nil
local ticks, defeated = 0, 0
local last_wave = -1
local started, finished, used = false, false, false

local function log(message)
    Isaac.DebugString("[vade-retro-test] " .. message)
end

test:AddCallback(ModCallbacks.MC_POST_GAME_STARTED, function()
    local player = Isaac.GetPlayer(0)
    local should_equip = BENCH_SCENARIO ~= "control"
    if should_equip then player:AddCollectible(item, 6, true) end
    started = Game().Difficulty == mode and player:HasCollectible(item) == should_equip
    log("started difficulty " .. Game().Difficulty .. ", equipped " .. tostring(player:HasCollectible(item)))
    if not started then log("incorrect difficulty or active item") end
end)

test:AddCallback(ModCallbacks.MC_ENTITY_TAKE_DMG, function()
    return false
end, EntityType.ENTITY_PLAYER)

test:AddCallback(ModCallbacks.MC_POST_UPDATE, function()
    if not started or finished then return end

    ticks = ticks + 1
    local game = Game()
    local room = game:GetRoom()
    local player = Isaac.GetPlayer(0)
    local wave = game:GetLevel().GreedModeWave
    if wave ~= last_wave then
        log("wave " .. tostring(wave))
        last_wave = wave
    end

    -- Walk on and off the button to trigger the optional waves too.
    if ticks == 50 or (wave >= 8 and ticks % 90 == 10) then
        player.Position = room:GetCenterPos()
    elseif wave >= 8 and ticks % 90 == 0 then
        player.Position = room:GetCenterPos() + Vector(-90, 0)
    end

    if wave == final_wave and room:IsClear() and defeated > 0 then
        if not should_use or used then
            log("cleared all " .. final_wave .. " waves (" .. BENCH_SCENARIO .. ")")
            finished = true
        end
        return
    end

    if ticks >= 60 and ticks % 3 == 0 then
        for _, enemy in ipairs(Isaac.GetRoomEntities()) do
            if enemy:IsVulnerableEnemy() and not enemy:IsDead() then
                enemy:Kill()
                defeated = defeated + 1
            end
        end
        if should_use and defeated > 0 then
            player:UseActiveItem(item, UseFlag.USE_NOANIM)
            used = true
        end
    end
end)
