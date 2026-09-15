/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef _AFFLICTION_WARLOCK_SPELLS_H
#define _AFFLICTION_WARLOCK_SPELLS_H

#include <cstdint>

// =============================================================================
// 痛苦术士 (Affliction Warlock) 3.3.5a 法术与天赋常量表
// -----------------------------------------------------------------------------
// 注：所有在 GetTalentSpellMinLevel 中登记的法术 ID 均为「最高 Rank」ID，
//     运行时统一交由 AdaptiveBotAI::GetAppropriateRank(id, true) 做等级降阶。
// =============================================================================
namespace AfflictionWarlockSpells
{
    // =========================================================================
    // 1. 核心 DoT 与暗影打击
    // =========================================================================
    constexpr uint32 HAUNT                  = 59164;  // 鬼影缠身 (60级纯天赋, 8s CD, 1.5s读条, +20%暗影DoT增伤)
    constexpr uint32 UNSTABLE_AFFLICTION    = 47843;  // 痛苦无常 Rank 5 (50级纯天赋, 15s DoT)
    constexpr uint32 CORRUPTION             = 47813;  // 腐蚀术 Rank 10 (基础核心暗影DoT)
    constexpr uint32 CURSE_OF_AGONY         = 47864;  // 痛苦诅咒 Rank 9 (24s暗影DoT)
    constexpr uint32 SHADOW_BOLT            = 47809;  // 暗影箭 Rank 13 (平稳期填充与叠暗影之拥)
    constexpr uint32 DRAIN_SOUL             = 47855;  // 吸取灵魂 Rank 6 (斩杀期核心通道, <25% 4倍伤害)
    constexpr uint32 SEED_OF_CORRUPTION     = 47836;  // 腐蚀之种 Rank 3 (多目标AoE DoT)

    // =========================================================================
    // 2. 续航、自保与仇恨控制
    // =========================================================================
    constexpr uint32 LIFE_TAP               = 57946;  // 生命分流 Rank 8 (扣血补蓝并触发雕文SP)
    constexpr uint32 DEATH_COIL             = 47860;  // 死亡缠绕 Rank 5 (瞬发恐惧自保并回血, 2m CD)
    constexpr uint32 SOULSHATTER            = 29858;  // 灵魂碎裂 (降低50%仇恨, 5m CD)
    constexpr uint32 DEMON_ARMOR            = 47889;  // 恶魔护甲 Rank 8 (常驻自保护甲与法伤/受疗增益)
    constexpr uint32 FEL_ARMOR              = 47893;  // 邪甲术 Rank 4 (核心输出护甲, 精神转法伤)

    // =========================================================================
    // 3. 核心被动与雕文光环补偿 (随从无天赋树，需在 ApplyPassiveTalents 手工注入)
    // =========================================================================
    constexpr uint32 DEATHS_EMBRACE         = 47199;  // 死亡之拥 Rank 3 (目标<25%吸取灵魂伤害提高400%)
    constexpr uint32 SHADOW_EMBRACE         = 32394;  // 暗影之拥 Rank 5 (暗影伤害加成Debuff)
    constexpr uint32 EVERLASTING_AFFLICTION = 47205;  // 永恒痛苦 Rank 5 (鬼影与吸取灵魂刷新腐蚀术)
    constexpr uint32 ERADICATION            = 47197;  // 灭绝 Rank 3 (腐蚀术跳数几率触发急速提升)
    constexpr uint32 PANDEMIC               = 58435;  // 传染 Rank 1 (痛苦无常与腐蚀术可以暴击)
    constexpr uint32 GLYPH_OF_QUICK_DECAY   = 70678;  // 急速凋零雕文 (急速降低腐蚀术跳数间隔)
    constexpr uint32 GLYPH_OF_LIFE_TAP      = 56218;  // 生命分流雕文 (分流后提供SP增益 63321)
    constexpr uint32 AURA_GLYPH_OF_LIFE_TAP = 63321;  // 生命分流雕文触发光环
    constexpr uint32 GLYPH_OF_HAUNT         = 56224;  // 鬼影缠身雕文 (+3%鬼影增伤)
}

#endif // _AFFLICTION_WARLOCK_SPELLS_H
