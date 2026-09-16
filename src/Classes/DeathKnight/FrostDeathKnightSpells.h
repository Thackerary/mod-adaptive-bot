/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef _BOT_FROST_DEATH_KNIGHT_SPELLS_H
#define _BOT_FROST_DEATH_KNIGHT_SPELLS_H

#include "Define.h"

namespace FrostDeathKnightSpells
{
    // =====================================================================
    // 姿态 / 符能增益 / 核心打击
    // 统一登记最高 Rank ID，等级不足时由 GetAppropriateRank 自动降阶
    // =====================================================================
    constexpr uint32 BLOOD_PRESENCE      = 48266;  // 鲜血灵气 (3.3.5a 核心 DPS 姿态: +15% 伤害, 4% 吸血)
    constexpr uint32 FROST_PRESENCE      = 48263;  // 冰霜灵气 (防坦姿态, 严禁开启)
    constexpr uint32 ICY_TOUCH           = 49909;  // 冰冷触摸 Rank 5 (施加冰霜疫病)
    constexpr uint32 PLAGUE_STRIKE       = 49921;  // 暗影打击 Rank 6 (施加血之疫病)
    constexpr uint32 OBLITERATE          = 51425;  // 湮灭 Rank 6 (核心双疾病物理重击)
    constexpr uint32 FROST_STRIKE        = 55268;  // 冰霜打击 Rank 6 (纯天赋, 40 级解锁, 泄符能主力)
    constexpr uint32 HOWLING_BLAST       = 51411;  // 凛风冲击 Rank 4 (纯天赋, 60 级解锁, 白霜免费瞬发)
    constexpr uint32 BLOOD_STRIKE        = 49930;  // 鲜血打击 Rank 6 (鲜血符文填充/转死符文)
    constexpr uint32 PESTILENCE          = 50842;  // 传染 (无损刷新主目标双疾病, 配合传染雕文)
    constexpr uint32 HORN_OF_WINTER      = 57623;  // 寒冬号角 Rank 2 (力量敏捷增益 + 产 10 符能)
    constexpr uint32 UNBREAKABLE_ARMOR   = 51271;  // 铜墙铁壁 (纯天赋, 50 级解锁, 1m CD)
    constexpr uint32 EMPOWER_RUNE_WEAPON = 47568;  // 符文武器增效 (5m CD, 重置符文 + 25 符能)
    constexpr uint32 ICEBOUND_FORTITUDE  = 48792;  // 冰封之韧 (濒死 50% 硬减伤 + 免疫昏迷, 2m CD)
    constexpr uint32 ANTI_MAGIC_SHELL    = 48707;  // 反魔法护罩 (魔法吸收自保, 45s CD)

    // =====================================================================
    // 触发光环 / 疾病 Debuff
    // =====================================================================
    constexpr uint32 AURA_KILLING_MACHINE = 51124; // 杀戮机器触发光环 (下一次冰霜打击/凛风冲击必暴)
    constexpr uint32 AURA_FREEZING_FOG    = 59052; // 冻结之雾 (白霜触发: 凛风冲击免费且瞬发)
    constexpr uint32 AURA_FROST_FEVER     = 55095; // 冰霜疫病 Debuff
    constexpr uint32 AURA_BLOOD_PLAGUE    = 55078; // 血之疫病 Debuff

    // =====================================================================
    // 满阶被动天赋根源 (铁律 33: 严禁注入 Rank 1 导致触发率/数值缩水)
    // =====================================================================
    constexpr uint32 THREAT_OF_THASSARIAN = 66192; // Rank 3 双持打击核心被动
    constexpr uint32 DUAL_WIELD_SPEC      = 50370; // Rank 3 副手武器伤害 +25%
    constexpr uint32 KILLING_MACHINE      = 51130; // Rank 5 杀戮机器触发源
    constexpr uint32 RIME                 = 56836; // Rank 3 白霜触发源 (湮灭 15% 触发冻结之雾)
    constexpr uint32 GLACIER_ROT          = 49478; // Rank 3 带病目标受冰霜伤害 +20%
    constexpr uint32 BLACK_ICE            = 49149; // Rank 5 冰霜与暗影伤害 +10%
    constexpr uint32 BLOOD_OF_THE_NORTH   = 54643; // Rank 3 鲜血符文转死亡符文 (100% 几率)
    constexpr uint32 NERVES_OF_COLD_STEEL = 49011; // Rank 3 双持命中 +3% 与近战急速
    constexpr uint32 MIGHT_OF_MOGRAINE    = 49031; // Rank 3 湮灭/冰霜打击暴击伤害 +45%

    // =====================================================================
    // 雕文补偿
    // =====================================================================
    constexpr uint32 GLYPH_OF_FROST_STRIKE = 58647; // 冰霜打击雕文 (-8 符能消耗)
    constexpr uint32 GLYPH_OF_OBLITERATE   = 58615; // 湮灭雕文 (湮灭伤害 +25%)
    constexpr uint32 GLYPH_OF_DISEASE      = 63334; // 传染雕文 (传染刷新主目标双疾病持续时间)
}

#endif // _BOT_FROST_DEATH_KNIGHT_SPELLS_H
