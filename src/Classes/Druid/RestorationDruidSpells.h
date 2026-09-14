/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef MOD_ADAPTIVE_BOT_RESTORATION_DRUID_SPELLS_H
#define MOD_ADAPTIVE_BOT_RESTORATION_DRUID_SPELLS_H

#include "Define.h"

// =============================================================================
// 恢复德鲁伊 3.3.5a 最高等级法术 / 天赋 / 雕文 ID 清单
// 说明：
//   1. 全部 ID 取 3.3.5a (WotLK) 最终 80 级版本；
//      低等级随从由 AdaptiveBotAI::GetAppropriateRank 依据 DBC SpellLevel
//      沿 GetPrevRankSpell 链自动降阶，严禁在此登记降阶法术。
//   2. 天赋 / 特权技能必须在专精的 GetTalentSpellMinLevel 中登记最低解锁等级
//      (3.3.5a 天赋法术 DBC SpellLevel 多为 0，无法自动降阶)。
// =============================================================================
namespace RestorationDruidSpells
{
    // ---- 核心治疗与形态 ----
    constexpr uint32 TREE_OF_LIFE          = 33891; // 生命之树形态 (50级天赋, 提升治疗受疗与护甲, 锁定非治疗法术)
    constexpr uint32 LIFEBLOOM             = 48451; // 生命之绽 Rank 3 (64级基础法术, 瞬发, 可叠3层, 绽放回血回蓝)
    constexpr uint32 REJUVENATION          = 48441; // 回春术 Rank 15 (瞬发主力 HoT)
    constexpr uint32 REGROWTH              = 48443; // 愈合 Rank 12 (直接治疗 + 持续 HoT)
    constexpr uint32 NOURISH               = 50464; // 滋养 Rank 1 (80级, 目标每持一个 HoT 提升 20% 治疗量)
    constexpr uint32 SWIFTMEND             = 18562; // 迅捷治愈 (40级天赋, 瞬发爆发, 15s CD)
    constexpr uint32 WILD_GROWTH           = 53251; // 野性成长 Rank 4 (60级天赋, 智能多目标群抬, 6s CD)
    constexpr uint32 HEALING_TOUCH         = 48378; // 治疗之触 Rank 15 (生读大加, 专供自然迅捷秒抬)

    // ---- 战略爆发与自保 ----
    constexpr uint32 NATURES_SWIFTNESS     = 17116; // 自然迅捷 (30级天赋, Off-GCD 使下个自然法术瞬发, 2m CD)
    constexpr uint32 INNERVATE             = 29166; // 激活 (40级, 目标高额回蓝, 3m CD)
    constexpr uint32 BARKSPIN              = 22812; // 树皮术 (自保 20% 减伤, 瞬发)

    // ---- 团队增益与驱散 ----
    constexpr uint32 MARK_OF_THE_WILD      = 48469; // 野性印记 Rank 9 (爪子)
    constexpr uint32 REMOVE_CURSE          = 2782;  // 驱除诅咒 (仅解诅咒)
    constexpr uint32 ABOLISH_POISON        = 2893;  // 驱毒术 (仅解中毒)

    // ---- 被动天赋补偿与雕文 (NPC 无天赋树, 由 AddAura 静态补偿) ----
    constexpr uint32 INTENSITY             = 17108; // 强烈 Rank 3 (15级, 施法中保持 50% 精神回蓝)
    constexpr uint32 MASTER_SHAPESHIFTER   = 48412; // 兽性大师 Rank 2 (树形态治疗 +4%)
    constexpr uint32 EMPOWERED_TOUCH       = 33879; // 强化之触 Rank 2 (滋养增效)
    constexpr uint32 EMPOWERED_REJUV       = 33886; // 强化回春术 Rank 5 (HoT 效果 +10%)
    constexpr uint32 GIFT_OF_EARTHMOTHER   = 51183; // 大地母亲的赐福 Rank 5 (法术急速)
    constexpr uint32 GLYPH_OF_SWIFTMEND    = 54825; // 迅捷治愈雕文 (迅捷治愈不再吞噬回春/愈合, 核心必带)
    constexpr uint32 GLYPH_OF_WILD_GROWTH  = 62970; // 野性成长雕文 (目标数量 +1, 达 6 目标)
    constexpr uint32 GLYPH_OF_RAPID_REJUV  = 71013; // 快速回春雕文 (急速使回春跳得更快)
}

#endif // MOD_ADAPTIVE_BOT_RESTORATION_DRUID_SPELLS_H
