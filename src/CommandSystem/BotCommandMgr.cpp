/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BotCommandMgr.h"
#include "AdaptiveBotAI.h"
#include "Chat.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

BotCommandMgrPlayerScript::BotCommandMgrPlayerScript() : PlayerScript("BotCommandMgrPlayerScript") {}

std::vector<AdaptiveBotAI*> BotCommandMgrPlayerScript::CollectBotGroup(Player* player)
{
    std::vector<AdaptiveBotAI*> botGroup;
    if (!player)
        return botGroup;

    std::lock_guard<std::mutex> lock(AdaptiveBotAI::s_botRegistryMutex);
    auto it = AdaptiveBotAI::s_masterBotRegistry.find(player->GetGUID());
    if (it != AdaptiveBotAI::s_masterBotRegistry.end())
        botGroup = it->second;

    return botGroup;
}

void BotCommandMgrPlayerScript::OnChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg)
{
    ProcessBotCommand(player, msg);
}

void BotCommandMgrPlayerScript::OnChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg, Group* /*group*/)
{
    ProcessBotCommand(player, msg);
}

bool BotCommandMgrPlayerScript::ProcessBotCommand(Player* player, std::string const& msg)
{
    if (!player || msg.empty())
        return false;

    // 指令前缀检查（支持半角 .bot 与全角 。bot，大小写不敏感）
    std::string lowerMsg = msg;
    std::transform(lowerMsg.begin(), lowerMsg.end(), lowerMsg.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string prefix = ".bot";
    size_t pos = lowerMsg.find(prefix);
    if (pos == std::string::npos)
    {
        prefix = "。bot";
        pos = lowerMsg.find(prefix);
        if (pos == std::string::npos)
            return false;
    }

    std::string sub = lowerMsg.substr(pos + prefix.length());
    // 去除前导空格
    size_t firstChar = sub.find_first_not_of(" \t\r\n");
    if (firstChar == std::string::npos)
        return false;
    sub = sub.substr(firstChar);

    if (sub == "assemble" || sub == "集合")
    {
        HandleAssemble(player);
        return true;
    }
    if (sub == "disband" || sub == "解散")
    {
        HandleDisband(player);
        return true;
    }
    if (sub == "rest" || sub == "休息")
    {
        HandleRest(player);
        return true;
    }
    if (sub == "stack" || sub == "密集" || sub == "集合阵")
    {
        HandleFormation(player, BotFormationType::STACK);
        return true;
    }
    if (sub == "fan" || sub == "扇形" || sub == "弧形")
    {
        HandleFormation(player, BotFormationType::FAN);
        return true;
    }
    if (sub == "spread" || sub == "分散" || sub == "散开")
    {
        HandleFormation(player, BotFormationType::SPREAD);
        return true;
    }

    return false;
}

void BotCommandMgrPlayerScript::HandleAssemble(Player* player)
{
    std::vector<AdaptiveBotAI*> const botGroup = CollectBotGroup(player);
    if (botGroup.empty())
        return;

    float const masterX = player->GetPositionX();
    float const masterY = player->GetPositionY();
    float const masterZ = player->GetPositionZ();
    float const masterO = player->GetOrientation();
    float const spreadRadius = 2.5f;
    uint32 const count = static_cast<uint32>(botGroup.size());

    for (uint32 i = 0; i < count; ++i)
    {
        AdaptiveBotAI* bot = botGroup[i];
        Creature* botCreature = bot ? bot->GetBotCreature() : nullptr;
        if (!botCreature || !botCreature->IsAlive() || botCreature->GetMap() != player->GetMap())
            continue;

        float const angleOffset = static_cast<float>(M_PI) + (static_cast<float>(i) - (count - 1) / 2.0f) * 0.5f;
        float const targetAngle = masterO + angleOffset;

        float const destX = masterX + spreadRadius * std::cos(targetAngle);
        float const destY = masterY + spreadRadius * std::sin(targetAngle);
        float const destZ = masterZ;

        bot->isResting = false;
        botCreature->CombatStop(true);
        botCreature->GetMotionMaster()->Clear();
        bot->apfMoveUpdateTimer = 0;

        botCreature->NearTeleportTo(destX, destY, destZ, masterO);
        botCreature->GetMotionMaster()->MoveFollow(player, 2.5f, player->GetAngle(botCreature));
    }

    ChatHandler(player->GetSession()).PSendSysMessage("【随从调度】全员已强制传送集合至您身边。");
}

void BotCommandMgrPlayerScript::HandleDisband(Player* player)
{
    std::vector<AdaptiveBotAI*> const botGroup = CollectBotGroup(player);
    if (botGroup.empty())
        return;

    for (AdaptiveBotAI* bot : botGroup)
    {
        if (!bot)
            continue;

        Creature* botCreature = bot->GetBotCreature();
        if (!botCreature)
            continue;

        bot->isResting = false;
        bot->UnregisterFromMaster();
        bot->masterGuid.Clear();
        botCreature->CombatStop(true);
        botCreature->GetMotionMaster()->Clear();
        botCreature->RestoreFaction();

        float homeX, homeY, homeZ, homeO;
        botCreature->GetHomePosition(homeX, homeY, homeZ, homeO);
        botCreature->NearTeleportTo(homeX, homeY, homeZ, homeO);
        botCreature->GetMotionMaster()->MoveIdle();

        if (botCreature->GetCreatureTemplate())
            botCreature->SetLevel(botCreature->GetCreatureTemplate()->minlevel);
        botCreature->SetHealth(botCreature->GetMaxHealth());

        if (!bot->guardianGuid.IsEmpty())
        {
            if (Creature* guardian = ObjectAccessor::GetCreature(*botCreature, bot->guardianGuid))
                guardian->DespawnOrUnsummon();
            bot->guardianGuid.Clear();
        }
    }

    ChatHandler(player->GetSession()).PSendSysMessage("【随从调度】冒险队伍已解散，随从已传送回所属驻地。");
}

void BotCommandMgrPlayerScript::HandleRest(Player* player)
{
    std::vector<AdaptiveBotAI*> const botGroup = CollectBotGroup(player);
    if (botGroup.empty())
        return;

    bool anyResting = false;
    for (AdaptiveBotAI* bot : botGroup)
    {
        if (bot && bot->isResting)
        {
            anyResting = true;
            break;
        }
    }

    bool const newRestState = !anyResting;

    for (AdaptiveBotAI* bot : botGroup)
    {
        if (!bot)
            continue;

        Creature* botCreature = bot->GetBotCreature();
        if (!botCreature || !botCreature->IsAlive() || botCreature->GetMap() != player->GetMap())
            continue;

        bot->isResting = newRestState;
        if (newRestState)
        {
            botCreature->CombatStop(true);
            botCreature->GetMotionMaster()->Clear();
            botCreature->HandleEmoteCommand(EMOTE_STATE_SIT);
        }
        else
        {
            botCreature->HandleEmoteCommand(EMOTE_STATE_STAND);
            botCreature->GetMotionMaster()->MoveFollow(player, 2.5f, player->GetAngle(botCreature));
        }
    }

    if (newRestState)
        ChatHandler(player->GetSession()).PSendSysMessage("【随从调度】全队进入就地休息状态，锁死索敌并休整。");
    else
        ChatHandler(player->GetSession()).PSendSysMessage("【随从调度】全队已解除休息，恢复待命作战状态。");
}

void BotCommandMgrPlayerScript::HandleFormation(Player* player, BotFormationType formation)
{
    std::vector<AdaptiveBotAI*> const botGroup = CollectBotGroup(player);
    if (botGroup.empty())
        return;

    float const masterX = player->GetPositionX();
    float const masterY = player->GetPositionY();
    float const masterZ = player->GetPositionZ();
    float const masterO = player->GetOrientation();
    uint32 const count = static_cast<uint32>(botGroup.size());

    for (uint32 i = 0; i < count; ++i)
    {
        AdaptiveBotAI* bot = botGroup[i];
        Creature* botCreature = bot ? bot->GetBotCreature() : nullptr;
        if (!botCreature || !botCreature->IsAlive() || botCreature->GetMap() != player->GetMap())
            continue;

        if (botCreature->HasUnitState(UNIT_STATE_CASTING | UNIT_STATE_CHARGING))
            continue;

        bot->isResting = false;
        float destX = masterX;
        float destY = masterY;

        switch (formation)
        {
            case BotFormationType::STACK:
            {
                // 密集集合阵：指挥官正后方 2.0 码，微幅随机离散
                float const offsetAngle = masterO + static_cast<float>(M_PI);
                float const jitter = (static_cast<float>(i % 3) - 1.0f) * 0.4f;
                destX = masterX + 2.0f * std::cos(offsetAngle) + jitter;
                destY = masterY + 2.0f * std::sin(offsetAngle) + jitter;
                break;
            }
            case BotFormationType::FAN:
            {
                // 弧形推进阵：指挥官正面 12 码处展开 120 度扇面
                float const sweepAngle = static_cast<float>(2.0 * M_PI / 3.0); // 120度
                float const startAngle = masterO - (sweepAngle * 0.5f);
                float const step = (count > 1) ? (sweepAngle / (count - 1)) : 0.0f;
                float const targetAngle = startAngle + step * i;
                destX = masterX + 12.0f * std::cos(targetAngle);
                destY = masterY + 12.0f * std::sin(targetAngle);
                break;
            }
            case BotFormationType::SPREAD:
            {
                // 极限分散阵：指挥官后方 180 度双层同心弧（偶数内圈 8 码，奇数外圈 16 码）
                float const sweepAngle = static_cast<float>(M_PI);
                float const startAngle = masterO + static_cast<float>(M_PI * 0.5);
                float const step = (count > 1) ? (sweepAngle / (count - 1)) : 0.0f;
                float const targetAngle = startAngle + step * i;
                float const radius = (i % 2 == 0) ? 8.0f : 16.0f;
                destX = masterX + radius * std::cos(targetAngle);
                destY = masterY + radius * std::sin(targetAngle);
                break;
            }
        }

        botCreature->GetMotionMaster()->MovePoint(1001, destX, destY, masterZ);
    }

    switch (formation)
    {
        case BotFormationType::STACK:
            ChatHandler(player->GetSession()).PSendSysMessage("【随从调度】阵型切换：【密集集合阵】（集中承伤与团补）。");
            break;
        case BotFormationType::FAN:
            ChatHandler(player->GetSession()).PSendSysMessage("【随从调度】阵型切换：【弧形推进阵】（正面迎敌，避侧后顺劈）。");
            break;
        case BotFormationType::SPREAD:
            ChatHandler(player->GetSession()).PSendSysMessage("【随从调度】阵型切换：【极限分散阵】（双层交错，防点名范围伤害）。");
            break;
    }
}

void AddSC_BotCommandMgr()
{
    new BotCommandMgrPlayerScript();
}
