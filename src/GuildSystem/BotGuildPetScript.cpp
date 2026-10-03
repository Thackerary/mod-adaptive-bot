/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BotGuildPetScript.h"
#include "BotCommandMgr.h"
#include "BotGuildEscrowMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "GameTime.h"
#include "GossipDef.h"
#include "Item.h"
#include "PetAI.h"
#include "Player.h"
#include <unordered_map>
#include <string>

namespace
{
    enum BotGuildPetAction : uint32
    {
        ACTION_TACTICAL_MENU     = 1,
        ACTION_BILLING_MENU      = 2,
        ACTION_CLAIM_SUPPLY      = 3,
        ACTION_BACK_TO_MAIN      = 4,
		ACTION_SUMMON_MAILBOX    = 5, // 热砂财阀特权：随身便携邮箱
		
        ACTION_TACTICAL_ASSEMBLE = 101,
        ACTION_TACTICAL_DISBAND  = 102,
        ACTION_TACTICAL_REST     = 103,
        ACTION_TACTICAL_STACK    = 104,
        ACTION_TACTICAL_FAN      = 105,
        ACTION_TACTICAL_SPREAD   = 106,
        ACTION_TACTICAL_REVIVE   = 107, // 战地急救唤醒

        ACTION_BILLING_SETTLE    = 201
    };
	// 内存独立时钟记录（免侵入 EscrowMgr 数据库表结构）
    static std::unordered_map<ObjectGuid, uint64> s_silverCovenantSupplyTime; // 银色盟约 30 分钟晶水计时
    static std::unordered_map<ObjectGuid, uint64> s_steamwheedleMailboxTime;  // 热砂便携邮箱 10 分钟 CD 计时
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
		case ACTION_SUMMON_MAILBOX:
            HandleSummonMailbox(player, creature);
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
        case ACTION_TACTICAL_REVIVE:
            // 急救结果由核心层自行下发战报（战斗门禁 / 无阵亡 / 成功三种回执），
            // 此处直接返回战术号令菜单，便于团长连续整补后再下令列阵。
            sBotGuildEscrowMgr->ReviveDeadBots(player);
            ShowTacticalCommands(player, creature);
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
    // 动态显示补给文案
    if (guildId == GUILD_SILVER_COVENANT)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【领取魔枢给养】每 30 分钟免费获取魔法晶水与点心。", GOSSIP_SENDER_MAIN, ACTION_CLAIM_SUPPLY);
    else
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【领取公会行军补给】领取每日专属战地配给物资。", GOSSIP_SENDER_MAIN, ACTION_CLAIM_SUPPLY);
	
	// 热砂财阀专属：随身便携邮箱终端
    if (guildId == GUILD_STEAMWHEEDLE_CARTEL)
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "【商业特权】部署热砂传讯便携邮箱（冷却 10 分钟）。", GOSSIP_SENDER_MAIN, ACTION_SUMMON_MAILBOX);
	
    SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
}

