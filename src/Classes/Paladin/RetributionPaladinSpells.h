/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

// =============================================================================
// 惩戒圣骑士 (Retribution Paladin) 3.3.5a 法术常量表
// -----------------------------------------------------------------------------
// 全部 ID 取 WotLK 最高等级 Rank。
// 基础法术的低等级由 AdaptiveBotAI::GetAppropriateRank 沿 GetPrevRankSpell
// 链自动降阶；天赋法术 DBC SpellLevel 恒为 0，必须由专精按
// GetTalentSpellMinLevel 显式门禁。本文件严禁承载任何逻辑。
// =============================================================================
namespace RetributionPaladinSpells
{
    // -------------------------------------------------------------------------
    // 核心近战打击与终结
    // -------------------------------------------------------------------------
    constexpr uint32 CRUSADER_STRIKE     = 35395;  // 十字军打击 (20 级天赋，瞬发近战，4s CD)
    constexpr uint32 DIVINE_STORM        = 53385;  // 神圣风暴 (60 级终极天赋，4 目标武器伤害 + 回血，10s CD)
    constexpr uint32 JUDGEMENT_OF_WISDOM = 53408;  // 智慧审判 (回蓝审判，8s CD，触发睿智审判回蓝)
    constexpr uint32 JUDGEMENT_OF_LIGHT  = 20271;  // 光明审判 (通用审判，与任意圣印兼容，备用)
    constexpr uint32 HAMMER_OF_WRATH     = 48806;  // 愤怒之锤 Rank 6 (斩杀技，目标 < 20% 可用，6s CD)
    constexpr uint32 EXORCISM            = 48801;  // 驱邪术 Rank 9 (神圣伤害，15s CD，需战争艺术瞬发打出)
    constexpr uint32 CONSECRATION        = 48819;  // 奉献 Rank 8 (近战范围神圣地面 DoT，8s CD)

    // -------------------------------------------------------------------------
    // 圣印与自保
    // -------------------------------------------------------------------------
    constexpr uint32 SEAL_OF_CORRUPTION    = 53736; // 腐蚀圣印 (单体叠加 DoT 圣印，联盟等效复仇圣印 31801)
    constexpr uint32 SEAL_OF_RIGHTEOUSNESS = 20290; // 正义圣印 Rank 9 (低等级主力单体圣印)
    constexpr uint32 SACRED_SHIELD         = 53601; // 圣洁护盾 (80 级天赋，伤害吸收盾，维持自身 30s)
    constexpr uint32 DIVINE_SHIELD         = 642;   // 圣盾术/无敌 (全免疫清仇恨但伤害 -50%，5m CD，触发自律)
    constexpr uint32 LAY_ON_HANDS          = 48788; // 圣疗术 Rank 5 (生命 < 15% 瞬间满血，20m CD，触发自律)
    constexpr uint32 HAND_OF_SALVATION     = 1038;  // 拯救之手 (持续降低仇恨，2m CD)

    // -------------------------------------------------------------------------
    // 爆发与控制
    // -------------------------------------------------------------------------
    constexpr uint32 AVENGING_WRATH      = 31884;  // 复仇之怒/翅膀 (70 级天赋，伤害 +20%，20s，2m CD，Off-GCD)
    constexpr uint32 HAMMER_OF_JUSTICE   = 10308;  // 制裁之锤 Rank 4 (近战昏迷打断，1m CD)

    // -------------------------------------------------------------------------
    // 核心状态与被动光环补偿 (以光环形式注入，弥补 NPC 无天赋树缺陷)
    // -------------------------------------------------------------------------
    constexpr uint32 FORBEARANCE          = 25771;  // 自律 Debuff (阻止无敌/圣疗)
    constexpr uint32 AURA_THE_ART_OF_WAR  = 59578;  // 战争艺术触发光环 (使驱邪术瞬发且不读条)
    constexpr uint32 JUDGEMENTS_OF_THE_WISE = 31878; // 睿智审判 Rank 3 (审判提供全团回蓝与自身即时回蓝)
    constexpr uint32 SHEATH_OF_LIGHT      = 53503;  // 圣光之鞘 Rank 3 (AP 转化为 SP)
    constexpr uint32 RIGHTEOUS_VENGEANCE  = 53382;  // 正义复仇 Rank 3 (暴击附带 30% 流血 DoT)
    constexpr uint32 FANATICISM           = 31880;  // 狂热 Rank 5 (审判暴击率 +18%)
    constexpr uint32 GLYPH_OF_JUDGEMENT   = 54922;  // 审判雕文 (审判伤害 +10%)
    constexpr uint32 GLYPH_OF_CONSECRATION = 54928; // 奉献雕文 (持续与冷却延长)
    constexpr uint32 GLYPH_OF_EXORCISM    = 54934;  // 驱邪雕文 (驱邪伤害 +20%)
}
