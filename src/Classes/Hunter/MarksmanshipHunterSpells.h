/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

// =============================================================================
// 射击猎人 (Marksmanship Hunter) 3.3.5a 法术常量表
// -----------------------------------------------------------------------------
// 全部 ID 取 WotLK 最高等级 Rank；低等级由 AdaptiveBotAI::GetAppropriateRank
// 沿 GetPrevRankSpell 链自动降阶，本文件严禁承载任何逻辑。
// =============================================================================
namespace MarksmanshipHunterSpells
{
    // -------------------------------------------------------------------------
    // 核心远程射击
    // -------------------------------------------------------------------------
    constexpr uint32 AUTO_SHOT             = 75;     // 自动射击 (远程白字)
    constexpr uint32 SERPENT_STING         = 49001;  // 毒蛇钉刺 Rank 12 (基础 DoT，奇美拉射击跳板)
    constexpr uint32 CHIMERA_SHOT          = 53209;  // 奇美拉射击 (60 级终极天赋，刷新钉刺 + 40% 自然伤害，10s CD)
    constexpr uint32 AIMED_SHOT            = 49050;  // 瞄准射击 Rank 9 (20 级天赋，物理致死打击，10s CD)
    constexpr uint32 KILL_SHOT             = 61006;  // 杀戮射击 Rank 3 (71 级技能，目标 < 20% 斩杀，15s CD)
    constexpr uint32 STEADY_SHOT           = 49052;  // 稳固射击 Rank 4 (62 级基础读条填充，2.0s)
    constexpr uint32 ARCANE_SHOT           = 49045;  // 奥术射击 Rank 11 (基础瞬发，低等级/跑位填充，6s CD)
    constexpr uint32 VOLLEY                = 58434;  // 乱射 Rank 6 (40 级引导 AoE，保留待多目标扩展)

    // -------------------------------------------------------------------------
    // 近战盲区与脱困解控
    // -------------------------------------------------------------------------
    constexpr uint32 RAPTOR_STRIKE         = 48996;  // 猛禽一击 Rank 11 (8 码内近战垫刀)
    constexpr uint32 WING_CLIP             = 2974;   // 摔绊 Rank 3 (8 码内减速脱困)
    constexpr uint32 DISENGAGE             = 781;    // 逃脱 Rank 3 (向后腾跃脱困，25s CD，仅战时有效)
    constexpr uint32 DETERRENCE            = 19263;  // 威慑 (20 级保命，5 秒 100% 招架/偏斜，90s CD)
    constexpr uint32 FEIGN_DEATH           = 5384;   // 假死 (30 级清仇恨保命，30s CD)

    // -------------------------------------------------------------------------
    // 守护与增益体系
    // -------------------------------------------------------------------------
    constexpr uint32 ASPECT_OF_THE_DRAGONHAWK = 61840; // 龙鹰守护 Rank 2 (80 级常驻输出守护)
    constexpr uint32 ASPECT_OF_THE_HAWK       = 27044; // 雄鹰守护 Rank 8 (低等级替代龙鹰)
    constexpr uint32 ASPECT_OF_THE_VIPER      = 34074; // 蝰蛇守护 (回蓝守护，伤害 -50%)
    constexpr uint32 TRUESHOT_AURA            = 19506; // 强击光环 (40 级天赋，全队 AP +10%)
    constexpr uint32 MISDIRECTION             = 34477; // 误导 (70 级仇恨转移，30s CD)

    // -------------------------------------------------------------------------
    // 爆发与打断
    // -------------------------------------------------------------------------
    constexpr uint32 SILENCING_SHOT        = 34490;  // 沉默射击 (30 级天赋，瞬发打断/沉默，20s CD，Off-GCD)
    constexpr uint32 RAPID_FIRE            = 3045;   // 急速射击 (急速 +40%，3 分钟 CD，Off-GCD)
    constexpr uint32 READINESS             = 23989;  // 准备就绪 (50 级天赋，重置全部猎人技能 CD，3 分钟 CD)

    // -------------------------------------------------------------------------
    // 被动天赋补偿与雕文 (以光环形式注入，弥补 NPC 无天赋树缺陷)
    // -------------------------------------------------------------------------
    constexpr uint32 MORTAL_SHOTS          = 34499;  // 致死射击 Rank 5 (暴击伤害 +30%)
    constexpr uint32 PIERCING_SHOTS        = 53238;  // 穿刺射击 Rank 3 (奇美拉/瞄准/稳固暴击附带 30% 流血)
    constexpr uint32 MASTER_MARKSMAN       = 34489;  // 射击大师 Rank 5 (暴击率 +5%，稳固耗蓝 -25%)
    constexpr uint32 GLYPH_OF_CHIMERA_SHOT = 56829;  // 奇美拉射击雕文 (奇美拉 CD -1s)
    constexpr uint32 GLYPH_OF_SERPENT_STING = 56800; // 毒蛇钉刺雕文 (毒蛇持续时间 +6s)
    constexpr uint32 GLYPH_OF_KILL_SHOT    = 56843;  // 杀戮射击雕文 (斩杀目标未死立即重置 CD)
}
