/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "ScriptMgr.h"
#include "Player.h"
#include "Group.h"
#include <string>
#include <vector>

class AdaptiveBotAI;

enum class BotFormationType
{
    STACK,   // 密集集合阵（分摊伤害、爆发团补）
    FAN,     // 弧形推进阵（120度扇形，避正面顺劈与龙尾扫击）
    SPREAD   // 极限分散阵（双层交错，防点名范围与连锁闪电）
};

class BotCommandMgrPlayerScript : public PlayerScript
{
public:
    BotCommandMgrPlayerScript();

    void OnChat(Player* player, uint32 type, uint32 lang, std::string& msg) override;
    void OnChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override;

private:
    static bool ProcessBotCommand(Player* player, std::string const& msg);
    static void HandleAssemble(Player* player);
    static void HandleDisband(Player* player);
    static void HandleRest(Player* player);
    static void HandleFormation(Player* player, BotFormationType formation);

    /// @brief 从集群总线中拉取当前指挥官名下的随从快照（持锁拷贝，出锁后安全遍历）。
    static std::vector<AdaptiveBotAI*> CollectBotGroup(Player* player);
};

void AddSC_BotCommandMgr();
