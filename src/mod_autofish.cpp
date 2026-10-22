/*
 * mod-autofish - hands-free fishing for AzerothCore
 *
 * Cast Fishing once (or type .autofish) and the server takes over:
 * it hooks the fish when the bobber splashes, stores the catch in your bags,
 * re-applies lures, opens clams, and casts again - until you move, turn,
 * enter combat, run out of bag space, or type .autofish off.
 *
 * Everything runs inside the player's own map update, through the same code
 * paths the client would trigger by clicking, so skill-ups, fishing pools,
 * junk rolls, quest items and loot tables all behave normally.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DataMap.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <cmath>
#include <string>
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{
    // ------------------------------------------------------------------ config
    struct AutoFishConfig
    {
        bool     enabled         = true;
        bool     startOnCast     = true;
        uint32   minSecurity     = SEC_PLAYER;
        uint32   reactionMin     = 400;   // ms after splash before hooking
        uint32   reactionMax     = 1200;
        uint32   recastMin       = 800;   // ms between catch and next cast
        uint32   recastMax       = 1800;
        bool     autoLure        = true;
        bool     autoOpen        = true;
        bool     stopOnMove      = true;
        bool     stopOnCombat    = true;
        uint32   maxMinutes      = 0;     // 0 = unlimited
        uint32   minFreeSlots    = 1;
        bool     announceCatches = false;
    };

    AutoFishConfig sCfg;

    // Lures, best first: Glow Worm, Sharpened Fish Hook, Aquadynamic Fish Attractor,
    // Flesh Eating Worm, Bright Baubles, Nightcrawlers, Shiny Bauble
    constexpr uint32 LURES[] = { 46006, 34861, 6533, 7307, 6532, 6530, 6529 };

    // Openable catches: Big-mouth / Small Barnacled / Thick-shelled / Jaggal / Darkwater Clam, Reinforced Crate
    constexpr uint32 OPENABLES[] = { 7973, 5523, 5524, 24476, 36781, 44475 };

    constexpr uint32 TICK_MS        = 100;
    constexpr uint32 MAX_CAST_FAILS = 3;
    constexpr float  MOVE_TOLERANCE = 0.75f;  // yards
    constexpr float  TURN_TOLERANCE = 0.15f;  // radians

    // ------------------------------------------------------------- per player
    struct AutoFishState : public DataMap::Base
    {
        bool       active         = false;
        bool       optedOut       = false;   // .autofish off disables start-on-cast for this login
        uint32     spellId        = 0;
        uint32     tickAcc        = 0;
        uint64     startedAt      = 0;
        uint64     hookAt         = 0;       // when to hook the current splash
        uint64     nextActionAt   = 0;       // earliest time to lure/open/cast
        uint64     expectChannel  = 0;       // a cast was sent; channel should exist by now
        uint32     castFails      = 0;
        uint32     catches        = 0;
        uint32     casts          = 0;
        ObjectGuid openingItem;
        // item-use safety net: if using an item didn't consume it, skip that item
        // for the rest of the session instead of retrying forever
        uint32     pendingEntry   = 0;
        uint32     pendingCount   = 0;
        uint64     pendingCheckAt = 0;
        std::unordered_set<uint32> skipEntries;
        float      anchorX = 0, anchorY = 0, anchorZ = 0, anchorO = 0;
    };

    uint64 Now() { return GameTime::GetGameTimeMS().count(); }

    AutoFishState* GetState(Player* player)
    {
        return player->CustomData.GetDefault<AutoFishState>("mod-autofish");
    }

    void Notify(Player* player, std::string const& msg)
    {
        ChatHandler(player->GetSession()).SendSysMessage("|cff33ccff[AutoFish]|r " + msg);
    }

    // Real players only. Playerbots (random bots and altbots) run on sessions with
    // no network socket, and they fish on their own - never take one over.
    bool IsRealPlayer(Player* player)
    {
        WorldSession* session = player->GetSession();
        return session && !session->IsSocketClosed();
    }

    bool Allowed(Player* player)
    {
        return sCfg.enabled && IsRealPlayer(player)
            && player->GetSession()->GetSecurity() >= AccountTypes(sCfg.minSecurity);
    }

    Item* GetPole(Player* player)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (!item)
            return nullptr;
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Class != ITEM_CLASS_WEAPON || proto->SubClass != ITEM_SUBCLASS_WEAPON_FISHING_POLE)
            return nullptr;
        return item;
    }

    GameObject* GetBobber(Player* player)
    {
        ObjectGuid guid = player->GetGuidValue(UNIT_FIELD_CHANNEL_OBJECT);
        if (!guid.IsGameObject())
            return nullptr;
        GameObject* go = player->GetMap()->GetGameObject(guid);
        if (!go || go->GetGoType() != GAMEOBJECT_TYPE_FISHINGNODE || go->GetOwnerGUID() != player->GetGUID())
            return nullptr;
        return go;
    }

    void SetAnchor(Player* player, AutoFishState* st)
    {
        st->anchorX = player->GetPositionX();
        st->anchorY = player->GetPositionY();
        st->anchorZ = player->GetPositionZ();
        st->anchorO = player->GetOrientation();
    }

    void Start(Player* player, AutoFishState* st, uint32 spellId)
    {
        st->active        = true;
        st->spellId       = spellId;
        st->startedAt     = Now();
        st->hookAt        = 0;
        st->nextActionAt  = 0;
        st->expectChannel = 0;
        st->castFails     = 0;
        st->catches       = 0;
        st->casts         = 0;
        st->openingItem.Clear();
        st->pendingEntry  = 0;
        st->skipEntries.clear();
        SetAnchor(player, st);
        Notify(player, "On. Move, turn or type .autofish off to stop.");
    }

    void Stop(Player* player, AutoFishState* st, std::string const& reason)
    {
        if (!st->active)
            return;
        st->active = false;
        uint32 minutes = uint32((Now() - st->startedAt) / MINUTE / IN_MILLISECONDS);
        Notify(player, "Off (" + reason + "). " + std::to_string(st->catches) + " catches in "
            + std::to_string(minutes) + " min.");
    }

    // Finds the highest fishing cast spell the player knows, for .autofish without a manual cast first.
    uint32 FindFishingSpell(Player* player)
    {
        // Grand Master -> Apprentice
        static constexpr uint32 ranks[] = { 51294, 33095, 18248, 7732, 7731, 7620 };
        for (uint32 id : ranks)
            if (player->HasSpell(id))
                return id;
        return 0;
    }

    // -------------------------------------------------------------- looting
    // Takes everything from the loot window the player currently has open,
    // if it came from our bobber, a fishing pool, or a clam we opened.
    void TakeLoot(Player* player, AutoFishState* st)
    {
        ObjectGuid lguid = player->GetLootGUID();
        if (lguid.IsEmpty())
            return;

        Loot* loot = nullptr;
        if (lguid.IsGameObject())
        {
            GameObject* go = player->GetMap()->GetGameObject(lguid);
            if (!go)
                return;
            bool ours = (go->GetGoType() == GAMEOBJECT_TYPE_FISHINGNODE && go->GetOwnerGUID() == player->GetGUID())
                || go->GetGoType() == GAMEOBJECT_TYPE_FISHINGHOLE;
            if (!ours)
                return;
            loot = &go->loot;
        }
        else if (lguid.IsItem() && lguid == st->openingItem)
        {
            Item* item = player->GetItemByGuid(lguid);
            if (!item)
                return;
            loot = &item->loot;
        }
        else
            return;

        size_t questSlots = 0;
        QuestItemMap const& questMap = loot->GetPlayerQuestItems();
        auto qitr = questMap.find(player->GetGUID());
        if (qitr != questMap.end() && qitr->second)
            questSlots = qitr->second->size();

        bool bagsFull = false;
        size_t totalSlots = loot->items.size() + questSlots;
        for (size_t slot = 0; slot < totalSlots && slot < 255; ++slot)
        {
            if (!loot->LootItemInSlot(uint32(slot), player))
                continue; // already looted or not for us

            InventoryResult msg = EQUIP_ERR_OK;
            LootItem* got = player->StoreLootItem(uint8(slot), loot, msg);
            if (msg != EQUIP_ERR_OK)
            {
                bagsFull = true;
                continue;
            }
            if (got)
            {
                ++st->catches;
                if (sCfg.announceCatches)
                    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(got->itemid))
                        Notify(player, "Caught " + proto->Name1 + (got->count > 1 ? " x" + std::to_string(got->count) : ""));
            }
        }

        if (loot->gold)
        {
            sScriptMgr->OnPlayerBeforeLootMoney(player, loot);
            loot->NotifyMoneyRemoved();
            player->ModifyMoney(loot->gold);
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, loot->gold);
            loot->gold = 0;
        }

        player->GetSession()->DoLootRelease(lguid);
        if (lguid == st->openingItem)
            st->openingItem.Clear();

        st->nextActionAt = Now() + urand(sCfg.recastMin, sCfg.recastMax);

        if (bagsFull)
            Stop(player, st, "bags full");
    }

    // ---------------------------------------------------------- next actions
    // Remember how many of this item we had, to confirm later that using it worked.
    void ExpectConsumed(Player* player, AutoFishState* st, uint32 entry, uint32 checkAfterMs)
    {
        st->pendingEntry   = entry;
        st->pendingCount   = player->GetItemCount(entry);
        st->pendingCheckAt = Now() + checkAfterMs;
    }

    // If an item we tried to use is still all there, skip it from now on.
    void CheckConsumed(Player* player, AutoFishState* st)
    {
        if (!st->pendingEntry)
            return;
        if (player->GetItemCount(st->pendingEntry) < st->pendingCount)
        {
            st->pendingEntry = 0;   // it was used up - worked
            return;
        }
        if (Now() < st->pendingCheckAt || player->IsNonMeleeSpellCast(false))
            return;
        if (player->GetItemCount(st->pendingEntry) >= st->pendingCount)
        {
            st->skipEntries.insert(st->pendingEntry);
            std::string name = std::to_string(st->pendingEntry);
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(st->pendingEntry))
                name = proto->Name1;
            Notify(player, "Couldn't use " + name + " - skipping it this session.");
        }
        st->pendingEntry = 0;
    }

    bool TryApplyLure(Player* player, AutoFishState* st, Item* pole)
    {
        if (!sCfg.autoLure || pole->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
            return false;

        for (uint32 entry : LURES)
        {
            if (st->skipEntries.count(entry))
                continue;
            Item* lure = player->GetItemByEntry(entry);
            if (!lure || player->CanUseItem(lure) != EQUIP_ERR_OK)
                continue;

            SpellCastTargets targets;
            targets.SetItemTarget(pole);
            ExpectConsumed(player, st, entry, 8000);   // lures take a few seconds to apply
            player->CastItemUseSpell(lure, targets, 0, 0);
            st->nextActionAt = Now() + 1000;
            return true;
        }
        return false;
    }

    bool TryOpenCatch(Player* player, AutoFishState* st)
    {
        if (!sCfg.autoOpen || player->GetFreeInventorySpace() < sCfg.minFreeSlots + 1)
            return false;

        for (uint32 entry : OPENABLES)
        {
            if (st->skipEntries.count(entry))
                continue;
            Item* item = player->GetItemByEntry(entry);
            if (!item)
                continue;
            ItemTemplate const* proto = item->GetTemplate();
            if (proto->LockID && item->IsLocked())
                continue;

            st->openingItem = item->GetGUID();
            ExpectConsumed(player, st, entry, 4000);

            if (proto->HasFlag(ITEM_FLAG_HAS_LOOT))
            {
                // lockbox-style container (e.g. Reinforced Crate): opens a loot window
                WorldPacket packet(CMSG_OPEN_ITEM, 2);
                packet << uint8(item->GetBagSlot()) << uint8(item->GetSlot());
                player->GetSession()->HandleOpenItemOpcode(packet);
            }
            else if (player->CanUseItem(item) == EQUIP_ERR_OK)
            {
                // clams: "Use: Open the clam" - same as right-clicking it
                SpellCastTargets targets;   // no target, like the client sends for self-use items
                player->CastItemUseSpell(item, targets, 0, 0);
            }
            else
            {
                st->skipEntries.insert(entry);
                st->pendingEntry = 0;
                continue;
            }

            st->nextActionAt = Now() + 1000;
            return true;
        }
        return false;
    }

    void CastFishing(Player* player, AutoFishState* st)
    {
        SpellCastResult result = player->CastSpell(player, st->spellId, false);
        ++st->casts;
        if (result != SPELL_CAST_OK)
        {
            ++st->castFails;
            st->nextActionAt = Now() + 1500;
            return;
        }
        st->expectChannel = Now() + 1500;
        st->hookAt = 0;
    }

    // ----------------------------------------------------------- main loop
    void Update(Player* player, AutoFishState* st)
    {
        uint64 now = Now();

        // stop conditions
        if (!Allowed(player))
            return Stop(player, st, "disabled");
        if (!player->IsAlive() || player->IsMounted() || player->IsInFlight())
            return Stop(player, st, "can't fish right now");
        if (sCfg.stopOnCombat && player->IsInCombat())
            return Stop(player, st, "combat");
        if (sCfg.stopOnMove)
        {
            float dist = player->GetExactDist(st->anchorX, st->anchorY, st->anchorZ);
            float turn = std::fabs(Position::NormalizeOrientation(player->GetOrientation() - st->anchorO));
            if (turn > float(M_PI))
                turn = 2.0f * float(M_PI) - turn;
            if (dist > MOVE_TOLERANCE || turn > TURN_TOLERANCE)
                return Stop(player, st, "you moved");
        }
        if (sCfg.maxMinutes && now - st->startedAt > uint64(sCfg.maxMinutes) * MINUTE * IN_MILLISECONDS)
            return Stop(player, st, "time limit");
        if (st->castFails >= MAX_CAST_FAILS)
            return Stop(player, st, "can't cast here - face open water");

        Item* pole = GetPole(player);
        if (!pole)
            return Stop(player, st, "no fishing pole equipped");

        // a loot window from fishing is open -> empty it
        if (!player->GetLootGUID().IsEmpty())
        {
            TakeLoot(player, st);
            return;
        }

        // bobber out: wait for the splash, then hook after a short reaction delay
        GameObject* bobber = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL) ? GetBobber(player) : nullptr;
        if (bobber)
        {
            st->castFails = 0;
            st->expectChannel = 0;
            if (bobber->getLootState() != GO_READY)
                return;

            if (!st->hookAt)
            {
                st->hookAt = now + urand(sCfg.reactionMin, sCfg.reactionMax);
                return;
            }
            if (now < st->hookAt)
                return;

            st->hookAt = 0;
            if (player->GetFreeInventorySpace() < sCfg.minFreeSlots)
                return Stop(player, st, "bags full");

            bobber->Use(player);
            TakeLoot(player, st);
            if (st->active && player->GetLootGUID().IsEmpty())
                st->nextActionAt = std::max(st->nextActionAt, now + sCfg.recastMin);
            return;
        }

        // cast sent but no bobber appeared (bad spot, too shallow, interrupted)
        if (st->expectChannel && now > st->expectChannel)
        {
            st->expectChannel = 0;
            ++st->castFails;
            st->nextActionAt = now + 1000;
        }

        // busy casting something (a lure, or the fishing cast itself)
        if (player->IsNonMeleeSpellCast(false) || st->expectChannel || now < st->nextActionAt)
            return;

        if (player->GetFreeInventorySpace() < sCfg.minFreeSlots)
            return Stop(player, st, "bags full");

        CheckConsumed(player, st);
        if (st->pendingEntry)
            return;   // still waiting to confirm the last item use
        if (TryApplyLure(player, st, pole))
            return;
        if (TryOpenCatch(player, st))
            return;
        CastFishing(player, st);
    }
}

// ============================================================== scripts
class AutoFishWorldScript : public WorldScript
{
public:
    AutoFishWorldScript() : WorldScript("AutoFishWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sCfg.enabled         = sConfigMgr->GetOption<bool>("AutoFish.Enable", true);
        sCfg.startOnCast     = sConfigMgr->GetOption<bool>("AutoFish.StartOnCast", true);
        sCfg.minSecurity     = sConfigMgr->GetOption<uint32>("AutoFish.MinSecurity", 0);
        sCfg.reactionMin     = sConfigMgr->GetOption<uint32>("AutoFish.ReactionMinMs", 400);
        sCfg.reactionMax     = sConfigMgr->GetOption<uint32>("AutoFish.ReactionMaxMs", 1200);
        sCfg.recastMin       = sConfigMgr->GetOption<uint32>("AutoFish.RecastMinMs", 800);
        sCfg.recastMax       = sConfigMgr->GetOption<uint32>("AutoFish.RecastMaxMs", 1800);
        sCfg.autoLure        = sConfigMgr->GetOption<bool>("AutoFish.AutoLure", true);
        sCfg.autoOpen        = sConfigMgr->GetOption<bool>("AutoFish.AutoOpenClams", true);
        sCfg.stopOnMove      = sConfigMgr->GetOption<bool>("AutoFish.StopOnMove", true);
        sCfg.stopOnCombat    = sConfigMgr->GetOption<bool>("AutoFish.StopOnCombat", true);
        sCfg.maxMinutes      = sConfigMgr->GetOption<uint32>("AutoFish.MaxMinutes", 0);
        sCfg.minFreeSlots    = sConfigMgr->GetOption<uint32>("AutoFish.MinFreeBagSlots", 1);
        sCfg.announceCatches = sConfigMgr->GetOption<bool>("AutoFish.AnnounceCatches", false);

        // the bobber only stays hookable for FISHING_BOBBER_READY_TIME seconds
        uint32 const maxReaction = (FISHING_BOBBER_READY_TIME - 1) * IN_MILLISECONDS;
        if (sCfg.reactionMax > maxReaction)
            sCfg.reactionMax = maxReaction;
        if (sCfg.reactionMin > sCfg.reactionMax)
            sCfg.reactionMin = sCfg.reactionMax;
        if (sCfg.recastMin > sCfg.recastMax)
            sCfg.recastMin = sCfg.recastMax;
    }
};

class AutoFishPlayerScript : public PlayerScript
{
public:
    AutoFishPlayerScript() : PlayerScript("AutoFishPlayerScript", { PLAYERHOOK_ON_UPDATE }) { }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!sCfg.enabled || !player->IsInWorld())
            return;

        AutoFishState* st = player->CustomData.Get<AutoFishState>("mod-autofish");

        // bots, and players whose client just disconnected: hands off
        if (!IsRealPlayer(player))
        {
            if (st)
                st->active = false;
            return;
        }

        if (!st || !st->active)
        {
            // start-on-cast: the player threw a bobber by hand
            if (!sCfg.startOnCast || (st && st->optedOut))
                return;
            if (!player->GetGuidValue(UNIT_FIELD_CHANNEL_OBJECT).IsGameObject())
                return;
            GameObject* bobber = GetBobber(player);
            Spell* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            if (!bobber || !channel || !Allowed(player) || !GetPole(player))
                return;
            if (!st)
                st = GetState(player);
            if (st->active)
                return;
            Start(player, st, channel->GetSpellInfo()->Id);
            return;
        }

        st->tickAcc += diff;
        if (st->tickAcc < TICK_MS)
            return;
        st->tickAcc = 0;

        Update(player, st);
    }
};

class AutoFishCommandScript : public CommandScript
{
public:
    AutoFishCommandScript() : CommandScript("AutoFishCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable autofishTable =
        {
            { "on",     HandleOn,     SEC_PLAYER, Console::No },
            { "off",    HandleOff,    SEC_PLAYER, Console::No },
            { "status", HandleStatus, SEC_PLAYER, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "autofish", autofishTable },
        };
        return commandTable;
    }

    static bool HandleOn(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;
        if (!Allowed(player))
        {
            handler->SendSysMessage("AutoFish is not available on this account.");
            return true;
        }
        AutoFishState* st = GetState(player);
        st->optedOut = false;
        if (st->active)
        {
            handler->SendSysMessage("AutoFish is already running.");
            return true;
        }
        if (!GetPole(player))
        {
            handler->SendSysMessage("Equip a fishing pole first.");
            return true;
        }
        Spell* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        uint32 spellId = (channel && GetBobber(player)) ? channel->GetSpellInfo()->Id : FindFishingSpell(player);
        if (!spellId)
        {
            handler->SendSysMessage("You don't know Fishing.");
            return true;
        }
        Start(player, st, spellId);
        return true;
    }

    static bool HandleOff(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;
        AutoFishState* st = GetState(player);
        st->optedOut = true;
        if (st->active)
            Stop(player, st, "turned off");
        else
            handler->SendSysMessage("AutoFish won't start on your casts until you type .autofish on");
        return true;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;
        AutoFishState* st = GetState(player);
        std::string msg = std::string("AutoFish: ") + (st->active ? "running" : "idle")
            + (st->optedOut ? " (start-on-cast off)" : "")
            + ", start-on-cast " + (sCfg.startOnCast ? "on" : "off");
        if (st->active)
            msg += ", " + std::to_string(st->catches) + " catches / " + std::to_string(st->casts) + " casts";
        handler->SendSysMessage(msg);
        return true;
    }
};

void Addmod_autofishScripts()
{
    new AutoFishWorldScript();
    new AutoFishPlayerScript();
    new AutoFishCommandScript();
}
