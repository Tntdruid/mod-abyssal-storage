-- Pocket Storage - Core Logic
PocketStorage = PocketStorage or {}
PocketStorage.items = {} -- { [itemEntry] = count }
PocketStorage.PREFIX = "PCKT"
-- Per-account Store/Keep lists, mirrored from the server
PocketStorage.rules = { store = {}, keep = {} } -- { [itemEntry] = true }
PocketStorage.autoStore = true
PocketStorage.activeRuleTab = "store"

-- ============================================================================
-- Utility
-- ============================================================================

local function ParseItemEntry(str)
    return tonumber(str)
end

-- Simple timer for 3.3.5 (no C_Timer)
local timerFrame = CreateFrame("Frame")
local activeTimers = {}
timerFrame:SetScript("OnUpdate", function(self, elapsed)
    local i = 1
    while i <= #activeTimers do
        local t = activeTimers[i]
        t.remaining = t.remaining - elapsed
        if t.remaining <= 0 then
            t.callback()
            table.remove(activeTimers, i)
        else
            i = i + 1
        end
    end
    if #activeTimers == 0 then
        self:Hide()
    end
end)
timerFrame:Hide()

function PocketStorage.SetTimer(delay, callback)
    table.insert(activeTimers, { remaining = delay, callback = callback })
    timerFrame:Show()
end

function PocketStorage.CancelTimers()
    wipe(activeTimers)
    timerFrame:Hide()
end

-- Extract item entry from an item link: |Hitem:12345:...|h[Name]|h
function PocketStorage:EntryFromLink(link)
    if not link then return nil end
    local entry = link:match("item:(%d+)")
    return entry and tonumber(entry) or nil
end

function PocketStorage:GetItemCount(entry)
    return self.items[entry] or 0
end

-- ============================================================================
-- Server Communication
-- ============================================================================

function PocketStorage:SendCommand(cmd)
    SendChatMessage("." .. cmd, "SAY")
end

function PocketStorage:Withdraw(entry, count)
    self:SendCommand("ps withdraw " .. entry .. " " .. (count or 0))
end

function PocketStorage:Deposit()
    self:SendCommand("ps deposit")
end

function PocketStorage:RequestSync()
    self:SendCommand("ps sync")
end

-- Accepts an item id, an item link or a number typed as text
function PocketStorage:ToItemEntry(input)
    if not input then return nil end
    return tonumber(input) or self:EntryFromLink(tostring(input))
end

function PocketStorage:AddRule(input, listKey)
    local entry = self:ToItemEntry(input)
    if not entry then
        DEFAULT_CHAT_FRAME:AddMessage("|cffff4444Pocket Storage: Drop an item, shift-click a link or type an item ID.|r")
        return
    end
    self:SendCommand("ps rule " .. (listKey == "keep" and "keep " or "store ") .. entry)
end

function PocketStorage:RemoveRule(entry)
    if entry then
        self:SendCommand("ps rule remove " .. entry)
    end
end

function PocketStorage:SetAutoStore(enabled)
    self:SendCommand("ps autostore " .. (enabled and "on" or "off"))
end

local function RefreshRules()
    if PocketStorage.UpdateRulesUI then PocketStorage:UpdateRulesUI() end
end

-- ============================================================================
-- Message Parsing
-- ============================================================================

-- msg is the body (arg2 from CHAT_MSG_ADDON), e.g. "SYNC:entry,count;..."
function PocketStorage:HandleMessage(msg)
    if not msg then return end

    local cmd = msg:match("^(%u+):")
    local payload = msg:match("^%u+:(.*)")

    if not cmd then return end

    if cmd == "SYNC" then
        self:HandleSync(payload)
    elseif cmd == "UPD" then
        self:HandleUpdate(payload)
    elseif cmd == "DEL" then
        self:HandleDelete(payload)
    elseif cmd == "ERR" then
        self:HandleError(payload)
    elseif cmd == "RULES" then
        self:HandleRules(payload)
    elseif cmd == "RULE" then
        self:HandleRule(payload)
    elseif cmd == "AUTO" then
        self.autoStore = payload == "1"
        RefreshRules()
    end
end

-- RULES:BEGIN, RULES:S:<id,id,...>, RULES:K:<id,id,...>, RULES:END
function PocketStorage:HandleRules(payload)
    if payload == "BEGIN" then
        self._pendingRules = { store = {}, keep = {} }
        return
    end

    local pending = self._pendingRules
    if not pending then return end

    if payload == "END" then
        self.rules = pending
        self._pendingRules = nil
        RefreshRules()
        return
    end

    local kind, ids = payload:match("^([SK]):(.+)$")
    if kind then
        local target = kind == "S" and pending.store or pending.keep
        for id in ids:gmatch("%d+") do
            target[tonumber(id)] = true
        end
    end
end

-- RULE:<S|K|N>:<id>, N = removed. An item is on one list at most.
function PocketStorage:HandleRule(payload)
    local kind, id = (payload or ""):match("^([SKN]):(%d+)$")
    if not kind then return end

    id = tonumber(id)
    self.rules.store[id] = nil
    self.rules.keep[id] = nil
    if kind == "S" then
        self.rules.store[id] = true
    elseif kind == "K" then
        self.rules.keep[id] = true
    end
    RefreshRules()
end

