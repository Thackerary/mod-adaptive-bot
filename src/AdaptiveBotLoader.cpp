/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "ScriptMgr.h"

// 声明防战随从脚本注册入口（在 BotProtectionWarriorAI.cpp 底部实现）
void AddSC_bot_protection_warrior();
void AddSC_bot_protection_paladin();
void AddSC_bot_blood_death_knight();
void AddSC_bot_frost_death_knight();
void AddSC_bot_unholy_death_knight();
void AddSC_bot_bear_druid();
void AddSC_bot_feral_cat_druid();
void AddSC_bot_holy_paladin();
void AddSC_bot_discipline_priest();
void AddSC_bot_holy_priest();
void AddSC_bot_restoration_shaman();
void AddSC_bot_enhancement_shaman();
void AddSC_bot_elemental_shaman();
void AddSC_bot_restoration_druid();
void AddSC_bot_marksmanship_hunter();
void AddSC_bot_survival_hunter();
void AddSC_bot_beast_mastery_hunter();
void AddSC_bot_assassination_rogue();
void AddSC_bot_subtlety_rogue();
void AddSC_bot_combat_rogue();
void AddSC_bot_arcane_mage();
void AddSC_bot_fire_mage();
void AddSC_bot_frost_mage();
void AddSC_bot_retribution_paladin();
void AddSC_bot_fury_warrior();
void AddSC_bot_arms_warrior();
void AddSC_bot_affliction_warlock();
void AddSC_bot_demonology_warlock();
void AddSC_bot_destruction_warlock();
void AddSC_bot_balance_druid();
void AddSC_bot_shadow_priest();

// 伴随型战斗护卫（Guardian/Pet）外观与脱战归位 AI
void AddSC_bot_guardian();

// 模块总入口：函数名必须严格匹配 "Add" + 模块目录名(连字符变下划线) + "Scripts"
void Addmod_adaptive_botScripts()
{
    AddSC_bot_protection_warrior();
    AddSC_bot_protection_paladin();
    AddSC_bot_blood_death_knight();
    AddSC_bot_frost_death_knight();
    AddSC_bot_unholy_death_knight();
    AddSC_bot_bear_druid();
    AddSC_bot_feral_cat_druid();
    AddSC_bot_holy_paladin();
    AddSC_bot_discipline_priest();
    AddSC_bot_holy_priest();
    AddSC_bot_restoration_shaman();
    AddSC_bot_enhancement_shaman();
    AddSC_bot_elemental_shaman();
    AddSC_bot_restoration_druid();
    AddSC_bot_marksmanship_hunter();
    AddSC_bot_survival_hunter();
    AddSC_bot_beast_mastery_hunter();
    AddSC_bot_assassination_rogue();
    AddSC_bot_subtlety_rogue();
    AddSC_bot_combat_rogue();
    AddSC_bot_arcane_mage();
    AddSC_bot_fire_mage();
    AddSC_bot_frost_mage();
    AddSC_bot_retribution_paladin();
    AddSC_bot_fury_warrior();
    AddSC_bot_arms_warrior();
    AddSC_bot_affliction_warlock();
    AddSC_bot_demonology_warlock();
    AddSC_bot_destruction_warlock();
    AddSC_bot_balance_druid();
    AddSC_bot_shadow_priest();
    AddSC_bot_guardian();
}
