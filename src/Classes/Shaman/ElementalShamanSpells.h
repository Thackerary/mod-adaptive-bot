/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Common.h"

namespace ElementalShamanSpells
{
    // =========================================================================
    // 核心伤害与打击法术 (DBC SpellLevel 自动降阶)
    // =========================================================================
    constexpr uint32 LIGHTNING_BOLT       = 49238; // 闪电箭 Rank 14 (核心读条填充)
    constexpr uint32 CHAIN_LIGHTNING      = 49271; // 闪电链 Rank 8 (多目标/爆发读条)
    constexpr uint32 LAVA_BURST           = 60043; // 熔岩爆发 Rank 2 (烈焰震击下必暴核心)
    constexpr uint32 FLAME_SHOCK          = 49233; // 烈焰震击 Rank 9 (必暴前置 DoT)
    constexpr uint32 EARTH_SHOCK          = 49231; // 大地震击 Rank 10
    constexpr uint32 FROST_SHOCK          = 49236; // 冰霜震击 Rank 7
    constexpr uint32 WIND_SHEAR           = 57994; // 风剪 Rank 1 (25码 6s CD 打断)

    // =========================================================================
    // 护盾与团队大招
    // =========================================================================
    constexpr uint32 WATER_SHIELD         = 52127; // 水之护盾 Rank 9 (常驻回蓝)
    constexpr uint32 BLOODLUST            = 2825;  // 嗜血 (部落)
    constexpr uint32 HEROISM              = 32182; // 英勇 (联盟)

    // =========================================================================
    // 核心天赋技能 (DBC SpellLevel 恒为 0，需严格校验等级契约)
    // =========================================================================
    constexpr uint32 ELEMENTAL_MASTERY    = 16166; // 元素掌握 (40级天赋，瞬发+15%急速，Off-GCD)
    constexpr uint32 TOTEM_OF_WRATH       = 57722; // 天怒图腾 Rank 4 (50级天赋，全团法强+法暴)
    constexpr uint32 THUNDERSTORM         = 59159; // 雷霆风暴 Rank 4 (60级天赋，击退+8%回蓝)

    // =========================================================================
    // 基础常驻图腾
    // =========================================================================
    constexpr uint32 STONESKIN_TOTEM      = 58753; // 石肤图腾 Rank 10 (大地)
    constexpr uint32 STRENGTH_OF_EARTH_TOTEM = 58643; // 大地之力图腾 Rank 8 (大地备选)
    constexpr uint32 MANA_SPRING_TOTEM    = 58774; // 法力之泉图腾 Rank 8 (水)
    constexpr uint32 WRATH_OF_AIR_TOTEM   = 3738;  // 空气之怒图腾 Rank 1 (空气，全团5%法术急速)
    constexpr uint32 MAGMA_TOTEM          = 58734; // 熔岩图腾 Rank 7 (火焰AoE)
    constexpr uint32 SEARING_TOTEM        = 58704; // 灼热图腾 Rank 10 (火焰单体)

    // =========================================================================
    // 满阶被动天赋根源注入
    // =========================================================================
    constexpr uint32 CONCUSSION           = 16108; // 震荡 Rank 5 (+5% 伤害)
    constexpr uint32 CALL_OF_FLAME        = 16161; // 烈焰召唤 Rank 3 (+15% 图腾 & +6% 熔岩爆发伤害)
    constexpr uint32 ELEMENTAL_FOCUS      = 16164; // 元素集中 (清晰预兆节能施法)
    constexpr uint32 REVERBERATION        = 16115; // 混响 Rank 5 (-1s 震击与风剪 CD)
    constexpr uint32 CALL_OF_THUNDER      = 16174; // 雷霆召唤 Rank 1 (+5% 暴击)
    constexpr uint32 UNRELENTING_STORM    = 30678; // 无情风暴 Rank 3 (智力转回蓝)
    constexpr uint32 ELEMENTAL_PRECISION  = 30674; // 元素精准 Rank 3 (+3% 命中，-30% 仇恨)
    constexpr uint32 LIGHTNING_MASTERY    = 16582; // 闪电掌握 Rank 5 (-0.5s 闪电箭/链读条)
    constexpr uint32 ELEMENTAL_OATH       = 51470; // 元素誓约 Rank 2 (暴击全团5%法暴光环，+10%清晰预兆伤害)
    constexpr uint32 LIGHTNING_OVERLOAD   = 30681; // 闪电过载 Rank 3 (33% 概率触发闪电复制)
    constexpr uint32 LAVA_FLOWS           = 51482; // 熔岩流动 Rank 3 (熔岩爆发暴击额外伤害)
    constexpr uint32 STORM_EARTH_AND_FIRE = 51486; // 风暴、大地与火焰 Rank 3 (-2.5s 闪电链 CD，烈焰震击跳数增加)
    constexpr uint32 SHAMANISM            = 51530; // 萨满教义 Rank 5 (+20% SP 闪电箭，+25% SP 熔岩爆发)

    // =========================================================================
    // 清晰预兆【Proc Buff】: 元素集中触发后使下 2 次伤害法术免蓝。
    // 底层为 ProcCharges 充能型光环, 判断与扣减必须走 Aura::GetCharges()
    // 与原地降层, 严禁整层 RemoveAurasDueToSpell (铁律 10)。
    // =========================================================================
    constexpr uint32 AURA_CLEARCASTING    = 16246; // 清晰预兆 (节能施法, 2 次免蓝)

    // =========================================================================
    // 雕文补偿
    // =========================================================================
    constexpr uint32 GLYPH_OF_LAVA        = 55444; // 熔岩雕文 (熔岩爆发法伤加成提升)
    constexpr uint32 GLYPH_OF_LIGHTNING_BOLT = 55448; // 闪电箭雕文 (闪电箭伤害+4%)
    constexpr uint32 GLYPH_OF_FLAME_SHOCK = 55447; // 烈焰震击雕文 (烈焰震击爆伤+60%)
    constexpr uint32 GLYPH_OF_TOTEM_OF_WRATH = 63280; // 天怒图腾雕文 (自身额外获得法强)
}