function PocketStorage:HandleSync(payload)
    if not payload or payload == "" then
        self.items = {}
        if self.UpdateUI then self:UpdateUI() end
        return
    end

    -- Parse "entry1,count1;entry2,count2;..."
    -- SYNC messages may arrive in multiple packets — merge instead of replace
    -- We track sync state: first SYNC clears, subsequent ones merge
    if not self._syncActive then
        self.items = {}
        self._syncActive = true
    end

    for pair in payload:gmatch("[^;]+") do
        local entry, count = pair:match("(%d+),(%d+)")
        if entry and count then
            self.items[tonumber(entry)] = tonumber(count)
        end
    end

    -- Debounce UI update for multi-packet syncs
    PocketStorage.CancelTimers()
    PocketStorage.SetTimer(0.1, function()
        PocketStorage._syncActive = false
        if PocketStorage.UpdateUI then PocketStorage:UpdateUI() end
    end)
end

function PocketStorage:HandleUpdate(payload)
    if not payload then return end
    local entry, count = payload:match("(%d+),(%d+)")
    if entry and count then
        entry = tonumber(entry)
        count = tonumber(count)
        if count > 0 then
            self.items[entry] = count
        else
            self.items[entry] = nil
        end
        -- Skip UI refresh during multi-packet SYNC to avoid flashing incomplete data
        if not self._syncActive and self.UpdateUI then self:UpdateUI() end
    end
end

function PocketStorage:HandleDelete(payload)
    if not payload then return end
    local entry = tonumber(payload)
    if entry then
        self.items[entry] = nil
        if not self._syncActive and self.UpdateUI then self:UpdateUI() end
    end
end

function PocketStorage:HandleError(payload)
    if payload then
        DEFAULT_CHAT_FRAME:AddMessage("|cffff4444Pocket Storage: " .. payload .. "|r")
    end
end

-- ============================================================================
-- Event Frame
-- ============================================================================

if RegisterAddonMessagePrefix then
    RegisterAddonMessagePrefix(PocketStorage.PREFIX)
end

local eventFrame = CreateFrame("Frame", "PocketStorageEventFrame", UIParent)
eventFrame:RegisterEvent("CHAT_MSG_ADDON")
eventFrame:RegisterEvent("PLAYER_LOGIN")

eventFrame:SetScript("OnEvent", function(self, event, arg1, arg2, ...)
    if event == "CHAT_MSG_ADDON" then
        -- arg1 = prefix ("PCKT"), arg2 = message body
        if arg1 == PocketStorage.PREFIX then
            PocketStorage:HandleMessage(arg2)
        end
    elseif event == "PLAYER_LOGIN" then
        PocketStorage:RequestSync()
    end
end)

-- ============================================================================
-- Item Tooltips — show the vault count on every item tooltip
-- ============================================================================

local VAULT_LINE = "Pocket Storage"

-- OnTooltipSetItem can fire more than once per tooltip (e.g. recipes), so look
-- for our line instead of keeping a flag that other addons' tooltip use can leave stale
local function HasVaultLine(tooltip)
    local name = tooltip:GetName()
    for i = 1, tooltip:NumLines() do
        local left = _G[name .. "TextLeft" .. i]
        if left and left:GetText() == VAULT_LINE then
            return true
        end
    end
    return false
end

local function AddVaultLine(tooltip)
    if HasVaultLine(tooltip) then return end

    -- The vault grid adds its own Bags/Storage/Total lines
    local owner = tooltip:GetOwner()
    if owner and owner.isPocketCell then return end

    local _, link = tooltip:GetItem()
    local entry = PocketStorage:EntryFromLink(link)
    if not entry then return end

    local count = PocketStorage:GetItemCount(entry)
    if count > 0 then
        tooltip:AddDoubleLine(VAULT_LINE, tostring(count), 0.78, 0.82, 0.88, 0.2, 1, 0.6)
        tooltip:Show()
    end
end

for _, tooltip in ipairs({ GameTooltip, ItemRefTooltip }) do
    tooltip:HookScript("OnTooltipSetItem", AddVaultLine)
end

-- ============================================================================
-- Slash Commands
-- ============================================================================

SLASH_POCKETSTORAGE1 = "/ps"
SLASH_POCKETSTORAGE2 = "/pocket"
SlashCmdList["POCKETSTORAGE"] = function(msg)
    if msg == "" or msg == "show" then
        if PocketStorageFrame then
            if PocketStorageFrame:IsShown() then
                PocketStorageFrame:Hide()
            else
                PocketStorageFrame:Show()
                if PocketStorage.UpdateUI then
                    PocketStorage:UpdateUI()
                end
            end
        end
    elseif msg == "deposit" then
        PocketStorage:Deposit()
    elseif msg == "sync" then
        PocketStorage:RequestSync()
    elseif msg == "rules" then
        if PocketStorage.ToggleRulesUI then PocketStorage:ToggleRulesUI() end
    else
        local cmd, rest = msg:match("^(%S+)%s*(.*)")
        if cmd == "withdraw" then
            local entry, count = rest:match("(%d+)%s*(%d*)")
            if entry then
                PocketStorage:Withdraw(tonumber(entry), tonumber(count) or 0)
            else
                DEFAULT_CHAT_FRAME:AddMessage("|cffff4444Usage: /ps withdraw <itemEntry> [count]|r")
            end
        else
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffPocket Storage Commands:|r")
            DEFAULT_CHAT_FRAME:AddMessage("  /ps - Toggle storage window")
            DEFAULT_CHAT_FRAME:AddMessage("  /ps deposit - Deposit all trade goods")
            DEFAULT_CHAT_FRAME:AddMessage("  /ps sync - Re-sync from server")
            DEFAULT_CHAT_FRAME:AddMessage("  /ps rules - Edit the Store and Keep lists")
            DEFAULT_CHAT_FRAME:AddMessage("  /ps withdraw <id> [count] - Withdraw items")
        end
    end
end
