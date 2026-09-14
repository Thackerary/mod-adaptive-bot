/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

namespace HolyPriestSpells
{
    // =========================================================================
    // 1. 核心治疗法术 (Core Healing)
    // 统一采用 3.3.5a 最高等级 ID，实际投放时 GetAppropriateRank 会依随从
    // 当前等级自动向下取可用等级，低等级段平滑降级，绝不空放。
    // =========================================================================
    constexpr uint32 CIRCLE_OF_HEALING   = 48089; // 治疗之环 Rank 7 (50级天赋，瞬发智能群抬，6s CD)
    constexpr uint32 GUARDIAN_SPIRIT     = 47788; // 守护之魂 (60级神圣终极天赋，40% 受疗加成 + 免死救急)
    constexpr uint32 DESPERATE_PRAYER    = 48173; // 绝望祷言 (20级天赋，瞬发自救底牌)
    constexpr uint32 PRAYER_OF_MENDING   = 48113; // 愈合祷言 Rank 5 (瞬发智能弹跳)
    constexpr uint32 PRAYER_OF_HEALING   = 48072; // 治疗祷言 Rank 7 (3 秒读条群体急救)
    constexpr uint32 FLASH_HEAL          = 48071; // 快速治疗 Rank 11 (1.5 秒读条，主力填充)
    constexpr uint32 GREATER_HEAL        = 48063; // 强效治疗术 Rank 9 (3 秒读条，高耗蓝兜底)
    constexpr uint32 BINDING_HEAL        = 48120; // 联结治疗 Rank 2 (自疗 + 目标治疗)
    constexpr uint32 RENEW               = 48068; // 恢复 Rank 14 (瞬发 HoT，可跑位施放)
    constexpr uint32 POWER_WORD_SHIELD   = 48066; // 真言术：盾 Rank 14 (濒死应急吸收)
    constexpr uint32 WEAKENED_SOUL       = 6788;  // 灵魂虚弱 (15 秒内禁止再次获得真言术：盾)
    constexpr uint32 HOLY_NOVA           = 48078; // 神圣新星 Rank 9 (瞬发群体小治疗)
    constexpr uint32 FADE                = 25429; // 渐隐术 Rank 7 (降低仇恨脱身)

    // =========================================================================
    // 2. 团队增益与驱散 (Raid Buffs & Dispel)
    // =========================================================================
    constexpr uint32 POWER_WORD_FORTITUDE  = 48161; // 真言术：韧 Rank 9
    constexpr uint32 DIVINE_SPIRIT         = 48073; // 神圣之灵 Rank 6 (仅对法力职业生效)
    constexpr uint32 INNER_FIRE            = 48168; // 心灵之火 Rank 11 (护甲 + 法强常驻)
    constexpr uint32 DISPEL_MAGIC          = 527;   // 驱散魔法 Rank 2
    constexpr uint32 MASS_DISPEL           = 32375; // 群体驱散 (多人同时中负面时节省 GCD)
    constexpr uint32 ABOLISH_DISEASE       = 552;   // 祛病术 (疾病持续驱散)
    constexpr uint32 CURE_DISEASE          = 528;   // 驱除疾病 (单体疾病)

    // =========================================================================
    // 3. 续航 (Sustain)
    // 注：下列均为基础技能，非天赋，调用 GetAppropriateRank 时第二参数传 false。
    // =========================================================================
    constexpr uint32 SHADOWFIEND           = 34433; // 暗影魔 (66级基础法术，召出后攻击回蓝，需敌对目标承载)
    constexpr uint32 HYMN_OF_HOPE          = 64901; // 希望圣歌 (群体回蓝，通道技，需刹停)

    // =========================================================================
    // 4. 神圣核心被动天赋与雕文补偿 (Passive Talents & Glyphs)
    // Creature 没有天赋树，必须主动注入等价被动光环，否则治疗量/续航全面亏模。
    // 下列天赋缺失将直接导致团本高压期崩盘。
    // =========================================================================
    constexpr uint32 SPIRITUAL_GUIDANCE          = 15031; // 精神指引 Rank 5 (40级)：25% 精神转法强
    constexpr uint32 EMPOWERED_RENEW             = 33152; // 强化恢复 Rank 3 (30级)：恢复额外加成 + 立即生效一跳
    constexpr uint32 DIVINE_PROVIDENCE           = 47567; // 神圣天恩 Rank 5 (40级)：环/祷言治疗量 +10%
    constexpr uint32 INSPIRATION                 = 15363; // 灵感 Rank 3 (20级)：暴击治疗使目标受物理伤害 -10%
    constexpr uint32 MEDITATION                  = 14777; // 冥想 Rank 3 (30级)：施法中仍保持 50% 精神回蓝
    constexpr uint32 SURGE_OF_LIGHT_TALENT       = 33154; // 圣光涌动【天赋被动】(30级)：注入自身，由治疗暴击触发 Proc
    constexpr uint32 SURGE_OF_LIGHT_PROC         = 33151; // 圣光涌动【临时 Proc】：使下一发快速治疗瞬发且零蓝耗
    constexpr uint32 SERENDIPITY                 = 63730; // 好运 Rank 3 (50级)：快疗/联结使下发大加/祷言读条 -20%
    constexpr uint32 SERENDIPITY_PROC            = 63734; // 好运【临时 Proc】：叠满 2 层后强效治疗术/治疗祷言读条 -40%
    constexpr uint32 GLYPH_OF_CIRCLE_OF_HEALING  = 55675; // 治疗之环雕文 (50级)：目标数量增至 6 个
    constexpr uint32 GLYPH_OF_PRAYER_OF_HEALING  = 55680; // 治疗祷言雕文 (60级)：附加持续 HoT
}
