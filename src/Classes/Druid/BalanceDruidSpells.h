/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

// =============================================================================
// 平衡德鲁伊 (Balance Druid) 3.3.5a 法术与天赋 ID 契约
// -----------------------------------------------------------------------------
// 说明：
//  1. 标注「纯天赋」的技能在 3.3.5a 中 DBC SpellLevel 恒为 0，GetAppropriateRank
//     不会因等级自动降阶。调用方必须同时在 GetTalentSpellMinLevel 中登记等级门槛，
//     并以 GetAppropriateRank(id, true) 解析，否则低等级枭兽会直接搓出高阶天赋。
//  2. 标注「基础法术」的技能走 GetAppropriateRank(id, false) 的 DBC Rank 降阶通道。
//  3. 被动光环 ID 用于 ApplyPassiveTalents 的根源注入，弥补 NPC 无天赋树缺陷。
// =============================================================================
namespace BalanceDruidSpells
{
    // =========================================================================
    // 1. 核心自然/奥术打击与 DoT
    // =========================================================================
    constexpr uint32 WRATH                   = 48461;  // 愤怒 Rank 12 (自然直伤填充，暴击触发月蚀)
    constexpr uint32 STARFIRE                = 48465;  // 星火术 Rank 10 (奥术长读条重击，暴击触发日蚀)
    constexpr uint32 MOONFIRE                = 48463;  // 月火术 Rank 14 (瞬发奥术直伤 + DoT，跑位期亦可挂)
    constexpr uint32 INSECT_SWARM            = 48468;  // 虫群 Rank 7 (瞬发自然 DoT，纯天赋，30 级解锁)
    constexpr uint32 STARFALL                = 53201;  // 星辰坠落 Rank 2 (纯天赋，60 级解锁；10s 独立群伤光环，1 分钟 CD)
    constexpr uint32 TYPHOON                 = 61384;  // 台风 Rank 5 (纯天赋，50 级解锁；正面击退减速，20s CD)

    // =========================================================================
    // 2. 姿态、自保与增益续航
    // =========================================================================
    constexpr uint32 MOONKIN_FORM            = 24858;  // 枭兽形态 (纯天赋，40 级解锁；+400% 护甲，+5% 全团法系暴击)
    constexpr uint32 BARKSKIN                = 22812;  // 树皮术 (Off-GCD 20% 减伤，1 分钟 CD，任何形态可用)
    constexpr uint32 INNERVATE               = 29166;  // 激活 (3 分钟 CD，回蓝大招，任何形态可用)
    constexpr uint32 MARK_OF_THE_WILD        = 48469;  // 野性赐福/爪子 Rank 9 (脱战全团常驻增益)
    constexpr uint32 THORNS                  = 48476;  // 荆棘术 Rank 8 (反伤增益，脱战维持)

    // =========================================================================
    // 3. 触发光环与被动天赋补偿
    // =========================================================================
    constexpr uint32 AURA_ECLIPSE_LUNAR      = 48518;  // 月蚀触发光环 (+40% 星火术暴击率)，由愤怒暴击触发
    constexpr uint32 AURA_ECLIPSE_SOLAR      = 48517;  // 日蚀触发光环 (+40% 愤怒伤害)，由星火术暴击触发
    constexpr uint32 ECLIPSE                 = 48525;  // 日月蚀 Rank 3 (暴击 100% 触发双蚀的核心被动根源，必须根源注入)
    constexpr uint32 NATURES_GRACE           = 61346;  // 自然之赐 Rank 3 (法术暴击 100% 触发降低读条 0.5s)
    constexpr uint32 EARTH_AND_MOON          = 48511;  // 大地与月亮 Rank 3 (13% 法术易伤 Debuff + 3% 暴击)
    constexpr uint32 MOONKIN_AURA            = 24907;  // 枭兽光环 (随枭兽形态派生的全团暴击增益)
    constexpr uint32 GLYPH_OF_STARFIRE       = 54844;  // 星火雕文 (星火术延长月火持续时间 3s，最多叠加 9s)
    constexpr uint32 GLYPH_OF_STARFALL       = 54828;  // 星辰坠落雕文 (冷却缩短 30s)
    constexpr uint32 GLYPH_OF_INSECT_SWARM   = 54830;  // 虫群雕文 (虫群伤害 +30%，但不再降低目标命中)
    constexpr uint32 IMPROVED_INSECT_SWARM   = 57848;  // 强化虫群 Rank 3 (目标有虫群时愤怒伤害 +3%，有月火时星火暴击 +3%)
}
