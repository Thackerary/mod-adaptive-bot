/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BotGuildPetScript.h"
#include "BotCommandMgr.h"
#include "BotGuildEscrowMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "GossipDef.h"
#include "Item.h"
#include "PetAI.h"
#include "Player.h"

namespace
{
    enum BotGuildPetAction : uint32
    {
        ACTION_TACTICAL_MENU     = 1,
        ACTION_BILLING_MENU      = 2,
        ACTION_CLAIM_SUPPLY      = 3,
        ACTION_BACK_TO_MAIN      = 4,

        ACTION_TACTICAL_ASSEMBLE = 101,
        ACTION_TACTICAL_DISBAND  = 102,
        ACTION_TACTICAL_REST     = 103,
        ACTION_TACTICAL_STACK    = 104,
        ACTION_TACTICAL_FAN      = 105,
        ACTION_TACTICAL_SPREAD   = 106,

        ACTION_BILLING_SETTLE    = 201
    };

    // 每日行军补给配方：按指挥官等级区间发放对应档次的实战药水
    struct SupplyKit
    {
        uint32 healPotion;
        uint32 manaPotion;
        uint32 count;
    };

    SupplyKit GetSupplyKitForLevel(uint8 level)
    {
        if (level >= 80)
            return { 33447, 33448, 5 }; // 符文治疗药水 / 符文法力药水
        if (level >= 60)
            return { 22829, 22832, 5 }; // 超级治疗药水 / 超级法力药水
        return { 13446, 13444, 5 };     // 极效治疗药水 / 极效法力药水
    }
}

BotGuildPetScript::BotGuildPetScript() : CreatureScript("npc_bot_guild_pet") {}

CreatureAI* BotGuildPetScript::GetAI(Creature* creature) const
{
    // 使魔实体本质是 Pet(MINI_PET)。此处必须显式返回原生 PetAI：
    // 若照搬普通 Gossip NPC 的做法（GetAI 返回 nullptr 交给引擎兜底），
    // 伴侣的跟随/保活行为会随 AI 被替换而丢失，使魔将僵在原地不再跟随指挥官。
    // 右键对话由 ScriptMgr 独立分发，与 AI 选择完全无关，互不干扰。
    return (creature && creature->IsPet()) ? new PetAI(creature) : nullptr;
}

bool BotGuildPetScript::OnGossipHello(Player* player, Creature* creature)
{
    if (!player || !creature)
        return false;

    // 归属鉴权：只有召唤该使魔的契约指挥官本人才可调出公会终端。
    if (creature->GetOwnerGUID() != player->GetGUID())
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage("使魔正在向其契约指挥官传递密信，不便与您交谈。");
        return true;
    }

    ShowPetMainMenu(player, creature);
    return true;
}

bool BotGuildPetScript::OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action)
{
    if (!player || !creature)
        return false;

    if (creature->GetOwnerGUID() != player->GetGUID())
    {
        CloseGossipMenuFor(player);
        return true;
    }

    ClearGossipMenuFor(player);

    switch (action)
    {
        case ACTION_TACTICAL_MENU:
            ShowTacticalCommands(player, creature);
            break;
        case ACTION_BILLING_MENU:
            ShowBillingStatus(player, creature);
            break;
        case ACTION_CLAIM_SUPPLY:
            HandleDailySupply(player, creature);
            break;
        case ACTION_BACK_TO_MAIN:
            ShowPetMainMenu(player, creature);
            break;

        case ACTION_TACTICAL_ASSEMBLE:
            BotCommandScript::DoAssemble(player);
            ShowTacticalCommands(player, creature);
            break;
        case ACTION_TACTICAL_DISBAND:
            // 解散链路已由 DoDisband 统一收口（尾款清算 -> 化身销毁 -> 本体唤醒 -> 归巢），
            // 解散后返回使魔主菜单：队伍已空，继续停留在战术号令菜单只会列出无效指令。
            BotCommandScript::DoDisband(player);
            ShowPetMainMenu(player, creature);
            break;
        case ACTION_TACTICAL_REST:
            BotCommandScript::DoRest(player);
            ShowTacticalCommands(player, creature);
            break;
        case ACTION_TACTICAL_STACK:
            BotCommandScript::DoFormation(player, BotFormationType::STACK);
            ShowTacticalCommands(player, creature);
            break;
        case ACTION_TACTICAL_FAN:
            BotCommandScript::DoFormation(player, BotFormationType::FAN);
            ShowTacticalCommands(player, creature);
            break;
        case ACTION_TACTICAL_SPREAD:
            BotCommandScript::DoFormation(player, BotFormationType::SPREAD);
            ShowTacticalCommands(player, creature);
            break;

        case ACTION_BILLING_SETTLE:
            HandleManualSettle(player, creature);
            break;

        default:
            CloseGossipMenuFor(player);
            break;
    }

    return true;
}

void BotGuildPetScript::ShowPetMainMenu(Player* player, Creature* creature)
{
    if (!player || !creature)
        return;

    uint8 const guildId = sBotGuildEscrowMgr->GetPlayerGuildId(player->GetGUID());
    if (guildId == GUILD_NONE)
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【公会使魔】您当前并未隶属于任何冒险者公会，请先前往主城公会前台办理入会手续。|r");
    }
    else
    {
        GuildPetConfig const* cfg = BotGuildEscrowMgr::GetGuildConfig(guildId);
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "【公会使魔】契约指挥官 {}，{} 听候调遣。", player->GetName(), cfg ? cfg->name : "公会");
    }

    AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "【战术号令中心】远程下达集合 / 解散 / 休息 / 阵型指令。", GOSSIP_SENDER_MAIN, ACTION_TACTICAL_MENU);
    AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "【佣金账单查询与结算】", GOSSIP_SENDER_MAIN, ACTION_BILLING_MENU);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【领取公会行军补给】", GOSSIP_SENDER_MAIN, ACTION_CLAIM_SUPPLY);

    SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
}

