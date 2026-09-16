/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

namespace ArmsWarriorSpells
{
    // =========================================================================
    // 1. 姿态、战吼与核心打击
    // =========================================================================
    constexpr uint32 BATTLE_STANCE          = 2457;   // 战斗姿态 (核心底座姿态)
    constexpr uint32 MORTAL_STRIKE          = 47486;  // 致死打击 Rank 8 (40级纯天赋, 6s CD, -50%受疗)
    constexpr uint32 BLADESTORM             = 46924;  // 利刃风暴 (60级纯天赋, 6s持续旋风, 1.5m CD)
    constexpr uint32 SWEEPING_STRIKES       = 12292;  // 横扫攻击 (30级纯天赋, 下5次近战攻击伤害溅射, 30s CD)
    constexpr uint32 OVERPOWER              = 7384;   // 压制 (血之气息触发或目标招架闪避后可用, 5怒气)
    constexpr uint32 REND                   = 47465;  // 撕裂 Rank 10 (核心物理流血 DoT, 维持血之气息)
    constexpr uint32 EXECUTE                = 47471;  // 斩杀 Rank 9 (猝死触发或 <20% 核心打击)
    constexpr uint32 SLAM                   = 47475;  // 猛击 Rank 8 (强化猛击后 0.5s 读条填充)
    constexpr uint32 HEROIC_STRIKE          = 47450;  // 英勇打击 Rank 13 (高怒气泄怒队列)
    constexpr uint32 CLEAVE                 = 47520;  // 顺劈斩 Rank 8 (多目标泄怒队列)
    constexpr uint32 CHARGE                 = 11578;  // 冲锋 Rank 3 (8~25码突进, 主宰天赋允许进战施放)
    constexpr uint32 SHATTERING_THROW       = 64382;  // 碎裂投掷 (破除无敌/降低20%护甲, 5m CD)
    constexpr uint32 BATTLE_SHOUT           = 47436;  // 战斗怒吼 Rank 9 (攻强增益)
    constexpr uint32 BLOODRAGE              = 2687;   // 血性狂暴 (瞬间产怒 + 激怒状态)
    constexpr uint32 BERSERKER_RAGE         = 18499;  // 狂暴之怒 (解恐 + 激怒状态, 30s CD)
    constexpr uint32 ENRAGED_REGENERATION   = 55694;  // 狂暴回复 (激怒自保回血, 3m CD)
    constexpr uint32 RETALIATION            = 20230;  // 反击风暴 (近战被围攻反弹, 5m CD)

    // =========================================================================
    // 2. 触发光环、被动天赋与雕文补偿
    // =========================================================================
    constexpr uint32 AURA_TASTE_FOR_BLOOD   = 60503;  // 血之气息触发光环 (使压制可用)
    constexpr uint32 TASTE_FOR_BLOOD        = 56638;  // 血之气息 Rank 3 天赋根源 (撕裂跳数 100% 触发)
    constexpr uint32 AURA_SUDDEN_DEATH      = 52437;  // 猝死触发光环 (使斩杀可用且保留10怒)
    constexpr uint32 SUDDEN_DEATH           = 29725;  // 猝死 Rank 3 天赋根源 (近战命中 9% 几率触发)
    constexpr uint32 JUGGERNAUT             = 64976;  // 主宰 Rank 1 (战时可用冲锋, 冲锋后致死/猛击暴击+25%)
    constexpr uint32 IMPROVED_SLAM          = 12862;  // 强化猛击 Rank 2 (猛击读条时间减少 1.0s)
    constexpr uint32 DEEP_WOUNDS            = 12867;  // 重伤 Rank 3 (暴击触发 48% 武器伤害流血)
    constexpr uint32 TRAUMA                 = 46855;  // 创伤 Rank 2 (暴击提升目标所受流血伤害 30%)
    constexpr uint32 UNRELENTING_ASSAULT    = 46860;  // 无坚不摧 Rank 2 (压制冷却降低4s)
    constexpr uint32 TWO_HANDED_WEAPON_SPEC = 12714;  // 双手武器专精 Rank 5 (双手武器物理伤害提升 10%)
    constexpr uint32 GLYPH_OF_MORTAL_STRIKE = 58368;  // 致死打击雕文 (致死打击伤害 +10%)
    constexpr uint32 GLYPH_OF_REND          = 58386;  // 撕裂雕文 (撕裂持续时间延长 6s)
    constexpr uint32 GLYPH_OF_BLADESTORM    = 63324;  // 利刃风暴雕文 (利刃风暴冷却时间缩短 15s)
    constexpr uint32 GLYPH_OF_EXECUTION     = 58367;  // 斩杀雕文 (斩杀视为额外消耗 10 点怒气结算伤害)
}
