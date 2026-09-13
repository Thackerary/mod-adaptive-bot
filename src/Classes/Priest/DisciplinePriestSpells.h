/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

namespace DisciplinePriestSpells
{
    // =========================================================================
    // 1. 核心治疗与护盾 (Core Healing & Shielding)
    // 下列 ID 统一采用 3.3.5a 最高等级，实际投放时 GetAppropriateRank 会依
    // 随从当前等级自动向下取可用等级，低等级段可平滑降级，不会空放。
    // =========================================================================
    constexpr uint32 POWER_WORD_SHIELD      = 48066; // 真言术：盾 Rank 14 (预铺核心，瞬发吸收)
    constexpr uint32 WEAKENED_SOUL          = 6788;  // 灵魂虚弱 (15 秒内禁止再次获得真言术：盾)
    constexpr uint32 PENANCE                = 53007; // 苦修 (2 秒 3 段通道，单体 HPS 最高)
    constexpr uint32 FLASH_HEAL             = 48071; // 快速治疗 Rank 11 (1.5 秒读条，主力填充)
    constexpr uint32 GREATER_HEAL           = 48063; // 强效治疗术 Rank 9 (3 秒读条，蓝耗偏高)
    constexpr uint32 RENEW                  = 48068; // 恢复 Rank 14 (瞬发 HoT，可跑位施放)
    constexpr uint32 PRAYER_OF_MENDING      = 48113; // 愈合祷言 Rank 5 (瞬发智能弹跳)
    constexpr uint32 PRAYER_OF_HEALING      = 48072; // 治疗祷言 Rank 7 (群体急救)
    constexpr uint32 HOLY_NOVA              = 48078; // 神圣新星 Rank 9 (瞬发群体小治疗)
    constexpr uint32 BINDING_HEAL           = 48120; // 联结治疗 Rank 2 (自疗 + 目标治疗)
    constexpr uint32 DESPERATE_PRAYER       = 48173; // 绝望祷言 (神圣系天赋，低等级过渡自保)

    // =========================================================================
    // 2. 减伤、辅助与团队增益 (Defensive, Utility & Raid Buffs)
    // =========================================================================
    constexpr uint32 PAIN_SUPPRESSION       = 33206; // 痛苦压制 (40% 减伤，战略级急救底牌)
    constexpr uint32 POWER_INFUSION         = 10060; // 能量注入 (20% 急速 + 20% 耗蓝降低)
    constexpr uint32 INNER_FIRE             = 48168; // 心灵之火 Rank 11 (护甲 + 法强常驻)
    constexpr uint32 POWER_WORD_FORTITUDE   = 48161; // 真言术：韧 Rank 9
    constexpr uint32 DIVINE_SPIRIT          = 48073; // 神圣之灵 Rank 6 (仅对法力职业生效)
    constexpr uint32 SHADOW_PROTECTION      = 48169; // 暗影防护 (抗性兜底)
    constexpr uint32 FADE                   = 25429; // 渐隐 Rank 7 (3.3.5a 最高等级，降低仇恨脱身)
    constexpr uint32 INNER_FOCUS            = 14751; // 心灵专注 (下一发法术免费 + 25% 暴击)

    // =========================================================================
    // 3. 驱散体系 (Dispel)
    // =========================================================================
    constexpr uint32 DISPEL_MAGIC           = 527;   // 驱散魔法 Rank 2 (攻防两用)
    constexpr uint32 MASS_DISPEL            = 32375; // 群体驱散 (多人同时中负面时节省 GCD)
    constexpr uint32 ABOLISH_DISEASE        = 552;   // 祛病术 (疾病持续驱散)
    constexpr uint32 CURE_DISEASE           = 528;   // 驱除疾病 (单体疾病)

    // =========================================================================
    // 4. 续航 (Sustain)
    // =========================================================================
    constexpr uint32 SHADOWFIEND            = 34433; // 暗影魔 (召出后攻击回蓝，需敌对目标承载)
    constexpr uint32 HYMN_OF_HOPE           = 64901; // 希望圣歌 (群体回蓝，通道技，谨慎使用)

    // =========================================================================
    // 5. 被动天赋光环补偿 (Passive Talent Auras)
    // Creature 没有天赋树，必须主动注入等价被动光环，否则治疗量/续航全面亏模。
    // =========================================================================
    constexpr uint32 IMPROVED_POWER_WORD_SHIELD    = 14748; // 强化真言术：盾 (吸收量 +15%)
    constexpr uint32 IMPROVED_POWER_WORD_FORTITUDE = 14749; // 强化真言术：韧
    constexpr uint32 IMPROVED_INNER_FIRE           = 14747; // 强化心灵之火
    constexpr uint32 MEDITATION                    = 14777; // 冥想 (施法中保持 30% 法力回复)
    constexpr uint32 SPIRITUAL_GUIDANCE            = 15031; // 精神指引 (精神转化法强)

    // =========================================================================
    // 6. 核心雕文 (Major Glyphs)
    // =========================================================================
    constexpr uint32 GLYPH_OF_POWER_WORD_SHIELD = 63234; // 真言术：盾雕文 (盾被打破时治疗目标)
}
