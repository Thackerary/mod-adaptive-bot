/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BotCommandMgr.h"
#include "AdaptiveBotAI.h"
#include "Chat.h"
#include "Creature.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

using namespace Acore::ChatCommands;

BotCommandScript::BotCommandScript() : CommandScript("BotCommandScript") {}

ChatCommandTable BotCommandScript::GetCommands() const
{
    static ChatCommandTable botSubCommandTable =
    {
        { "assemble", HandleAssemble, SEC_PLAYER, Console::No },
        { "集合",     HandleAssemble, SEC_PLAYER, Console::No },
        { "disband",  HandleDisband,  SEC_PLAYER, Console::No },
        { "解散",     HandleDisband,  SEC_PLAYER, Console::No },
        { "rest",     HandleRest,     SEC_PLAYER, Console::No },
        { "休息",     HandleRest,     SEC_PLAYER, Console::No },
        { "stack",    HandleStack,    SEC_PLAYER, Console::No },
        { "密集",     HandleStack,    SEC_PLAYER, Console::No },
        { "集合阵",   HandleStack,    SEC_PLAYER, Console::No },
        { "fan",      HandleFan,      SEC_PLAYER, Console::No },
        { "扇形",     HandleFan,      SEC_PLAYER, Console::No },
        { "弧形",     HandleFan,      SEC_PLAYER, Console::No },
        { "spread",   HandleSpread,   SEC_PLAYER, Console::No },
        { "分散",     HandleSpread,   SEC_PLAYER, Console::No },
        { "散开",     HandleSpread,   SEC_PLAYER, Console::No }
    };

    static ChatCommandTable commandTable =
    {
        { "bot", botSubCommandTable }
    };

    return commandTable;
}

std::vector<AdaptiveBotAI*> BotCommandScript::CollectBotGroup(Player* player)
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


bool BotCommandScript::HandleAssemble(ChatHandler* handler)
{
    Player* player = handler->GetPlayer();
    if (!player)
        return false;

    DoAssemble(player);
    return true;
}

void BotCommandScript::DoAssemble(Player* player)
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
        bot->isHoldingFormation = false;
        // 强制起立：若随从正以坐姿休整，直接瞬移会让其保持蹲坐模型滑行到落点。
        botCreature->HandleEmoteCommand(EMOTE_STATE_STAND);
        botCreature->CombatStop(true);
        botCreature->GetMotionMaster()->Clear();
        bot->apfMoveUpdateTimer = 0;

        botCreature->NearTeleportTo(destX, destY, destZ, masterO);
        botCreature->GetMotionMaster()->MoveFollow(player, 2.5f, player->GetAngle(botCreature));
    }

    ChatHandler(player->GetSession()).PSendSysMessage("【随从调度】全员已强制传送集合至您身边。");
}

bool BotCommandScript::HandleDisband(ChatHandler* handler)
{
    Player* player = handler->GetPlayer();
    if (!player)
        return false;

    DoDisband(player);
    return true;
}

void BotCommandScript::DoDisband(Player* player)
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
        bot->isHoldingFormation = false;
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

bool BotCommandScript::HandleRest(ChatHandler* handler)
{
    Player* player = handler->GetPlayer();
    if (!player)
        return false;

    DoRest(player);
    return true;
}

void BotCommandScript::DoRest(Player* player)
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

        // 休息指令优先级高于阵型保持：必须在切入休整的同一时刻解除阵型锁，
        // 否则 rest 之后随从仍被 isHoldingFormation 挡在跟随巡检之外，
        // 解除休息时无法自动归队。
        bot->isHoldingFormation = false;
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

bool BotCommandScript::HandleStack(ChatHandler* handler)
{
    Player* player = handler->GetPlayer();
    if (!player)
        return false;

    DoFormation(player, BotFormationType::STACK);
    return true;
}

bool BotCommandScript::HandleFan(ChatHandler* handler)
{
    Player* player = handler->GetPlayer();
    if (!player)
        return false;

    DoFormation(player, BotFormationType::FAN);
    return true;
}

bool BotCommandScript::HandleSpread(ChatHandler* handler)
{
    Player* player = handler->GetPlayer();
    if (!player)
        return false;

    DoFormation(player, BotFormationType::SPREAD);
    return true;
}

void BotCommandScript::DoFormation(Player* player, BotFormationType formation)
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
        bot->isHoldingFormation = true;
        // 强制起立：坐姿模型执行 MovePoint 会表现为蹲坐滑行，
        // 必须在解除休息标记的同一时刻播放下站立表情。
        botCreature->HandleEmoteCommand(EMOTE_STATE_STAND);
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

        // 高度校准：直接沿用指挥官 Z 轴会让斜坡/楼梯上的阵型落点穿模或悬空，
        // 改由地图地表高度接口解算落点真实地面 Z，取不到有效地面时回退指挥官 Z。
        float destZ = masterZ;
        if (Map* map = player->GetMap())
        {
            float const groundZ = map->GetHeight(player->GetPhaseMask(), destX, destY, masterZ, true, 50.0f);
            if (groundZ > INVALID_HEIGHT)
                destZ = groundZ;
        }

        botCreature->GetMotionMaster()->MovePoint(1001, destX, destY, destZ);
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
    new BotCommandScript();
}
