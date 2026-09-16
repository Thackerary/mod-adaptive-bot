/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

// =============================================================================
// 暗影牧师 (Shadow Priest) 3.3.5a 法术与天赋常量定义
// 说明：
//   - 所有 ID 统一采用各技能最高 Rank (80 级可达)，运行时由基类
//     GetAppropriateRank() 自动降阶至随从当前等级可用的最高 Rank。
//   - 纯天赋类技能在 DBC 中 SpellLevel = 0，调用 GetAppropriateRank(id, true)
//     并需在 GetTalentSpellMinLevel() 中显式注册最低解锁等级。
// =============================================================================
namespace ShadowPriestSpells
{
    // =========================================================================
    // 1. 核心暗影打击与 DoT
    // =========================================================================
    constexpr uint32 MIND_FLAY           = 48156;  // 精神鞭笞 Rank 6  (20级纯天赋, 3s 引导通道)
    constexpr uint32 VAMPIRIC_TOUCH      = 48160;  // 吸血鬼之触 Rank 5 (50级纯天赋, 1.5s 读条, 15s DoT + 全团回蓝)
    constexpr uint32 DEVOURING_PLAGUE    = 48300;  // 噬灵瘟疫 Rank 9  (瞬发暗影疾病 DoT, 24s)
    constexpr uint32 SHADOW_WORD_PAIN    = 48125;  // 暗言术：痛 Rank 12 (瞬发核心暗影 DoT)
    constexpr uint32 MIND_BLAST          = 48127;  // 心灵震爆 Rank 12 (1.5s 读条爆发直伤, 8s CD)
    constexpr uint32 SHADOW_WORD_DEATH   = 48158;  // 暗言术：灭 Rank 4  (瞬发斩杀/移动填充, 12s CD)

    // =========================================================================
    // 2. 姿态、自保与回蓝大招
    // =========================================================================
    constexpr uint32 SHADOWFORM          = 15473;  // 暗影形态 (40级纯天赋, +15% 暗伤, -15% 承伤, DoT 享受急速)
    constexpr uint32 VAMPIRIC_EMBRACE    = 15286;  // 吸血鬼之拥 (30级纯天赋, 30m 自身常驻光环, 暗伤转化为治疗)
    constexpr uint32 DISPERSION          = 47585;  // 消散 (60级纯天赋, 90% 减伤 + 回蓝, 2m CD)
    constexpr uint32 SHADOWFIEND         = 34433;  // 暗影恶魔 (召唤暗影魔回蓝, 3m CD)
    constexpr uint32 POWER_WORD_SHIELD   = 48066;  // 真言术：盾 Rank 14 (紧急自保吸收盾)
    constexpr uint32 INNER_FIRE          = 48168;  // 心灵之火 Rank 9 (常驻护甲与法伤增益)
    constexpr uint32 PRAYER_OF_FORTITUDE = 48162;  // 坚韧祷言 Rank 4 (全团耐力增益)
    constexpr uint32 SHADOW_PROTECTION   = 48170;  // 暗影防护祷言 Rank 2 (暗抗增益)

    // =========================================================================
    // 3. 触发光环、被动天赋与雕文光环补偿
    // =========================================================================
    constexpr uint32 AURA_WEAKENED_SOUL  = 6788;   // 虚弱灵魂光环
    constexpr uint32 AURA_SHADOW_WEAVING = 15258;  // 暗影交织触发光环 (最高叠 5 层, 每层 +2% 暗伤)
    constexpr uint32 SHADOW_WEAVING      = 15258;  // 暗影交织天赋 Rank 3 (100% 几率叠加, 被动根源注入用)
    constexpr uint32 PAIN_AND_SUFFERING  = 47582;  // 苦修与磨难 Rank 3 (精神鞭笞 100% 刷新目标身上的暗言术：痛)
    constexpr uint32 DARKNESS            = 15308;  // 黑暗 Rank 5 (暗影法术伤害 +10%)
    constexpr uint32 SHADOW_POWER        = 33220;  // 暗影能量 Rank 5 (暗影法术暴击伤害加成 +100%)
    constexpr uint32 MISERY              = 33195;  // 悲惨 Rank 3 (命中后目标受法术命中 +3%, 提高自身法强收益)
    constexpr uint32 GLYPH_OF_SHADOW     = 55689;  // 暗影雕文 (暗影形态下非持续性暗影暴击提升精神 30% 法强)
    constexpr uint32 GLYPH_OF_MIND_FLAY  = 55687;  // 精神鞭笞雕文 (目标有暗言术：痛时鞭笞伤害 +10%)
    constexpr uint32 GLYPH_OF_DISPERSION = 63229;  // 消散雕文 (消散冷却缩短 45s)
}
