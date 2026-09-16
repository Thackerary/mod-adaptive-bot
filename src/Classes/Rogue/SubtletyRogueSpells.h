/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

namespace SubtletyRogueSpells
{
    // =========================================================================
    // 姿态、战术技能与核心打击
    // =========================================================================
    constexpr uint32 STEALTH             = 1784;   // 潜行 (脱战常驻)
    constexpr uint32 SHADOW_DANCE        = 51713;  // 暗影之舞 (60级纯天赋, 持续6s/雕文8s, 1m CD)
    constexpr uint32 SHADOWSTEP          = 36554;  // 暗影步 (30级纯天赋, 8~25码传送背身, 30s CD)
    constexpr uint32 PREMEDITATION       = 14183;  // 预谋 (40级纯天赋, 潜行/影舞下瞬发加2星, 20s CD)
    constexpr uint32 PREPARATION         = 14185;  // 伺机待发 (30级纯天赋, 重置急跑/消失/闪避/暗步冷却, 5m CD)
    constexpr uint32 HEMORRHAGE          = 48660;  // 出血 Rank 5 (20级纯天赋, 核心物理产星技能, 35能量)
    constexpr uint32 AMBUSH              = 48691;  // 伏击 Rank 10 (潜行/影舞核心核弹, 必须目标背后)
    constexpr uint32 BACKSTAB            = 48657;  // 背刺 Rank 12 (背后填充重击)
    constexpr uint32 EVISCERATE          = 48668;  // 刺骨 Rank 12 (主力物理终结技)
    constexpr uint32 RUPTURE             = 48672;  // 割裂 Rank 8 (物理流血终结技)
    constexpr uint32 SLICE_AND_DICE      = 6774;   // 切割 Rank 2 (核心攻速增益终结技)
    constexpr uint32 VANISH              = 26889;  // 消失 Rank 3 (战斗中强行潜行, 3m CD)
    constexpr uint32 EVASION             = 26669;  // 闪避 Rank 2 (濒死 50% 闪避, 3m CD)
    constexpr uint32 CLOAK_OF_SHADOWS    = 31224;  // 暗影斗篷 (魔法免疫与解除, 1m CD)
    constexpr uint32 SINISTER_STRIKE     = 48638;  // 邪恶攻击 Rank 12 (低级正面无出血时兜底产星)
    constexpr uint32 DEADLY_POISON       = 57973;  // 致命毒药 Rank 9 (普攻模拟注入)
    constexpr uint32 INSTANT_POISON      = 57965;  // 速效毒药 Rank 9 (技能命中模拟注入)

    // =========================================================================
    // 触发光环、被动天赋与雕文补偿
    // =========================================================================
    constexpr uint32 AURA_SHADOW_DANCE   = 51713;  // 暗影之舞触发光环
    constexpr uint32 AURA_STEALTH        = 1784;   // 潜行光环
    constexpr uint32 HONOR_AMONG_THIEVES = 51701;  // 盗贼的尊严 Rank 3 (团队暴击时每秒稳定+1星)
    constexpr uint32 MASTER_OF_SUBTLETY  = 31223;  // 敏锐大师 Rank 3 (潜行及破潜后6s增伤10%)
    constexpr uint32 FIND_WEAKNESS       = 31238;  // 寻找弱点 Rank 3 (终结技提升物理伤害10%)
    constexpr uint32 SERRATED_BLADES     = 14173;  // 锯刃 Rank 3 (无视护甲 + 割裂增伤)
    constexpr uint32 SINISTER_CALLING    = 31220;  // 险恶召唤 Rank 5 (敏捷+15%, 背刺/出血伤害+10%)
    constexpr uint32 OPPORTUNITY         = 14072;  // 机遇 Rank 2 (背刺/伏击伤害+20%)
    constexpr uint32 DIRTY_DEEDS         = 14083;  // 卑鄙 Rank 2 (目标<35%技能增伤20%)
    constexpr uint32 SLAUGHTER_FROM_THE_SHADOWS = 51711; // 暗影杀手 Rank 5 (背刺/伏击耗能-20, 出血耗能-5)
    constexpr uint32 GLYPH_OF_SHADOW_DANCE = 63420; // 暗影之舞雕文 (持续时间延长2s)
    constexpr uint32 GLYPH_OF_HEMORRHAGE = 56805;  // 出血雕文 (出血伤害提高40%)
    constexpr uint32 GLYPH_OF_EVISCERATE = 56804;  // 刺骨雕文 (刺骨暴击率提高10%)
}
