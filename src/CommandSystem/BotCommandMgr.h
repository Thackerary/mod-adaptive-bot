/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "ScriptMgr.h"
#include "ChatCommand.h"
#include "Player.h"
#include <vector>

class AdaptiveBotAI;

enum class BotFormationType
{
    STACK,   // 密集集合阵（分摊伤害、爆发团补）
    FAN,     // 弧形推进阵（120度扇形，避正面顺劈与龙尾扫击）
    SPREAD   // 极限分散阵（双层交错，防点名范围与连锁闪电）
};

class BotCommandScript : public CommandScript
{
public:
    BotCommandScript();

    Acore::ChatCommands::ChatCommandTable GetCommands() const override;

    // 指令路由包装器 (由 AzerothCore 命令分发器直接调用)
    static bool HandleAssemble(ChatHandler* handler);
    static bool HandleDisband(ChatHandler* handler);
    static bool HandleRest(ChatHandler* handler);
    static bool HandleStack(ChatHandler* handler);
    static bool HandleFan(ChatHandler* handler);
    static bool HandleSpread(ChatHandler* handler);

    // 战术核心执行逻辑
    static void DoAssemble(Player* player);
    static void DoDisband(Player* player);
    static void DoRest(Player* player);
    static void DoFormation(Player* player, BotFormationType formation);

    /// @brief 从集群总线中拉取当前指挥官名下的随从快照（持锁拷贝，出锁后安全遍历）。
    static std::vector<AdaptiveBotAI*> CollectBotGroup(Player* player);
};

void AddSC_BotCommandMgr();