void BotGuildPetScript::ShowTacticalCommands(Player* player, Creature* creature)
{
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【集合】全员瞬移集合至我身边。", GOSSIP_SENDER_MAIN, ACTION_TACTICAL_ASSEMBLE);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【解散】解散队伍并遣返全部随从。", GOSSIP_SENDER_MAIN, ACTION_TACTICAL_DISBAND);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "【战地急救】唤醒重塑战死随从（以50%生命法力归队）。", GOSSIP_SENDER_MAIN, ACTION_TACTICAL_REVIVE);
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
	uint8 const guildId = sBotGuildEscrowMgr->GetPlayerGuildId(player->GetGUID());
    if (guildId == GUILD_NONE)
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【行军补给】您尚未加入任何公会，无法享受公会后勤补给。|r");
        ShowPetMainMenu(player, creature);
        return;
    }
	uint64 const now = static_cast<uint64>(GameTime::GetGameTime().count());
    // -------------------------------------------------------------------------
    // 分支 1：达拉然银色盟约（30 分钟独立晶水给养）
    // -------------------------------------------------------------------------
    if (guildId == GUILD_SILVER_COVENANT)
    {
        auto it = s_silverCovenantSupplyTime.find(player->GetGUID());
        if (it != s_silverCovenantSupplyTime.end() && now < (it->second + 1800))
        {
            uint32 const remainMin = static_cast<uint32>((it->second + 1800 - now + 59) / 60);
            if (player->GetSession())
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff8000【魔枢给养】达拉然魔枢给养正在调配中，还需等待 {} 分钟方可再次申领。|r", remainMin);
            ShowPetMainMenu(player, creature);
            return;
        }

        uint32 const waterId = (player->GetLevel() >= 75) ? 43523 : 33445; // 魔法冰川之水 / 魔法甘露之水
        uint32 const foodId  = (player->GetLevel() >= 75) ? 43518 : 33449; // 魔法甘露点心 / 魔法羊角面包

        bool const wOk = player->AddItem(waterId, 20);
        bool const fOk = player->AddItem(foodId, 20);

        if (!wOk || !fOk)
        {
            if (wOk) player->DestroyItemCount(waterId, 20, true);
            if (fOk) player->DestroyItemCount(foodId, 20, true);

            if (player->GetSession())
                ChatHandler(player->GetSession()).PSendSysMessage("|cffff0000【魔枢给养】背包空间不足，请清理出至少 2 个空位。|r");
            ShowPetMainMenu(player, creature);
            return;
        }

        s_silverCovenantSupplyTime[player->GetGUID()] = now;
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage("【魔枢给养】已成功领取达拉然魔法晶水与点心各 20 份！");
        ShowPetMainMenu(player, creature);
        return;
    }
	
	// -------------------------------------------------------------------------
    // 分支 2：其余 9 大公会专属每日配给（20 小时宽限期门禁）
    // -------------------------------------------------------------------------
    if (!sBotGuildEscrowMgr->CanClaimDailySupply(player->GetGUID()))
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【行军补给】今日公会专属配给物资已领取完毕，请明日再来。|r");
        ShowPetMainMenu(player, creature);
        return;
    }
	bool success = false;
    std::string supplyName = "";
    uint32 grantedItem1 = 0, count1 = 0;
    uint32 grantedItem2 = 0, count2 = 0;
	
	switch (guildId)
    {
        case GUILD_EXPLORERS_LEAGUE: // 探险者协会：古代抗性合剂 x1 (+50全抗)
            grantedItem1 = 43548; count1 = 1;
            success = player->AddItem(grantedItem1, count1);
            supplyName = "古代抗性合剂 x1";
            break;

        case GUILD_SUNREAVERS: // 夺日者议会：夺日者魔能宝石 x3
            grantedItem1 = 34062; count1 = 3;
            success = player->AddItem(grantedItem1, count1);
            supplyName = "夺日者魔能宝石 x3";
            break;

        case GUILD_UNDERBELLY_SYNDICATE: // 下水道黑市：走私物资箱 x1
            grantedItem1 = 44700; count1 = 1;
            success = player->AddItem(grantedItem1, count1);
            supplyName = "黑市走私物资箱 x1";
            break;

        case GUILD_WARSONG_OFFENSIVE: // 战歌远征队：战歌战旗 x1 (防背包唯一性冲突)
            if (player->HasItemCount(38309, 1))
            {
                if (player->GetSession())
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cffff8000【行军补给】您的背包中已持有一面战歌战旗，请使用后再行申领。|r");
                ShowPetMainMenu(player, creature);
                return;
            }
            grantedItem1 = 38309; count1 = 1;
            success = player->AddItem(grantedItem1, count1);
            supplyName = "战歌战旗 x1";
            break;

        case GUILD_STEAMWHEEDLE_CARTEL: // 热砂财阀：地精速效爆雷 x5
            grantedItem1 = 41112; count1 = 5;
            success = player->AddItem(grantedItem1, count1);
            supplyName = "地精强效速效爆雷 x5";
            break;

        default: // 军情七处、死亡猎手、银色北伐军、塞纳里奥：自适应等级战斗药水包
        {
            SupplyKit const kit = GetSupplyKitForLevel(player->GetLevel());
            grantedItem1 = kit.healPotion; count1 = kit.count;
            grantedItem2 = kit.manaPotion; count2 = kit.count;
            bool const hOk = player->AddItem(grantedItem1, count1);
            bool const mOk = player->AddItem(grantedItem2, count2);
            success = (hOk && mOk);
            supplyName = "强化战斗药水配给包（治疗/法力各5瓶）";
            break;
        }
    }
	
	if (!success)
    {
        // 失败差量回滚防刷
        if (grantedItem1 && count1) player->DestroyItemCount(grantedItem1, count1, true);
        if (grantedItem2 && count2) player->DestroyItemCount(grantedItem2, count2, true);

        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【行军补给】背包空间不足，请清理背包后重试。|r");
        ShowPetMainMenu(player, creature);
        return;
    }
	
	sBotGuildEscrowMgr->RecordDailySupplyClaim(player->GetGUID());
	
	if (player->GetSession())
        ChatHandler(player->GetSession()).PSendSysMessage("【公会配给】已成功申领专属物资：{}！", supplyName);

    ShowPetMainMenu(player, creature);
}

void BotGuildPetScript::HandleSummonMailbox(Player* player, Creature* creature)
{
    uint64 const now = static_cast<uint64>(GameTime::GetGameTime().count());
    auto it = s_steamwheedleMailboxTime.find(player->GetGUID());

    if (it != s_steamwheedleMailboxTime.end() && now < (it->second + 600))
    {
        uint32 const remainMin = static_cast<uint32>((it->second + 600 - now + 59) / 60);
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff8000【便携邮箱】热砂传讯便携邮箱正在冷却中，还需等待 {} 分钟。|r", remainMin);
        ShowPetMainMenu(player, creature);
        return;
    }

    // 释放工程学随身便携邮箱法术 (Spell 54710: MOLL-E，持续 10 分钟)
    player->CastSpell(player, 54710, true);
    s_steamwheedleMailboxTime[player->GetGUID()] = now;

    if (player->GetSession())
        ChatHandler(player->GetSession()).PSendSysMessage("【商业特权】热砂随身便携邮箱已成功展开，持续 10 分钟。");

    CloseGossipMenuFor(player);
}

void AddSC_BotGuildPetScript()
{
    new BotGuildPetScript();
}
