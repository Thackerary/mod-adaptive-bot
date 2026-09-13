/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "ScriptMgr.h"

// 声明防战随从脚本注册入口（在 BotProtectionWarriorAI.cpp 底部实现）
void AddSC_bot_protection_warrior();
void AddSC_bot_protection_paladin();
void AddSC_bot_blood_death_knight();
void AddSC_bot_bear_druid();
void AddSC_bot_holy_paladin();

// 模块总入口：函数名必须严格匹配 "Add" + 模块目录名(连字符变下划线) + "Scripts"
void Addmod_adaptive_botScripts()
{
    AddSC_bot_protection_warrior();
    AddSC_bot_protection_paladin();
    AddSC_bot_blood_death_knight();
    AddSC_bot_bear_druid();
    AddSC_bot_holy_paladin();
}
