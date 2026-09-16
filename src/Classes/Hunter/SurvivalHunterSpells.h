/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef _SURVIVAL_HUNTER_SPELLS_H
#define _SURVIVAL_HUNTER_SPELLS_H

#include "Define.h"

namespace SurvivalHunterSpells
{
    // =========================================================================
    // 守护、核心射击与战术大招
    // =========================================================================
    constexpr uint32 AUTO_SHOT                = 75;     // 自动射击 (远程普攻核心通道)
    constexpr uint32 EXPLOSIVE_SHOT           = 60053;  // 爆炸射击 Rank 4 (纯天赋, 60级解锁, 6s CD, 核心火焰/自然重击)
    constexpr uint32 BLACK_ARROW              = 63670;  // 黑箭 Rank 6 (纯天赋, 50级解锁, 24s CD, 暗影DoT + 触发荷枪实弹)
    constexpr uint32 SERPENT_STING            = 49001;  // 毒蛇钉刺 Rank 12 (激活毒性钉刺增伤)
    constexpr uint32 KILL_SHOT                = 61006;  // 杀戮射击 Rank 3 (目标<20%强力物理斩杀, 15s/9s CD)
    constexpr uint32 AIMED_SHOT               = 49050;  // 瞄准射击 Rank 9 (瞬发远程物理填充, 10s CD)
    constexpr uint32 STEADY_SHOT              = 49052;  // 稳固射击 Rank 4 (读条填充射击)
    constexpr uint32 RAPID_FIRE               = 3045;   // 急速射击 (3m CD, +40%远程急速, Off-GCD)
    constexpr uint32 MISDIRECTION             = 34477;  // 误导 (30s CD, 仇恨转移给主坦)
    constexpr uint32 FEIGN_DEATH              = 5384;   // 假死 (30s CD, 脱困清仇恨)
    constexpr uint32 DETERRENCE               = 19263;  // 威慑 (90s CD, 100%招架/偏斜硬减伤)
    constexpr uint32 DISENGAGE                = 781;    // 逃脱 (25s CD, 盲区后跳拉开距离)
    constexpr uint32 ASPECT_OF_THE_DRAGONHAWK = 61846;  // 龙鹰守护 Rank 2 (常驻 AP 与闪避增益)
    constexpr uint32 ASPECT_OF_THE_VIPER      = 34074;  // 蝰蛇守护 (回蓝守护)

    // =========================================================================
    // 触发光环、被动天赋与雕文补偿
    // =========================================================================
    constexpr uint32 AURA_LOCK_AND_LOAD       = 56453;  // 荷枪实弹触发光环 (2次免费且无CD爆炸射击)
    constexpr uint32 AURA_EXPLOSIVE_SHOT      = 53301;  // 爆炸射击 DoT 光环
    constexpr uint32 AURA_SERPENT_STING       = 49001;  // 毒蛇钉刺 Debuff
    constexpr uint32 AURA_BLACK_ARROW         = 63670;  // 黑箭 Debuff
    constexpr uint32 LOCK_AND_LOAD            = 56344;  // 荷枪实弹 Rank 3 天赋根源 (黑箭/陷阱触发)
    constexpr uint32 SNIPER_TRAINING          = 53304;  // 狙击训练 Rank 3 (暴击与伤害提高 6%)
    constexpr uint32 HUNTING_PARTY            = 53292;  // 狩猎小队 Rank 3 (暴击使全团回蓝)
    constexpr uint32 NOXIOUS_STINGS           = 53243;  // 毒性钉刺 Rank 3 (目标带钉刺时全伤害 +3%)
    constexpr uint32 THRILL_OF_THE_HUNT       = 34499;  // 狩猎刺激 Rank 3 (技能暴击返还 40% 法力)
    constexpr uint32 TNT                      = 56337;  // T.N.T. Rank 3 (爆炸射击/黑箭暴击+6%, 伤害+6%)
    constexpr uint32 TRAP_MASTERY             = 63461;  // 陷阱掌握 Rank 3
    constexpr uint32 SURVIVAL_EXPERIENCE      = 53297;  // 生存本能 Rank 2 (敏捷与暴击加成)
    constexpr uint32 GLYPH_OF_EXPLOSIVE_SHOT  = 56832;  // 爆炸射击雕文 (爆炸射击暴击率提高 4%)
    constexpr uint32 GLYPH_OF_KILL_SHOT       = 56844;  // 杀戮射击雕文 (杀戮射击冷却时间缩短 6s)
    constexpr uint32 GLYPH_OF_SERPENT_STING   = 56828;  // 毒蛇钉刺雕文 (毒蛇钉刺持续时间延长 6s)
}

#endif // _SURVIVAL_HUNTER_SPELLS_H
