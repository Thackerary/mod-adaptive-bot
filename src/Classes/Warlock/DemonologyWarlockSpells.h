/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

namespace DemonologyWarlockSpells
{
    // =========================================================================
    // 护甲、核心诅咒、打击与战术大招
    // =========================================================================
    constexpr uint32 SHADOW_BOLT           = 47809;  // 暗影箭 Rank 13 (核心读条填充技, 叠暗影与烈焰易伤)
    constexpr uint32 INCINERATE            = 47838;  // 烧尽 Rank 4 (熔火之心触发消费技)
    constexpr uint32 SOUL_FIRE             = 47825;  // 灵魂之火 Rank 6 (灭杀斩杀期核心极速重击)
    constexpr uint32 IMMOLATE              = 47811;  // 献祭 Rank 11 (1.5s 读条核心火焰 DoT)
    constexpr uint32 CORRUPTION            = 47813;  // 腐蚀术 Rank 10 (瞬发核心暗影 DoT, 触发熔火之心)
    constexpr uint32 CURSE_OF_DOOM         = 47867;  // 末日灾祸 Rank 3 (60s 单发超高伤害诅咒, 限长线首领)
    constexpr uint32 CURSE_OF_AGONY        = 47864;  // 痛苦诅咒 Rank 8 (24s 常规平滑暗影 DoT)
    constexpr uint32 METAMORPHOSIS         = 47241;  // 恶魔变形 (纯天赋, 60级解锁, 3m/2.2m CD, +20%全伤害)
    constexpr uint32 IMMOLATION_AURA       = 50589;  // 献祭光环 (仅变身期间可用, 近身8码火焰 AoE, 30s CD, Off-GCD)
    constexpr uint32 DEMONIC_EMPOWERMENT   = 47193;  // 恶魔强化 (纯天赋, 50级解锁, 1m CD, 团队与恶魔增益)
    constexpr uint32 LIFE_TAP              = 57946;  // 生命分流 Rank 8 (异常资源消耗, 彻底旁路 CanCast 底层直放)
    constexpr uint32 FEL_ARMOR             = 47889;  // 邪甲术 Rank 4 (常驻提供法强与受疗增益)
    constexpr uint32 DEATH_COIL            = 47860;  // 死亡缠绕 Rank 5 (2m CD, 濒死回血与惊骇)
    constexpr uint32 SOUL_SHATTER          = 29858;  // 灵魂碎裂 (5m CD, 仇恨削减 50%)

    // =========================================================================
    // 触发光环、被动天赋与雕文补偿
    // =========================================================================
    constexpr uint32 AURA_DECIMATION       = 63167;  // 灭杀触发光环 (使灵魂之火读条缩短 40%)
    constexpr uint32 AURA_MOLTEN_CORE      = 47383;  // 熔火之心触发光环 (3次充能, 烧尽伤害+18%, 读条-30%)
    constexpr uint32 AURA_METAMORPHOSIS    = 47241;  // 恶魔变形光环
    constexpr uint32 AURA_GLYPH_OF_LIFE_TAP = 63321; // 生命分流雕文光环 (精神转化为法术强度)
    constexpr uint32 AURA_SHADOW_AND_FLAME = 17800;  // 暗影与烈焰 Debuff (目标受法术暴击率 +5%)
    constexpr uint32 AURA_IMMOLATE         = 47811;  // 献祭 DoT
    constexpr uint32 AURA_CORRUPTION       = 47813;  // 腐蚀术 DoT
    constexpr uint32 AURA_CURSE_OF_DOOM    = 47867;  // 末日灾祸 Debuff
    constexpr uint32 AURA_CURSE_OF_AGONY   = 47864;  // 痛苦诅咒 Debuff
    constexpr uint32 AURA_FEL_ARMOR        = 47889;  // 邪甲术增益
    constexpr uint32 DECIMATION            = 54118;  // 灭杀 Rank 2 天赋根源 (目标<=35%诱发极速魂火)
    constexpr uint32 MOLTEN_CORE           = 47247;  // 熔火之心 Rank 3 天赋根源 (腐蚀术跳数触发)
    constexpr uint32 DEMONIC_PACT          = 48090;  // 恶魔契约 Rank 5 天赋根源 (全团提供 10% 法强光环)
    constexpr uint32 DEMONIC_AEGIS         = 30146;  // 恶魔庇护 Rank 3 (邪甲术效果提高 30%)
    constexpr uint32 SHADOW_AND_FLAME      = 30293;  // 暗影与烈焰 Rank 5 (暗影箭/烧尽法伤加成+20%, 目标暴击易伤)
    constexpr uint32 RUIN                  = 17959;  // 毁灭 Rank 5 (毁灭系法术暴击伤害 +100%)
    constexpr uint32 IMPROVED_IMMOLATE     = 17834;  // 强化献祭 Rank 3 (献祭伤害 +30%)
    constexpr uint32 GLYPH_OF_LIFE_TAP     = 63321;  // 生命分流雕文 (生命分流激活法强)
    constexpr uint32 GLYPH_OF_METAMORPHOSIS = 56247; // 恶魔变形雕文 (变形持续时间延长 6s)
    constexpr uint32 GLYPH_OF_QUICK_DECAY  = 70669;  // 急速凋零雕文 (急速使腐蚀术周期缩短)
}
