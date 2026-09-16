/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef _FROST_MAGE_SPELLS_H
#define _FROST_MAGE_SPELLS_H

#include "Define.h"

namespace FrostMageSpells
{
    // =========================================================================
    // 护甲、核心法术与战术大招
    // =========================================================================
    constexpr uint32 FROSTBOLT               = 42842;  // 寒冰箭 Rank 16 (核心读条填充技)
    constexpr uint32 ICE_LANCE               = 42914;  // 冰枪术 Rank 3 (瞬发, 冻结目标 3 倍伤害)
    constexpr uint32 DEEP_FREEZE             = 44572;  // 深度冻结 (纯天赋, 60 级解锁, 30s CD)
    constexpr uint32 FROSTFIRE_BOLT          = 47610;  // 霜火之箭 Rank 2 (思维冻结瞬发消费)
    constexpr uint32 ICE_BARRIER             = 43039;  // 寒冰护体 Rank 8 (纯天赋, 40 级解锁, 常驻吸收盾)
    constexpr uint32 ICY_VEINS               = 12472;  // 冰冷血脉 (纯天赋, 30 级解锁, Off-GCD)
    constexpr uint32 COLD_SNAP               = 11958;  // 急速冷却 (纯天赋, 40 级解锁, Off-GCD)
    constexpr uint32 FROST_NOVA              = 42917;  // 冰霜新星 Rank 6 (近身定身剥离)
    constexpr uint32 CONE_OF_COLD            = 42931;  // 冰锥术 Rank 8 (正面锥形减速伤害)
    constexpr uint32 MAGE_ARMOR              = 43024;  // 魔甲术 Rank 6 (回蓝与抗性护甲)
    constexpr uint32 MOLTEN_ARMOR            = 43046;  // 熔甲术 Rank 3 (暴击护甲)
    constexpr uint32 MIRROR_IMAGE            = 55342;  // 镜像 (80 级解锁, Off-GCD)
    constexpr uint32 EVOCATION               = 12051;  // 唤醒 (长引导回蓝)
    constexpr uint32 MANA_GEM                = 42985;  // 製造法力法玉/红宝石
    constexpr uint32 MANA_GEM_EFFECT         = 42987;  // 法力红宝石回蓝效果 (触发式直放)
    constexpr uint32 ICE_BLOCK               = 45438;  // 寒冰屏障 (冰箱自保)
    constexpr uint32 INVISIBILITY            = 66;     // 隐形术 (清仇恨脱困)
    constexpr uint32 BLINK                   = 1953;   // 闪现术 (盲区解控位移)
    constexpr uint32 COUNTERSPELL            = 2139;   // 法术反制 (远程压秒打断)

    // =========================================================================
    // 触发光环 / 目标 Debuff
    // =========================================================================
    constexpr uint32 AURA_FINGERS_OF_FROST   = 44544;  // 寒冰指触发光环 (2 层充能, 视同冻结)
    constexpr uint32 AURA_BRAIN_FREEZE       = 57761;  // 思维冻结触发光环 (瞬发 0 耗蓝霜火之箭)
    constexpr uint32 AURA_ICY_VEINS          = 12472;  // 冰冷血脉光环
    constexpr uint32 AURA_ICE_BARRIER        = 43039;  // 寒冰护体吸收盾光环
    constexpr uint32 AURA_WINTERS_CHILL      = 28595;  // 深冬之寒 Debuff (目标受法术暴击率 +5%)
    constexpr uint32 AURA_FROST_NOVA         = 42917;  // 冰霜新星定身 Debuff

    // =========================================================================
    // 满阶被动天赋根源 / 雕文补偿 (铁律 33: 严禁注入 Rank 1 缩水根源)
    // =========================================================================
    constexpr uint32 FINGERS_OF_FROST        = 44545;  // 寒冰指 Rank 2 天赋根源
    constexpr uint32 BRAIN_FREEZE            = 44549;  // 思维冻结 Rank 3 天赋根源
    constexpr uint32 SHATTER                 = 12983;  // 碎冰 Rank 3 (冻结目标暴击率 +50%)
    constexpr uint32 ICE_SHARDS              = 12674;  // 寒冰碎片 Rank 3 (冰系暴击伤害 +100%)
    constexpr uint32 PIERCING_ICE            = 12985;  // 刺骨寒冰 Rank 3 (冰霜法术伤害 +6%)
    constexpr uint32 ARCTIC_WINDS            = 31678;  // 极寒之风 Rank 5 (冰霜伤害 +5%)
    constexpr uint32 EMPOWERED_FROSTBOLT     = 31683;  // 强化寒冰箭 Rank 2 (读条 -0.2s)
    constexpr uint32 WINTERS_CHILL           = 28595;  // 深冬之寒 Rank 3 天赋根源 (冰系暴击易伤)
    constexpr uint32 PRECISION               = 29440;  // 法术精准 Rank 3 (法术命中 +3%, 耗蓝 -3%)
    constexpr uint32 FROST_CHANNELING        = 12531;  // 冰霜导能 Rank 3 (冰霜法术耗蓝 -10%, 仇恨 -10%)
    constexpr uint32 GLYPH_OF_FROSTBOLT      = 56370;  // 寒冰箭雕文 (寒冰箭伤害 +5%)
    constexpr uint32 GLYPH_OF_ICE_LANCE      = 56377;  // 冰枪术雕文 (对高等级目标伤害 +30%)
    constexpr uint32 GLYPH_OF_ETERNAL_WATER  = 70937;  // 永恒之水雕文 (备用, 当前阶段不下发)
}

#endif // _FROST_MAGE_SPELLS_H
