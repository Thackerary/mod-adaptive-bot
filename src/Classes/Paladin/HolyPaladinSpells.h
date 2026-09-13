/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

namespace HolyPaladinSpells
{
    // =========================================================================
    // 1. 核心治疗与道标 (Core Healing & Beacon)
    // =========================================================================
    constexpr uint32 BEACON_OF_LIGHT        = 53563; // 圣光道标 (60 码复制治疗核心机制)
    constexpr uint32 SACRED_SHIELD          = 53601; // 圣洁护盾 (常驻吸收盾)
    constexpr uint32 HOLY_LIGHT             = 48782; // 圣光术 Rank 13 (重伤刷血核心)
    constexpr uint32 FLASH_OF_LIGHT         = 48785; // 圣光闪现 Rank 9 (平稳填充)
    constexpr uint32 HOLY_SHOCK             = 48825; // 神圣震击 Rank 7 (瞬发急救)
    constexpr uint32 LAY_ON_HANDS           = 48788; // 圣疗术 Rank 5 (极限救急)
    constexpr uint32 CLEANSE                = 4987;  // 清洁术 (驱散魔法/中毒/疾病)

    // =========================================================================
    // 2. 光环、祝福与圣印 (Auras, Blessings & Seals)
    // =========================================================================
    constexpr uint32 CONCENTRATION_AURA     = 19746; // 专注光环 (防击退，治疗主光环)
    constexpr uint32 DEVOTION_AURA          = 48942; // 虔诚光环 Rank 10 (光环兜底)
    constexpr uint32 BLESSING_OF_WISDOM     = 48936; // 智慧祝福 Rank 9
    constexpr uint32 BLESSING_OF_MIGHT      = 48932; // 力量祝福 Rank 10 (无蓝职业专用)
    constexpr uint32 BLESSING_OF_KINGS      = 20217; // 王者祝福
    constexpr uint32 SEAL_OF_WISDOM         = 20166; // 智慧圣印 (审判前置)
    constexpr uint32 SEAL_OF_RIGHTEOUSNESS  = 21084; // 正义圣印 Rank 9 (低等级过渡保底)

    // =========================================================================
    // 3. 爆发、续航与自保 (Burst, Sustain & Defensive Cooldowns)
    // =========================================================================
    constexpr uint32 DIVINE_PLEA            = 54428; // 神圣祈求 (回蓝，治疗量 -50%)
    constexpr uint32 AVENGING_WRATH         = 31884; // 复仇之怒 (翅膀，治疗量 +20%)
    constexpr uint32 DIVINE_FAVOR           = 20216; // 神恩术 (下一发治疗必暴击)
    constexpr uint32 DIVINE_ILLUMINATION    = 28280; // 神启 (耗蓝减半 15 秒)
    constexpr uint32 DIVINE_SHIELD          = 1020;  // 圣盾术 Rank 2 (50 级，12 秒)
    constexpr uint32 HAND_OF_PROTECTION     = 10278; // 保护之手 Rank 3 (物理免疫保队友)
    constexpr uint32 JUDGEMENT_OF_LIGHT     = 48934; // 光明审判 Rank 6 (维持纯洁审判)
    constexpr uint32 FORBEARANCE            = 25771; // 自律 (封印圣盾/保护之手/圣佑)

    // =========================================================================
    // 4. 被动天赋光环补偿 (Passive Talent Auras)
    // =========================================================================
    constexpr uint32 IMPROVED_JUDGEMENTS    = 25956; // 强化审判 Rank 2 (审判射程 +20 码)
    constexpr uint32 ILLUMINATION           = 20272; // 启发 Rank 5 (暴击治疗回蓝)
    constexpr uint32 HOLY_GUIDANCE          = 31841; // 神圣指引 Rank 5 (智力转化法强)
    constexpr uint32 INFUSION_OF_LIGHT      = 53576; // 圣光灌注 Rank 2

    // 纯洁审判必须严格分离「天赋被动触发器」与「急速 Buff 本体」：
    // NPC 没有天赋树，需注入被动 (54155)，审判命中后才会由引擎派生急速 Buff (53657)。
    constexpr uint32 TALENT_JUDGEMENTS_OF_THE_PURE = 54155; // 纯洁审判 Rank 5 (天赋被动触发器)
    constexpr uint32 BUFF_JUDGEMENTS_OF_THE_PURE   = 53657; // 纯洁审判急速 Buff (15% 急速，1 分钟)

    // =========================================================================
    // 5. 核心雕文 (Major Glyphs)
    // =========================================================================
    constexpr uint32 GLYPH_OF_HOLY_LIGHT    = 54937; // 圣光术雕文 (圣光术溅射群抬核心)
}