void BotGuildPetScript::ShowTacticalCommands(Player* player, Creature* creature)
{
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【集合】全员瞬移集合至我身边。", GOSSIP_SENDER_MAIN, ACTION_TACTICAL_ASSEMBLE);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【解散】解散队伍并遣返全部随从。", GOSSIP_SENDER_MAIN, ACTION_TACTICAL_DISBAND);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【休息】切换就地休息 / 恢复待命。", GOSSIP_SENDER_MAIN, ACTION_TACTICAL_REST);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【密集集合阵】集中承伤与团补。", GOSSIP_SENDER_MAIN, ACTION_TACTICAL_STACK);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【弧形推进阵】正面迎敌，避侧后顺劈。", GOSSIP_SENDER_MAIN, ACTION_TACTICAL_FAN);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【极限分散阵】双层交错，防点名范围伤害。", GOSSIP_SENDER_MAIN, ACTION_TACTICAL_SPREAD);
    AddGossipItemFor(player, GOSSIP_ICON_DOT, "返回。", GOSSIP_SENDER_MAIN, ACTION_BACK_TO_MAIN);

    SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
}

void BotGuildPetScript::ShowBillingStatus(Player* player, Creature* creature)
{
    uint32 pendingCopper = 0;
    uint32 killedCount = 0;
    uint32 graceRemainingMs = 0;
    bool inGracePeriod = false;

    bool const hasContract = sBotGuildEscrowMgr->GetContractSnapshot(
        player->GetGUID(), pendingCopper, killedCount, inGracePeriod, graceRemainingMs);

    if (!hasContract)
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage("【公会账单】您当前没有在途的雇佣契约。");
    }
    else if (pendingCopper == 0)
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage("【公会账单】{} 当前没有待结佣金，账目已清。", player->GetName());
    }
    else
    {
        uint32 const gold = pendingCopper / 10000;
        uint32 const silver = (pendingCopper % 10000) / 100;
        uint32 const copper = pendingCopper % 100;

        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "【公会账单】累计协助击杀 {} 个目标，待结佣金 {}金 {}银 {}铜。", killedCount, gold, silver, copper);

        if (inGracePeriod)
        {
            // 向上取整到分钟：剩余 1ms 也应显示为"仅剩 1 分钟"，不能显示 0 分钟造成误判。
            uint32 const minutesLeft = (graceRemainingMs + 59999) / 60000;
            if (player->GetSession())
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff0000【公会催缴】您当前处于欠费宽限期，剩余 {} 分钟，逾期将强制解散并列入失信黑名单！|r",
                    minutesLeft);
        }

        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "【立即结算】刷卡结清全部佣金。", GOSSIP_SENDER_MAIN, ACTION_BILLING_SETTLE);
    }

    AddGossipItemFor(player, GOSSIP_ICON_DOT, "返回。", GOSSIP_SENDER_MAIN, ACTION_BACK_TO_MAIN);
    SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
}

void BotGuildPetScript::HandleManualSettle(Player* player, Creature* creature)
{
    // SettleCurrentBill 内部已完成扣款、契约复位与账单回执的全流程；
    // 返回 false 仅代表"金币不足并已切入宽限期"，故此处只需补一条提示。
    if (!sBotGuildEscrowMgr->SettleCurrentBill(player, BILLING_REASON_MANUAL))
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【公会使魔】结算失败：背包金币不足，请尽快筹集资金以免触发信用制裁。|r");
    }

    ShowBillingStatus(player, creature);
}

void BotGuildPetScript::HandleDailySupply(Player* player, Creature* creature)
{
    if (!sBotGuildEscrowMgr->CanClaimDailySupply(player->GetGUID()))
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【行军补给】今日补给已领取，或您尚未加入任何冒险者公会。|r");
        ShowPetMainMenu(player, creature);
        return;
    }

    SupplyKit const kit = GetSupplyKitForLevel(player->GetLevel());

    // Player::AddItem 的返回类型是 bool（发放成功与否），并非 Item*，
    // 不能用于指针判空；此处按返回值逐个记录发放结果。
    bool const healGranted = player->AddItem(kit.healPotion, kit.count);
    bool const manaGranted = player->AddItem(kit.manaPotion, kit.count);

    // 背包空间不足时绝不记录领取时间戳：否则玩家会既没拿到物资、
    // 又白白消耗掉当天的领取资格。
    if (!healGranted || !manaGranted)
    {
        // 部分发放回滚：若治疗药水已入库而法力药水发放失败，
        // 不回滚会让玩家在「未消耗领取资格」的前提下白拿一份药水，
        // 反复触发即可无限刷取。此处按已发放量精确倒扣，保证零净收益。
        if (healGranted)
            player->DestroyItemCount(kit.healPotion, kit.count, true);
        if (manaGranted)
            player->DestroyItemCount(kit.manaPotion, kit.count, true);
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【行军补给】您的背包空间不足，请整理背包后重新领取。|r");
        ShowPetMainMenu(player, creature);
        return;
    }

    sBotGuildEscrowMgr->RecordDailySupplyClaim(player->GetGUID());

    if (player->GetSession())
        ChatHandler(player->GetSession()).PSendSysMessage(
            "【行军补给】已领取：治疗药水 x{}、法力药水 x{}。祝您远征顺利！", kit.count, kit.count);

    ShowPetMainMenu(player, creature);
}

void AddSC_BotGuildPetScript()
{
    new BotGuildPetScript();
}
