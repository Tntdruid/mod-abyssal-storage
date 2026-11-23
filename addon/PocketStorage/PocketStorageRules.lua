-- Pocket Storage - Store/Keep rules window (per account, saved on the server)

local ROW_HEIGHT = 22
local VISIBLE_ROWS = 12
local WIDTH = 380

local GOLD = { 1, 0.85, 0 }

local function Solid(parent, layer, r, g, b, a)
    local tex = parent:CreateTexture(nil, layer)
    tex:SetTexture("Interface\\Buttons\\WHITE8X8")
    tex:SetVertexColor(r, g, b, a)
    return tex
end

local function SkinButton(btn)
    btn:SetNormalTexture("Interface\\Buttons\\WHITE8X8")
    btn:SetPushedTexture("Interface\\Buttons\\WHITE8X8")
    btn:SetHighlightTexture("Interface\\Buttons\\WHITE8X8")
    btn:GetNormalTexture():SetVertexColor(0.12, 0.12, 0.12, 0.9)
    btn:GetPushedTexture():SetVertexColor(0.18, 0.18, 0.18, 1)
    btn:GetHighlightTexture():SetVertexColor(0.25, 0.25, 0.25, 0.9)
    btn:GetFontString():SetTextColor(unpack(GOLD))
end

local function Count(set)
    local n = 0
    for _ in pairs(set) do n = n + 1 end
    return n
end

local function CursorItemEntry()
    local kind, id, link = GetCursorInfo()
    if kind ~= "item" then return nil end
    return tonumber(id) or PocketStorage:EntryFromLink(link)
end

local function AddFromCursor()
    local entry = CursorItemEntry()
    if entry then
        PocketStorage:AddRule(entry, PocketStorage.activeRuleTab)
        ClearCursor()
    end
end

-- ============================================================================
-- Main Frame
-- ============================================================================

