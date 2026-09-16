/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef _ENHANCEMENT_SHAMAN_SPELLS_H
#define _ENHANCEMENT_SHAMAN_SPELLS_H

#include "Define.h"

namespace EnhancementShamanSpells
{
    // =========================================================================
    // 护盾、核心打击与大招
    // =========================================================================
    constexpr uint32 LIGHTNING_SHIELD        = 49281;  // 闪电之盾 Rank 11 (常驻自保与静电震击联动)
    constexpr uint32 STORMSTRIKE             = 17364;  // 风暴打击 (40 级纯天赋, 8s CD, 双武器重击 + 20% 自然易伤)
    constexpr uint32 LAVA_LASH               = 60103;  // 熔岩猛击 (40 级纯天赋, 6s CD, 副手火焰打击)
    constexpr uint32 EARTH_SHOCK             = 49231;  // 大地震击 Rank 10 (核心瞬发自然爆发)
    constexpr uint32 FLAME_SHOCK             = 49233;  // 烈焰震击 Rank 9 (核心火焰流血 DoT)
    constexpr uint32 LIGHTNING_BOLT          = 49238;  // 闪电箭 Rank 14 (漩涡武器 5 层瞬发消费)
    constexpr uint32 CHAIN_LIGHTNING         = 49271;  // 闪电链 Rank 8 (漩涡武器 5 层多目标瞬发消费)
    constexpr uint32 FIRE_NOVA               = 61657;  // 火焰新星 Rank 9 (以火焰图腾为原点爆破)
    constexpr uint32 FERAL_SPIRIT            = 51533;  // 野性狼魂 (60 级纯天赋, 3m CD, 召唤两只幽灵狼爆发)
    constexpr uint32 SHAMANISTIC_RAGE        = 30823;  // 萨满之怒 (50 级纯天赋, 1m CD, 30% 减伤 + 命中回蓝)
    constexpr uint32 BLOODLUST               = 2825;   // 嗜血 (部落核心全团爆发, 5m CD)
    constexpr uint32 HEROISM                 = 32182;  // 英勇 (联盟核心全团爆发, 5m CD)
    constexpr uint32 MAGMA_TOTEM             = 58734;  // 熔岩图腾 Rank 7 (AoE 火焰图腾)
    constexpr uint32 SEARING_TOTEM           = 58704;  // 灼热图腾 Rank 10 (单体火焰图腾)
    constexpr uint32 STRENGTH_OF_EARTH_TOTEM = 58643;  // 大地之力图腾 Rank 8 (力量敏捷增益)
    constexpr uint32 WINDFURY_TOTEM          = 8512;   // 风怒图腾 (近战攻速增益)
    constexpr uint32 MANA_SPRING_TOTEM       = 58774;  // 法力之泉图腾 Rank 8 (团队回蓝)
    constexpr uint32 WIND_SHEAR              = 57994;  // 风剪 (短 CD 远程压秒打断)

    // =========================================================================
    // 触发光环、被动天赋与雕文补偿
    // =========================================================================
    constexpr uint32 AURA_MAELSTROM_WEAPON   = 53817;  // 漩涡武器层数光环 (1~5 层, 5 层使法术瞬发)
    constexpr uint32 AURA_STORMSTRIKE        = 17364;  // 风暴打击自然易伤 Debuff
    constexpr uint32 MAELSTROM_WEAPON        = 51532;  // 漩涡武器 Rank 5 天赋根源
    constexpr uint32 DUAL_WIELD              = 30798;  // 双持被动
    constexpr uint32 DUAL_WIELD_SPEC         = 30819;  // 双武器专精 Rank 3 (物理命中 +6%)
    constexpr uint32 UNLEASHED_RAGE          = 30809;  // 怒火释放 Rank 5 (近战暴击团队 AP +10%)
    constexpr uint32 WEAPON_MASTERY          = 29086;  // 武器专精 Rank 3 (武器伤害 +10%)
    constexpr uint32 STATIC_SHOCK            = 51527;  // 静电震击 Rank 3 (平砍几率触发闪电之盾电击)
    constexpr uint32 FLURRY                  = 16280;  // 乱舞 Rank 5 (暴击攻速 +30%)
    constexpr uint32 MENTAL_QUICKNESS        = 30814;  // 敏锐思维 Rank 3 (AP 转化为法强, 瞬发耗蓝 -6%)
    constexpr uint32 SHAMANISTIC_FOCUS       = 43338;  // 萨满专注 (近战暴击使下一个震击耗蓝 -60%)
    constexpr uint32 GLYPH_OF_STORMSTRIKE    = 55446;  // 风暴打击雕文 (自然易伤提升至 28%)
    constexpr uint32 GLYPH_OF_FERAL_SPIRIT   = 63271;  // 野性狼魂雕文 (幽灵狼 AP 提高 30%)
    constexpr uint32 GLYPH_OF_LIGHTNING_SHIELD = 55445; // 闪电之盾雕文 (闪电之盾伤害 +20%)
}

#endif // _ENHANCEMENT_SHAMAN_SPELLS_H
