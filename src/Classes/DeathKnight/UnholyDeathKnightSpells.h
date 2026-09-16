/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef UNHOLY_DEATH_KNIGHT_SPELLS_H_
#define UNHOLY_DEATH_KNIGHT_SPELLS_H_

#include "Define.h"

namespace UnholyDeathKnightSpells
{
    // =========================================================================
    // 姿态 / 常驻增益
    // =========================================================================
    constexpr uint32 BLOOD_PRESENCE      = 48266; // 鲜血灵气 (3.3.5a PvE 输出姿态: +15% 伤害, 4% 吸血)
    constexpr uint32 HORN_OF_WINTER      = 57623; // 寒冬号角 Rank 2 (力量敏捷增益 + 产 10 符能)

    // =========================================================================
    // 疾病链 (施加疫病 / 无损刷新)
    // =========================================================================
    constexpr uint32 ICY_TOUCH           = 49909; // 冰冷触摸 Rank 5 (施加冰霜疫病)
    constexpr uint32 PLAGUE_STRIKE       = 49921; // 暗影打击 Rank 6 (施加血之疫病)
    constexpr uint32 PESTILENCE          = 50842; // 传染 (配合传染雕文无损刷新主目标双疾病)

    // =========================================================================
    // 核心打击与泄能
    // =========================================================================
    constexpr uint32 SCOURGE_STRIKE      = 55090; // 天灾打击 Rank 6 (纯天赋, 55 级解锁, 核心双疾病物理/暗影打击)
    constexpr uint32 DEATH_COIL          = 49895; // 凋零缠绕 Rank 5 (核心远程泄符能 / 末日突降免费打击)
    constexpr uint32 BLOOD_STRIKE        = 49930; // 鲜血打击 Rank 6 (维持荒芜增益 / 转死亡符文)

    // =========================================================================
    // 大招 / 生存
    // =========================================================================
    constexpr uint32 BONE_SHIELD         = 49222; // 白骨之盾 (纯天赋, 40 级解锁, 1m CD, +2% 伤害 / -20% 承伤)
    constexpr uint32 SUMMON_GARGOYLE     = 49206; // 召唤石像鬼 (纯天赋, 60 级解锁, 3m CD, 消耗 60 符能)
    constexpr uint32 EMPOWER_RUNE_WEAPON = 47568; // 符文武器增效 (5m CD, 重置符文 + 25 符能)
    constexpr uint32 ICEBOUND_FORTITUDE  = 48792; // 冰封之韧 (濒死 50% 硬减伤 + 免疫昏迷, 2m CD)
    constexpr uint32 ANTI_MAGIC_SHELL    = 48707; // 反魔法护罩 (绿坝吸收魔法自保, 45s CD)

    // =========================================================================
    // 触发光环 / 疾病 Debuff / 光环 
    // =========================================================================
    constexpr uint32 AURA_SUDDEN_DOOM    = 49530; // 末日突降触发光环 (下一次凋零缠绕免费且不消耗符能)
    constexpr uint32 AURA_DESOLATION     = 66803; // 荒芜触发光环 (鲜血打击后全伤害提高 5%, 持续 20s)
    constexpr uint32 AURA_BONE_SHIELD    = 49222; // 白骨之盾光环
    constexpr uint32 AURA_FROST_FEVER    = 55095; // 冰霜疫病 Debuff
    constexpr uint32 AURA_BLOOD_PLAGUE   = 55078; // 血之疫病 Debuff

    // =========================================================================
    // 满阶被动天赋根源 (严禁注入 Rank 1, 铁律 33)
    // =========================================================================
    constexpr uint32 DESOLATION          = 66803; // 荒芜 Rank 5 (鲜血打击 100% 触发荒芜增益光环)
    constexpr uint32 EBON_PLAGUEBRINGER  = 51161; // 黑色热疫使者 Rank 3 (疫病附加黑色热疫: +13% 受法伤, +30% 疾病伤害)
    constexpr uint32 RAGE_OF_RIVENDARE   = 50125; // 瑞文戴尔之怒 Rank 5 (对带病目标法术/技能伤害 +10%)
    constexpr uint32 IMPURITY            = 49638; // 不纯 Rank 5 (攻击强度对法术伤害加成提高 20%)
    constexpr uint32 REAPING             = 56835; // 收割 Rank 3 (鲜血打击/传染 100% 将鲜血/冰霜符文转为死亡符文)
    constexpr uint32 NECROSIS            = 51465; // 坏死 Rank 5 (普通攻击附加 20% 暗影伤害)
    constexpr uint32 BLOOD_CAKED_BLADE   = 51460; // 鲜血染红之刃 Rank 3 (近战命中 30% 几率触发血刃副攻击)
    constexpr uint32 MASTER_OF_GHOULS    = 52143; // 食尸鬼主宰 (使召唤物成为常驻随从)
    constexpr uint32 SUDDEN_DOOM         = 49530; // 末日突降 Rank 3 天赋根源 (普攻/鲜血打击 15% 触发免费缠绕)

    // =========================================================================
    // 雕文补偿
    // =========================================================================
    constexpr uint32 GLYPH_OF_DISEASE        = 63334; // 传染雕文 (传染刷新主目标双疾病持续时间)
    constexpr uint32 GLYPH_OF_SCOURGE_STRIKE = 58642; // 天灾打击雕文 (天灾打击暴击率提高)
    constexpr uint32 GLYPH_OF_DARK_DEATH     = 58671; // 暗黑死亡雕文 (凋零缠绕伤害提高 15%)

    // =========================================================================
    // 纯天赋技能最低解锁等级契约
    // (3.3.5a 天赋法术 DBC SpellLevel 恒为 0, GetAppropriateRank 无法降阶,
    //  必须显式登记并在施法前门禁, 否则低等级会 100% CanCast 失败)
    // =========================================================================
    constexpr uint8 BONE_SHIELD_MIN_LEVEL     = 40;
    constexpr uint8 SCOURGE_STRIKE_MIN_LEVEL  = 55;
    constexpr uint8 SUMMON_GARGOYLE_MIN_LEVEL = 60;
}

#endif // UNHOLY_DEATH_KNIGHT_SPELLS_H_