local function CreateRulesFrame()
    if PocketStorageRulesFrame then return PocketStorageRulesFrame end

    local frame = CreateFrame("Frame", "PocketStorageRulesFrame", UIParent)
    frame:SetSize(WIDTH, 470)
    frame:SetPoint("CENTER")
    frame:SetFrameStrata("DIALOG")
    frame:EnableMouse(true)
    frame:SetMovable(true)
    frame:SetClampedToScreen(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", frame.StartMoving)
    frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
    frame:Hide()
    tinsert(UISpecialFrames, "PocketStorageRulesFrame") -- close with Escape

    Solid(frame, "BACKGROUND", 0.07, 0.07, 0.07, 0.95):SetAllPoints()

    local header = frame:CreateTexture(nil, "ARTWORK")
    header:SetPoint("TOPLEFT")
    header:SetPoint("TOPRIGHT")
    header:SetHeight(32)
    header:SetTexture("Interface\\PaperDollInfoFrame\\UI-GearManager-Title-Background")

    local icon = frame:CreateTexture(nil, "OVERLAY")
    icon:SetSize(18, 18)
    icon:SetPoint("TOPLEFT", 10, -7)
    icon:SetTexture("Interface\\Icons\\INV_Misc_Bag_EnchantedMageweave")

    local title = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightLarge")
    title:SetPoint("TOP", 0, -8)
    title:SetText("Storage Rules")
    title:SetTextColor(unpack(GOLD))

    local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", -2, -2)

    -- ========================================================================
    -- Tabs
    -- ========================================================================

    local function MakeTab(x, listKey, tip)
        local btn = CreateFrame("Button", nil, frame)
        btn:SetSize(170, 24)
        btn:SetPoint("TOPLEFT", x, -40)
        btn.bg = Solid(btn, "BACKGROUND", 0.15, 0.15, 0.15, 0.7)
        btn.bg:SetAllPoints()

        btn.label = btn:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
        btn.label:SetPoint("CENTER")

        function btn:SetActive(active)
            if active then
                self.bg:SetVertexColor(GOLD[1], GOLD[2], GOLD[3], 0.8)
                self.label:SetTextColor(1, 1, 1)
            else
                self.bg:SetVertexColor(0.15, 0.15, 0.15, 0.7)
                self.label:SetTextColor(0.8, 0.8, 0.8)
            end
        end

        btn:SetScript("OnClick", function()
            PocketStorage.activeRuleTab = listKey
            PocketStorage:UpdateRulesUI()
        end)
        btn:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine(self.label:GetText())
            GameTooltip:AddLine(tip, 1, 1, 1, true)
            GameTooltip:Show()
        end)
        btn:SetScript("OnLeave", GameTooltip_Hide)
        return btn
    end

    local tabStore = MakeTab(16, "store", "Always stored, even items that are not trade goods. Only stackable items.")
    local tabKeep = MakeTab(WIDTH - 186, "keep", "Never stored, not even trade goods.")

    -- ========================================================================
    -- List (FauxScrollFrame so it scrolls past the visible rows)
    -- ========================================================================

    local listBg = Solid(frame, "BORDER", 0, 0, 0, 0.25)
    listBg:SetPoint("TOPLEFT", 16, -72)
    listBg:SetSize(WIDTH - 32, ROW_HEIGHT * VISIBLE_ROWS)

    local scroll = CreateFrame("ScrollFrame", "PocketStorageRulesScroll", frame, "FauxScrollFrameTemplate")
    scroll:SetPoint("TOPLEFT", listBg)
    scroll:SetPoint("BOTTOMRIGHT", listBg, -24, 0)
    scroll:SetScript("OnVerticalScroll", function(self, offset)
        FauxScrollFrame_OnVerticalScroll(self, offset, ROW_HEIGHT, function() PocketStorage:UpdateRulesUI() end)
    end)

    local empty = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    empty:SetPoint("CENTER", listBg)
    empty:SetWidth(WIDTH - 80)

    local rows = {}
    for i = 1, VISIBLE_ROWS do
        local row = CreateFrame("Button", nil, frame)
        row:SetSize(WIDTH - 56, ROW_HEIGHT)
        row:SetPoint("TOPLEFT", listBg, 0, -(i - 1) * ROW_HEIGHT)
        row:RegisterForClicks("LeftButtonUp", "RightButtonUp")

        row.bg = Solid(row, "BACKGROUND", 0, 0, 0, i % 2 == 0 and 0.22 or 0.1)
        row.bg:SetAllPoints()

        row.icon = row:CreateTexture(nil, "ARTWORK")
        row.icon:SetSize(18, 18)
        row.icon:SetPoint("LEFT", 6, 0)

        row.text = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        row.text:SetPoint("LEFT", row.icon, "RIGHT", 8, 0)
        row.text:SetPoint("RIGHT", -24, 0)
        row.text:SetJustifyH("LEFT")

        row.remove = row:CreateTexture(nil, "OVERLAY")
        row.remove:SetSize(12, 12)
        row.remove:SetPoint("RIGHT", -6, 0)
        row.remove:SetTexture("Interface\\Buttons\\UI-StopButton")
        row.remove:Hide()

        row:SetScript("OnClick", function(self, button)
            if not self.itemEntry then return end
            if button == "RightButton" then
                PocketStorage:RemoveRule(self.itemEntry)
            elseif IsModifiedClick("CHATLINK") then
                local _, link = GetItemInfo(self.itemEntry)
                if link then ChatEdit_InsertLink(link) end
            end
        end)
        row:SetScript("OnEnter", function(self)
            if not self.itemEntry then return end
            self.bg:SetVertexColor(0.3, 0.3, 0.3, 0.5)
            self.remove:Show()
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetHyperlink("item:" .. self.itemEntry)
            GameTooltip:AddLine("Right-click to remove", 0.6, 0.6, 0.6)
            GameTooltip:Show()
        end)
        row:SetScript("OnLeave", function(self)
            self.bg:SetVertexColor(0, 0, 0, i % 2 == 0 and 0.22 or 0.1)
            self.remove:Hide()
            GameTooltip:Hide()
        end)
        -- Dropping straight on the list works too
        row:SetScript("OnReceiveDrag", AddFromCursor)

        rows[i] = row
    end

    -- ========================================================================
    -- Drop Zone
    -- ========================================================================

    local drop = CreateFrame("Button", nil, frame)
    drop:SetSize(WIDTH - 32, 56)
    drop:SetPoint("TOPLEFT", listBg, "BOTTOMLEFT", 0, -10)
    drop.bg = Solid(drop, "BACKGROUND", 0.12, 0.12, 0.12, 0.8)
    drop.bg:SetAllPoints()

    drop.label = drop:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    drop.label:SetPoint("CENTER")

    drop:SetScript("OnReceiveDrag", AddFromCursor)
    drop:SetScript("OnClick", AddFromCursor)
    drop:SetScript("OnEnter", function(self)
        if GetCursorInfo() == "item" then self.bg:SetVertexColor(GOLD[1], GOLD[2], GOLD[3], 0.25) end
    end)
    drop:SetScript("OnLeave", function(self)
        self.bg:SetVertexColor(0.12, 0.12, 0.12, 0.8)
    end)

    -- ========================================================================
    -- Add by Link or ID
    -- ========================================================================

    local input = CreateFrame("EditBox", "PocketStorageRulesInput", frame, "InputBoxTemplate")
    input:SetSize(WIDTH - 130, 22)
    input:SetPoint("TOPLEFT", drop, "BOTTOMLEFT", 6, -10)
    input:SetAutoFocus(false)
    input:SetFontObject("GameFontHighlightSmall")

    local hint = input:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("LEFT", 2, 0)
    hint:SetText("Item ID or shift-click an item")
    -- 3.3.5 has no SetShown, so toggle the hint by hand
    local function UpdateHint(self)
        if self:GetText() == "" and not self:HasFocus() then hint:Show() else hint:Hide() end
    end
    input:SetScript("OnTextChanged", UpdateHint)
    input:SetScript("OnEditFocusGained", function() hint:Hide() end)
    input:SetScript("OnEditFocusLost", UpdateHint)
    input:SetScript("OnEscapePressed", input.ClearFocus)

    local addBtn = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    addBtn:SetSize(80, 24)
    addBtn:SetPoint("LEFT", input, "RIGHT", 10, 0)
    addBtn:SetText("Add")
    SkinButton(addBtn)

    local function AddFromInput()
        local text = input:GetText()
        if text ~= "" then
            PocketStorage:AddRule(text, PocketStorage.activeRuleTab)
            input:SetText("")
        end
        input:ClearFocus()
    end
    addBtn:SetScript("OnClick", AddFromInput)
    input:SetScript("OnEnterPressed", AddFromInput)

    -- Shift-clicking an item while the box has focus puts its link here
    hooksecurefunc("ChatEdit_InsertLink", function(link)
        if input:HasFocus() and link then
            input:SetText(link)
        end
    end)

    -- ========================================================================
    -- Footer: auto-store switch
    -- ========================================================================

    local autoStore = CreateFrame("CheckButton", "PocketStorageRulesAutoStore", frame, "UICheckButtonTemplate")
    autoStore:SetSize(24, 24)
    autoStore:SetPoint("BOTTOMLEFT", 12, 12)
    _G[autoStore:GetName() .. "Text"]:SetText("Auto-store on loot")
    autoStore:SetScript("OnClick", function(self)
        PocketStorage:SetAutoStore(self:GetChecked() and true or false)
    end)
    autoStore:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_TOP")
        GameTooltip:AddLine("Auto-store on loot")
        GameTooltip:AddLine("Applies to every character on this account. Deposit All still works when it is off.", 1, 1, 1, true)
        GameTooltip:Show()
    end)
    autoStore:SetScript("OnLeave", GameTooltip_Hide)

    local accountNote = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    accountNote:SetPoint("BOTTOMRIGHT", -16, 18)
    accountNote:SetText("Saved for this account")

    -- ========================================================================
    -- Refresh
    -- ========================================================================

    function PocketStorage:UpdateRulesUI()
        if not frame:IsShown() then return end

        local isStore = self.activeRuleTab ~= "keep"
        local list = isStore and self.rules.store or self.rules.keep

        local entries = {}
        for entry in pairs(list) do
            entries[#entries + 1] = entry
        end

        local function NameOf(entry) return GetItemInfo(entry) or "" end
        table.sort(entries, function(a, b)
            local na, nb = NameOf(a), NameOf(b)
            if na == nb then return a < b end
            return na < nb
        end)

        FauxScrollFrame_Update(scroll, #entries, VISIBLE_ROWS, ROW_HEIGHT)
        local offset = FauxScrollFrame_GetOffset(scroll)

        for i, row in ipairs(rows) do
            local entry = entries[i + offset]
            if entry then
                local name, _, quality, _, _, _, _, _, _, texture = GetItemInfo(entry)
                -- The item info loader refreshes this window once the server answers
                if not name then PocketStorage:RequestItemInfo(entry) end
                local r, g, b = GetItemQualityColor(quality or 1)
                row.itemEntry = entry
                row.icon:SetTexture(texture or PocketStorage:GetItemIcon(entry))
                row.text:SetText(name or ("Item #" .. entry))
                row.text:SetTextColor(r, g, b)
                row:Show()
            else
                row.itemEntry = nil
                row:Hide()
            end
        end


        tabStore:SetActive(isStore)
        tabKeep:SetActive(not isStore)
        tabStore.label:SetText(("Store (%d)"):format(Count(self.rules.store)))
        tabKeep.label:SetText(("Keep (%d)"):format(Count(self.rules.keep)))

        if #entries == 0 then
            empty:SetText(isStore
                and "Your store list is empty.\nDrop items here to always store them."
                or "Your keep list is empty.\nDrop items here to keep them in your bags.")
            empty:Show()
        else
            empty:Hide()
        end

        drop.label:SetText(isStore
            and "Drop an item here to |cffffd700store|r it on loot"
            or "Drop an item here to |cff00ff00keep|r it")

        autoStore:SetChecked(self.autoStore)
    end

    frame:SetScript("OnShow", function() PocketStorage:UpdateRulesUI() end)

    return frame
end

function PocketStorage:ToggleRulesUI()
    local frame = CreateRulesFrame()
    if frame:IsShown() then
        frame:Hide()
    else
        frame:Show()
    end
end
