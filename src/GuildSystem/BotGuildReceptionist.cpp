/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BotGuildReceptionist.h"
#include "BotGuildEscrowMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "GossipDef.h"
#include "Player.h"
#include <string>

namespace
{
    enum BotGuildReceptionistAction : uint32
    {
        ACTION_BROWSE_GUILDS     = 1,
        ACTION_REPAY_DEBT        = 2,
        ACTION_REISSUE_PET       = 3,
        ACTION_LEAVE_CONFIRM     = 4,
        ACTION_LEAVE_CONFIRM_YES = 5,
        ACTION_BACK_TO_MAIN      = 6,

        ACTION_GUILD_DETAIL_BASE = 100,  // 100 + guildId -> 公会详情
        ACTION_JOIN_GUILD_BASE   = 200   // 200 + guildId -> 确认加入
    };

    bool IsFactionEligible(Player* player, GuildPetConfig const* cfg)
    {
        if (!player || !cfg)
            return false;

        if (cfg->isAllianceOnly)
            return player->GetTeamId() == TEAM_ALLIANCE;

        if (cfg->isHordeOnly)
            return player->GetTeamId() == TEAM_HORDE;

        return true;
    }
}

BotGuildReceptionistScript::BotGuildReceptionistScript() : CreatureScript("npc_bot_guild_receptionist") {}

bool BotGuildReceptionistScript::OnGossipHello(Player* player, Creature* creature)
{
    if (!player || !creature || !creature->IsAlive())
        return false;

    ShowMainMenu(player, creature);
    return true;
}

bool BotGuildReceptionistScript::OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action)
{
    if (!player || !creature)
        return false;

    ClearGossipMenuFor(player);

    // 先判高位偏移，再判低位指令：加入（200+）必须先于详情（100+）命中，
    // 否则 action=201 之类会被详情分支的 `>= 100` 提前吞掉。
    if (action >= ACTION_JOIN_GUILD_BASE)
    {
        HandleJoinGuild(player, creature, static_cast<uint8>(action - ACTION_JOIN_GUILD_BASE));
        return true;
    }

    if (action >= ACTION_GUILD_DETAIL_BASE)
    {
        ShowGuildDetail(player, creature, static_cast<uint8>(action - ACTION_GUILD_DETAIL_BASE));
        return true;
    }

    switch (action)
    {
        case ACTION_BROWSE_GUILDS:
            ShowGuildListMenu(player, creature);
            break;
        case ACTION_REPAY_DEBT:
            sBotGuildEscrowMgr->ClearBankruptcy(player);
            CloseGossipMenuFor(player);
            break;
        case ACTION_REISSUE_PET:
            HandleReissuePet(player, creature);
            break;
        case ACTION_LEAVE_CONFIRM:
            ShowLeaveConfirmMenu(player, creature);
            break;
        case ACTION_LEAVE_CONFIRM_YES:
            HandleLeaveGuild(player, creature);
            break;
        case ACTION_BACK_TO_MAIN:
            ShowMainMenu(player, creature);
            break;
        default:
            CloseGossipMenuFor(player);
            break;
    }

    return true;
}

void BotGuildReceptionistScript::ShowMainMenu(Player* player, Creature* creature)
{
    if (!player || !creature)
        return;

    ObjectGuid const guid = player->GetGUID();
    uint8 const guildId = sBotGuildEscrowMgr->GetPlayerGuildId(guid);
    bool const isBankrupt = sBotGuildEscrowMgr->IsBankrupt(guid);

    if (guildId == GUILD_NONE)
    {
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "我想浏览并申请加入冒险者公会。", GOSSIP_SENDER_MAIN, ACTION_BROWSE_GUILDS);
    }
    else
    {
        GuildPetConfig const* cfg = BotGuildEscrowMgr::GetGuildConfig(guildId);
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage("【公会前台】您当前的会籍：{}。",
                cfg ? cfg->name : "未知公会");

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "补发本公会专属通信使魔。", GOSSIP_SENDER_MAIN, ACTION_REISSUE_PET);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "办理退会手续。", GOSSIP_SENDER_MAIN, ACTION_LEAVE_CONFIRM);
    }

    if (isBankrupt)
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "【清偿欠款】解除公会失信封禁。", GOSSIP_SENDER_MAIN, ACTION_REPAY_DEBT);

    SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
}

void BotGuildReceptionistScript::ShowGuildListMenu(Player* player, Creature* creature)
{
    for (auto const& cfg : BotGuildEscrowMgr::GUILD_CONFIGS)
    {
        std::string label = cfg.name;
        if (cfg.isAllianceOnly)
            label += "（联盟）";
        else if (cfg.isHordeOnly)
            label += "（部落）";
        else
            label += "（中立）";

        if (!IsFactionEligible(player, &cfg))
            label += " —— 阵营不符";

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, label, GOSSIP_SENDER_MAIN,
            ACTION_GUILD_DETAIL_BASE + static_cast<uint32>(cfg.guildId));
    }

    AddGossipItemFor(player, GOSSIP_ICON_DOT, "返回。", GOSSIP_SENDER_MAIN, ACTION_BACK_TO_MAIN);
    SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
}

