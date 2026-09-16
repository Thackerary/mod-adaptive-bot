/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

// =============================================================================
// 毁灭术士 (Destruction Warlock) 3.3.5a 法术常量表
// -----------------------------------------------------------------------------
// 命名约定:
//   - 技能本体 ID 统一使用最高 Rank 的 DBC ID, 运行时一律经 GetAppropriateRank 降阶;
//   - AURA_*   为「触发光环 / 目标 Debuff」本体;
//   - 其余大写名为「被动天赋根源 / 雕文被动」, 必须在 ApplyPassiveTalents 中注入。
// =============================================================================
namespace DestructionWarlockSpells
{
    // =========================================================================
    // 护甲、核心打击与战术技能
    // =========================================================================
    constexpr uint32 CHAOS_BOLT            = 59172;  // 混乱之箭 Rank 4 (纯天赋, 60级解锁, 穿透爆击直伤核心, 12s/10s CD)
    constexpr uint32 CONFLAGRATE           = 17962;  // 燃烧 (纯天赋, 40级解锁, 瞬发高爆直伤, 需献祭前置, 10s CD)
    constexpr uint32 IMMOLATE              = 47811;  // 献祭 Rank 11 (核心 1.5s 读条火焰 DoT, 燃烧机制的硬性前置)
    constexpr uint32 INCINERATE            = 47838;  // 烧尽 Rank 4 (64级解锁, 核心主力读条填充技)
    constexpr uint32 SHADOWBURN            = 47827;  // 暗影灼烧 Rank 10 (瞬发高伤填充/移动机动, 15s CD)
    constexpr uint32 SHADOW_BOLT           = 47809;  // 暗影箭 Rank 13 (低等级兜底填充, 并叠暗影与烈焰易伤)
    constexpr uint32 CURSE_OF_DOOM         = 47867;  // 末日灾祸 Rank 3 (60s 单发超高伤害诅咒, 限长线首领)
    constexpr uint32 CURSE_OF_AGONY        = 47864;  // 痛苦诅咒 Rank 8 (24s 常规平滑暗影 DoT)
    constexpr uint32 CORRUPTION            = 47813;  // 腐蚀术 Rank 10 (瞬发核心暗影 DoT)
    constexpr uint32 LIFE_TAP              = 57946;  // 生命分流 Rank 8 (异常资源消耗, 彻底旁路 CanCast 底层直放)
    constexpr uint32 FEL_ARMOR             = 47889;  // 邪甲术 Rank 4 (常驻提供法强与受疗增益)
    constexpr uint32 DEATH_COIL            = 47860;  // 死亡缠绕 Rank 5 (2m CD, 濒死回血与惊骇)
    constexpr uint32 SOUL_SHATTER          = 29858;  // 灵魂碎裂 (5m CD, 仇恨削减 50%)

    // =========================================================================
    // 触发光环与目标 Debuff
    // =========================================================================
    constexpr uint32 AURA_BACKDRAFT        = 54277;  // 爆燃触发光环 (3次充能, 毁灭系施法时间缩短 30%)
    constexpr uint32 AURA_GLYPH_OF_LIFE_TAP = 63321; // 生命分流雕文光环 (精神转化为法术强度)
    constexpr uint32 AURA_SHADOW_AND_FLAME = 17800;  // 暗影与烈焰 Debuff (目标受法术暴击率 +5%)
    constexpr uint32 AURA_IMMOLATE         = 47811;  // 献祭 DoT
    constexpr uint32 AURA_CONFLAGRATE      = 17962;  // 燃烧光环
    constexpr uint32 AURA_CURSE_OF_DOOM    = 47867;  // 末日灾祸 Debuff
    constexpr uint32 AURA_CURSE_OF_AGONY   = 47864;  // 痛苦诅咒 Debuff
    constexpr uint32 AURA_CORRUPTION       = 47813;  // 腐蚀术 Debuff

    // =========================================================================
    // 满阶被动天赋根源 (铁律 33: 严禁注入 DBC 默认 Rank 1 根源)
    // =========================================================================
    constexpr uint32 BACKDRAFT             = 47260;  // 爆燃 Rank 3 天赋根源 (燃烧触发极速施法)
    constexpr uint32 FIRE_AND_BRIMSTONE    = 47249;  // 硫磺与烈火 Rank 5 天赋根源 (献祭使烧尽伤害 +10%, 混乱之箭暴击 +25%)
    constexpr uint32 RUIN                  = 17959;  // 毁灭 Rank 5 (毁灭系法术暴击伤害 +100%)
    constexpr uint32 SHADOW_AND_FLAME      = 30293;  // 暗影与烈焰 Rank 5 (暗影箭/烧尽法伤加成 +20%, 目标暴击易伤)
    constexpr uint32 IMPROVED_IMMOLATE     = 17834;  // 强化献祭 Rank 3 (献祭伤害 +30%)
    constexpr uint32 EMBERSTORM            = 17958;  // 灰烬风暴 Rank 5 (火焰伤害 +10%, 烧尽读条时间缩短 10%)
    constexpr uint32 DESTRUCTIVE_REACH     = 17918;  // 毁灭延伸 Rank 2 (毁灭法术射程 +20%, 仇恨 -10%)
    constexpr uint32 BANE                  = 17789;  // 灾祸 Rank 5 (暗影箭/混乱之箭/献祭读条缩短 0.5s)
    constexpr uint32 BACKLASH              = 34939;  // 反冲 Rank 3 (暴击率 +3%)
    constexpr uint32 DEMONIC_AEGIS         = 30146;  // 恶魔庇护 Rank 3 (邪甲术效果提高 30%)

    // =========================================================================
    // 雕文补偿
    // =========================================================================
    constexpr uint32 GLYPH_OF_CONFLAGRATE  = 56235;  // 燃烧雕文 (燃烧不再吞噬献祭)
    constexpr uint32 GLYPH_OF_CHAOS_BOLT   = 56241;  // 混乱之箭雕文 (混乱之箭冷却缩短 2s)
    constexpr uint32 GLYPH_OF_INCINERATE   = 56242;  // 烧尽雕文 (烧尽伤害提高 5%)
    constexpr uint32 GLYPH_OF_LIFE_TAP     = 56226;  // 生命分流雕文 (被动法术根源, 分流时触发 63321 法强光环)
}
