/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef _FIRE_MAGE_SPELLS_H
#define _FIRE_MAGE_SPELLS_H

#include "Define.h"

namespace FireMageSpells
{
    // =========================================================================
    // 护甲、核心法术与战术大招
    // =========================================================================
    constexpr uint32 FIREBALL             = 42833;  // 火球术 Rank 14 (核心读条填充技)
    constexpr uint32 PYROBLAST            = 42891;  // 炎爆术 Rank 12 (基础读条 5 秒, 战斗中严禁硬读, 仅法术连击瞬发)
    constexpr uint32 LIVING_BOMB          = 55360;  // 活动炸弹 Rank 3 (纯天赋, 60 级解锁, 核心 12s DoT + 末跳爆炸)
    constexpr uint32 SCORCH               = 42859;  // 灼烧 Rank 11 (用于维持强化灼烧 5% 法术暴击 Debuff)
    constexpr uint32 FIRE_BLAST           = 42873;  // 火焰冲击 Rank 9 (瞬发直接伤害, 8s CD)
    constexpr uint32 DRAGONS_BREATH       = 42950;  // 龙息术 Rank 6 (纯天赋, 50 级解锁, 正面锥形火焰伤害 + 昏迷, 20s CD)
    constexpr uint32 COMBUSTION           = 11129;  // 燃烧 (纯天赋, 50 级解锁, 2m CD, 火系暴击叠加, Off-GCD)
    constexpr uint32 MIRROR_IMAGE         = 55342;  // 镜像 (80 级解锁, 3m CD, 召唤分身爆发并降仇恨, Off-GCD)
    constexpr uint32 MOLTEN_ARMOR         = 43046;  // 熔甲术 Rank 3 (常驻精神转暴击护甲)
    constexpr uint32 EVOCATION            = 12051;  // 唤醒 (4m CD, 8s 长引导回蓝)
    constexpr uint32 MANA_GEM             = 42985;  // 製造法力法玉/红宝石 (2m CD, 恢复法力)
    // 法力宝石本体为「制造物品」类法术, Creature 随从无物品栏与宝石实体,
    // 常规通道会被物品所有权校验直接拒绝; 必须以下方 Replenish Mana 通道触发式注入。
    constexpr uint32 MANA_GEM_EFFECT      = 42987;  // 法力红宝石回蓝效果 (触发式直放)
    constexpr uint32 ICE_BLOCK            = 45438;  // 寒冰屏障 (5m CD, 冰箱自保)
    constexpr uint32 INVISIBILITY         = 66;     // 隐形术 (3m CD, 清仇恨脱困)
    constexpr uint32 BLINK                = 1953;   // 闪现术 (15s CD, 盲区解控位移)
    constexpr uint32 FROST_NOVA           = 42917;  // 冰霜新星 Rank 6 (近战近身定身剥离)
    constexpr uint32 COUNTERSPELL         = 2139;   // 法术反制 (24s CD, 远程压秒打断)

    // =========================================================================
    // 触发光环、被动天赋与雕文补偿
    // =========================================================================
    constexpr uint32 AURA_HOT_STREAK      = 48108;  // 法术连击触发光环 (使下一个炎爆术瞬发且不耗蓝)
    constexpr uint32 AURA_COMBUSTION      = 11129;  // 燃烧光环
    constexpr uint32 AURA_LIVING_BOMB     = 55360;  // 活动炸弹 DoT 光环
    constexpr uint32 AURA_IMPROVED_SCORCH = 22959;  // 强化灼烧 Debuff (目标受法术暴击率 +5%)
    constexpr uint32 HOT_STREAK           = 44447;  // 法术连击 Rank 3 天赋根源 (连续 2 次火系暴击触发)
    constexpr uint32 IGNITE               = 12848;  // 点燃 Rank 5 (火系暴击附加 40% 伤害的流血质量)
    constexpr uint32 FIRE_POWER           = 12842;  // 火焰强化 Rank 5 (火系法术伤害 +10%)
    constexpr uint32 CRITICAL_MASS        = 11368;  // 火焰重击 Rank 3 (火系法术暴击率 +6%)
    constexpr uint32 PYROMANIAC           = 34295;  // 纵火 Rank 3 (暴击率 +3%, 蓝耗 -3%)
    constexpr uint32 EMPOWERED_FIRE       = 31658;  // 强化火球术 Rank 3 (法伤加成提高 15%)
    constexpr uint32 IMPROVED_SCORCH      = 12873;  // 强化灼烧 Rank 3 (灼烧有 100% 几率附加 5% 易伤)
    constexpr uint32 WORLD_IN_FLAMES      = 28593;  // 烈焰世界 Rank 3 (烈焰风暴/活动炸弹等暴击 +6%)
    constexpr uint32 MOLTEN_SHIELDS       = 13043;  // 熔岩护盾 Rank 2 (防护机制)
    constexpr uint32 GLYPH_OF_FIREBALL    = 56368;  // 火球术雕文 (火球术读条缩短 0.15s)
    constexpr uint32 GLYPH_OF_LIVING_BOMB = 63091;  // 活动炸弹雕文 (活动炸弹末跳可以暴击)
    constexpr uint32 GLYPH_OF_MOLTEN_ARMOR = 56382; // 熔甲术雕文 (精神转暴击提升至 55%)
}

#endif // _FIRE_MAGE_SPELLS_H