void BotGuildReceptionistScript::ShowGuildDetail(Player* player, Creature* creature, uint8 guildId)
{
    GuildPetConfig const* cfg = BotGuildEscrowMgr::GetGuildConfig(guildId);
    if (!cfg)
    {
        ShowMainMenu(player, creature);
        return;
    }

    if (!IsFactionEligible(player, cfg))
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【公会前台】{} 仅招募{}成员，您的阵营无法申请加入。|r",
                cfg->name, cfg->isAllianceOnly ? "联盟" : "部落");
    }
    else
    {
        if (player->GetSession())
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "【公会前台】{}：入会后可领取专属通信使魔，并享受随从雇佣 7.5 折内部津贴。", cfg->name);

            // 职业契合度前置提醒：非契合职业入会后无法为队伍激活本会战术光环。
            // 若不在入会前讲清，玩家会误以为「入会即得光环」而产生落差投诉。
            if (!sBotGuildEscrowMgr->IsPlayerClassAffiliated(guildId, player->getClass()))
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff8000【契合度警示】您的职业与本公会的战术风格不符。入会后仍可享受 7.5 折佣金津贴与每日行军补给，但您本人无法为队伍激活本公会的专属战术光环（需依赖本会正统随从提供）。|r");
            }
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "确认加入该公会。", GOSSIP_SENDER_MAIN,
            ACTION_JOIN_GUILD_BASE + static_cast<uint32>(guildId));
    }

    AddGossipItemFor(player, GOSSIP_ICON_DOT, "返回公会列表。", GOSSIP_SENDER_MAIN, ACTION_BROWSE_GUILDS);
    SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
}

void BotGuildReceptionistScript::HandleJoinGuild(Player* player, Creature* creature, uint8 guildId)
{
    GuildPetConfig const* cfg = BotGuildEscrowMgr::GetGuildConfig(guildId);
    if (!cfg)
    {
        ShowMainMenu(player, creature);
        return;
    }

    // SetPlayerGuild 内部已包含「阵营准入」与「重复会籍」双重校验，
    // 返回 false 即代表不满足入会条件，无需在此处重复判断分支细节。
    if (!sBotGuildEscrowMgr->SetPlayerGuild(player, guildId))
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【公会前台】入会手续办理失败：您已隶属于其他公会，或阵营不符合该公会的招募条件。|r");
        ShowMainMenu(player, creature);
        return;
    }

    if (!player->HasItemCount(cfg->itemId, 1))
        player->AddItem(cfg->itemId, 1);

    if (player->GetSession())
        ChatHandler(player->GetSession()).PSendSysMessage(
            "【公会前台】欢迎加入{}！专属通信使魔已放入您的背包，右键使用即可召唤并在野外随时调度本会随从！",
            cfg->name);

    creature->Whisper("愿我们的旗帜与你同行。", LANG_UNIVERSAL, player);
    ShowMainMenu(player, creature);
}

void BotGuildReceptionistScript::ShowLeaveConfirmMenu(Player* player, Creature* creature)
{
    if (player->GetSession())
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffff8000【公会前台】您确定要脱离会籍吗？退会后专属使魔将被注销回收，且无法再享受 7.5 折内部雇佣津贴！|r");

    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "我确认退会，请办理手续。", GOSSIP_SENDER_MAIN, ACTION_LEAVE_CONFIRM_YES);
    AddGossipItemFor(player, GOSSIP_ICON_DOT, "暂不退会，返回。", GOSSIP_SENDER_MAIN, ACTION_BACK_TO_MAIN);
    SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
}

void BotGuildReceptionistScript::HandleLeaveGuild(Player* player, Creature* creature)
{
    // LeavePlayerGuild 内部完整覆盖「随从未解散」「佣金未结清」两项门禁，
    // 以及使魔实体/光环/道具/法术四重回收，失败原因已由对方逐条回执。
    if (sBotGuildEscrowMgr->LeavePlayerGuild(player))
    {
        creature->Whisper("契约已终止，愿圣光照亮你前行的道路。", LANG_UNIVERSAL, player);
        CloseGossipMenuFor(player);
        return;
    }

    ShowMainMenu(player, creature);
}

void BotGuildReceptionistScript::HandleReissuePet(Player* player, Creature* creature)
{
    uint8 const guildId = sBotGuildEscrowMgr->GetPlayerGuildId(player->GetGUID());
    GuildPetConfig const* cfg = BotGuildEscrowMgr::GetGuildConfig(guildId);

    if (!cfg)
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【公会前台】您当前没有有效的会籍，无法补发通信使魔。|r");
        ShowMainMenu(player, creature);
        return;
    }

    if (player->HasItemCount(cfg->itemId, 1))
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage("【公会前台】您已持有本公会的通信使魔，无需重复补发。");
        ShowMainMenu(player, creature);
        return;
    }

    if (!player->AddItem(cfg->itemId, 1))
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【公会前台】您的背包空间不足，请整理背包后再次申领。|r");
        ShowMainMenu(player, creature);
        return;
    }

    if (player->GetSession())
        ChatHandler(player->GetSession()).PSendSysMessage(
            "【公会前台】{} 的通信使魔已重新放入您的背包，右键使用即可召唤。", cfg->name);

    ShowMainMenu(player, creature);
}

void AddSC_BotGuildReceptionist()
{
    new BotGuildReceptionistScript();
}
