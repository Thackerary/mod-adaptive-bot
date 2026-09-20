/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef _COMBAT_ROGUE_SPELLS_H
#define _COMBAT_ROGUE_SPELLS_H

#include "Define.h"

// =============================================================================
// 战斗潜行者 (Combat Rogue) 3.3.5a 法术常量表
// =============================================================================
namespace CombatRogueSpells
{
    // -------------------------------------------------------------------------
    // 姿态、战术大招与核心打击
    // -------------------------------------------------------------------------
    constexpr uint32 STEALTH             = 1784;   // 潜行 (脱战常驻)
    constexpr uint32 SINISTER_STRIKE     = 48638;  // 邪恶攻击 Rank 12 (核心产星打击)
    constexpr uint32 SLICE_AND_DICE      = 6774;   // 切割 Rank 2 (攻速核心增益, 100% 维持)
    constexpr uint32 EVISCERATE          = 48668;  // 刺骨 Rank 12 (主力物理终结技)
    constexpr uint32 RUPTURE             = 48672;  // 割裂 Rank 8 (物理流血终结技)
    constexpr uint32 BLADE_FLURRY        = 13877;  // 剑刃乱舞 (30 级纯天赋, 攻速+20%/攻击溅射)
    constexpr uint32 ADRENALINE_RUSH     = 13750;  // 冲动 (40 级纯天赋, 回能速度+100%)
    constexpr uint32 KILLING_SPREE       = 51690;  // 杀戮盛宴 (60 级纯天赋, 飞斩增伤)
    constexpr uint32 TRICKS_OF_THE_TRADE = 57934;  // 嫁祸诀窍 (75 级解锁, 仇恨转移 + 增伤)
    constexpr uint32 VANISH              = 26889;  // 消失 Rank 3 (强行进潜清仇恨)
    constexpr uint32 EVASION             = 26669;  // 闪避 Rank 2 (濒死 50% 闪避)
    constexpr uint32 CLOAK_OF_SHADOWS    = 31224;  // 暗影斗篷 (魔法免疫与解除)
    constexpr uint32 KICK                = 38768;  // 脚踢 Rank 5 (Off-GCD 打断, 接入基类压秒仲裁)
    constexpr uint32 DEADLY_POISON       = 57973;  // 致命毒药 Rank 9 (副手命中模拟注入)
    constexpr uint32 INSTANT_POISON      = 57965;  // 速效毒药 Rank 9 (主手命中模拟注入)

    // -------------------------------------------------------------------------
    // 触发光环、被动天赋与雕文补偿
    // -------------------------------------------------------------------------
    constexpr uint32 AURA_STEALTH        = 1784;   // 潜行光环
    constexpr uint32 AURA_KILLING_SPREE  = 51690;  // 杀戮盛宴引导/增益光环
    constexpr uint32 AURA_ADRENALINE_RUSH = 13750; // 冲动光环
    constexpr uint32 AURA_BLADE_FLURRY   = 13877;  // 剑刃乱舞光环
    constexpr uint32 COMBAT_POTENCY      = 35551;  // 战斗潜能 Rank 5 (副手命中几率回复 15 能量)
    constexpr uint32 HACK_AND_SLASH      = 13964;  // 砍击与劈砍 Rank 5 (剑/斧命中几率额外攻击)
    constexpr uint32 DUAL_WIELD_SPEC     = 13807;  // 双武器专精 Rank 5 (副手武器伤害 +50%)
    constexpr uint32 VITALITY            = 61329;  // 活力 Rank 3 (能量恢复速度提高 12%)
    constexpr uint32 PREY_ON_THE_WEAK    = 51689;  // 欺凌弱小 Rank 5 (目标血量低于自身时暴伤 +20%)
    constexpr uint32 SAVAGE_COMBAT       = 58415;  // 野蛮战斗 Rank 2 (中毒目标受物理伤害 +4%, 攻强 +4%)
    constexpr uint32 SURPRISE_ATTACKS    = 32910;  // 猝不及防 (终结技不可招架闪避, 技能伤害 +10%)
    constexpr uint32 GLYPH_OF_SINISTER_STRIKE = 56801; // 邪恶攻击雕文 (暴击额外加 1 星)
    constexpr uint32 GLYPH_OF_KILLING_SPREE   = 63252; // 杀戮盛宴雕文 (冷却缩短 45s -> 75s)
    constexpr uint32 GLYPH_OF_RUPTURE         = 56802; // 割裂雕文 (割裂持续时间延长 4s)
}

#endif // _COMBAT_ROGUE_SPELLS_H
