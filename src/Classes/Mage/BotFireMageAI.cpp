/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "FireMageSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "SpellAuras.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Chat.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

class BotFireMageAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位与射程参数 (铁律 17 迟滞区间 / 铁律 36 射程收敛)
    // -------------------------------------------------------------------------
    // 火焰法主力通道 (火球术/灼烧/龙息/火焰冲击) 与法系标准远程一致,
    // 不存在短射程填充技拘束, 故最大交战距离维持 35 码, 理想站桩位 28 码。
    // =========================================================================
    static constexpr float DEADZONE_RETREAT_DIST = 8.0f;   // 近战盲区撤退进入线
    static constexpr float MIN_ENGAGE_DIST       = 15.0f;  // 安全站桩读条下限线
    static constexpr float RETREAT_SAFE_DIST     = 16.0f;  // 撤退姿态退出安全线 (必须 > 进入线)
    static constexpr float MAX_ENGAGE_DIST       = 35.0f;  // 最大交战距离 (脱节上限)
    static constexpr float IDEAL_SHOT_DIST       = 28.0f;  // 理想施法站位

    // 龙息术锥形有效半径 (正面 8 码)
    static constexpr float DRAGONS_BREATH_DIST   = 8.0f;

    // 撤离锚定坦克背身位时的跟随距离: 必须 >= RETREAT_SAFE_DIST。
    // 若沿用 6~8 码贴坦, 随从与 Boss 的间距仍落在 8 码近战盲区内,
    // 会持续反复触发撤退与闪现, 永远无法恢复 15 码外的施法站位。
    static constexpr float TANK_RETREAT_DIST = RETREAT_SAFE_DIST;

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」, 引擎内部已叠加目标朝向。
    // 严禁自行叠加 tank->GetOrientation(), 否则站位会随坦克转向持续漂移。
    // M_PI 即锚点正后方背身位, 可规避顺劈斩与正面吐息。
    static constexpr float BEHIND_ANGLE = static_cast<float>(M_PI);

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪 (HasSpellCooldown 恒 false),
    // 凡无「持续光环保护」的 CD 技能必须由专精自行计时,
    // 否则会因 CD 判定恒假而在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_FIRE_BLAST         = 8000;
    static constexpr uint32 CD_DRAGONS_BREATH     = 20000;
    static constexpr uint32 CD_COMBUSTION         = 120000;
    static constexpr uint32 CD_MIRROR_IMAGE       = 180000;
    static constexpr uint32 CD_EVOCATION          = 240000;
    static constexpr uint32 CD_MANA_GEM           = 120000;
    static constexpr uint32 CD_ICE_BLOCK          = 300000;
    static constexpr uint32 CD_INVISIBILITY       = 180000;
    static constexpr uint32 CD_BLINK              = 15000;
    static constexpr uint32 CD_FROST_NOVA         = 25000;
    static constexpr uint32 CD_COUNTERSPELL       = 24000;

    // 生命与法力阈值
    static constexpr float ICE_BLOCK_HP_PCT         = 20.0f;
    static constexpr float FIRE_BLAST_EXECUTE_HP_PCT = 20.0f;  // 火焰冲击斩杀补刀线
    static constexpr float MANA_GEM_USE_PCT         = 75.0f;
    static constexpr float EVOCATION_MANA_PCT       = 15.0f;

    // 冰箱/隐形的硬性超时兜底 (防止解除条件永不满足时长期躺平发呆)
    static constexpr uint32 ICE_BLOCK_TIMEOUT_MS       = 2500;
    static constexpr uint32 ICE_BLOCK_MIN_HOLD_MS      = 1500;
    static constexpr float  ICE_BLOCK_SAFE_HP_PCT      = 50.0f;
    static constexpr uint32 INVISIBILITY_TIMEOUT_MS    = 3000;

    // 灼烧易伤补挂窗口 (覆盖 1.5s 读条 + 网络延迟)
    static constexpr int32 SCORCH_REFRESH_WINDOW = 5000;

    // 法力宝石的最早可习得等级: 低于此等级宝石自管冷却恒为 0,
    // 唤醒若以「宝石已进入 CD」为前置将永久无法引导, 形成低级唤醒死锁。
    static constexpr uint8 MANA_GEM_MIN_LEVEL = 28;

    // 满阶被动天赋与雕文解锁等级 (严格注入满阶 Rank 根源, 铁律 33)
    static constexpr uint8 LEVEL_GLYPH             = 20;
    static constexpr uint8 LEVEL_MOLTEN_SHIELDS    = 25;
    static constexpr uint8 LEVEL_IMPROVED_SCORCH   = 25;
    static constexpr uint8 LEVEL_IGNITE            = 30;
    static constexpr uint8 LEVEL_FIRE_POWER        = 35;
    static constexpr uint8 LEVEL_CRITICAL_MASS     = 40;
    static constexpr uint8 LEVEL_HOT_STREAK        = 40;
    static constexpr uint8 LEVEL_PYROMANIAC        = 45;
    static constexpr uint8 LEVEL_EMPOWERED_FIRE    = 45;
    static constexpr uint8 LEVEL_WORLD_IN_FLAMES   = 50;

public:
    explicit BotFireMageAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 远程随从按远程单位接管移动逻辑, 禁止迈入怪物近战范围
    bool IsRangedBot() const override { return true; }

    // 法系远程 (与猎人物理远程解耦): 伤害由法术强度与火系乘数通道支撑
    bool IsRangedPhysicalBot() const override { return false; }

    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注: 3.3.5a 中纯天赋技能 DBC SpellLevel 恒为 0, GetAppropriateRank 无法降阶,
    //     必须在此登记最低解锁等级并在施法前显式门禁。
    //     基础法术 (火球/炎爆/灼烧/火焰冲击/熔甲/唤醒/宝石/冰箱/隐形/闪现/冰环/反制) 严禁登记于此。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case FireMageSpells::COMBUSTION:      return 50;
            case FireMageSpells::DRAGONS_BREATH:  return 50;
            case FireMageSpells::LIVING_BOMB:     return 60;
            default:                              return 0;
        }
    }

    // =========================================================================
    // 生命周期
    // =========================================================================
    void Reset() override
    {
        // 法力通道必须先于基类 Reset 完成配置, 保证等级同步走法力分支
        me->setPowerType(POWER_MANA);

        AdaptiveBotAI::Reset();

        ResetMageTimers();
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 level) override
    {
        AdaptiveBotAI::OnLevelSynced(level);

        me->setPowerType(POWER_MANA);
        if (me->GetMaxPower(POWER_MANA) > 0)
            me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));

        ApplyPassiveTalents();
    }

    // =========================================================================
    // 核心决策循环
    // =========================================================================
    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);
        UpdateMageTimers(diff);

        // 全局读条/引导双保险守卫 (铁律 1): 读条期间引擎置位 UNIT_STATE_CASTING,
        // 引导类法术 (唤醒/奥术飞弹型长通道) 在部分状态下不置位,
        // 故追加 CURRENT_CHANNELED_SPELL 显式判定, 杜绝读条被跟随移动指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 0. 冰箱 / 隐形脱困闸门 (最高优先级, 覆盖脱战与战斗双分支)
        // ---------------------------------------------------------------------
        // 寒冰屏障与隐形术在底层会持续压制随从行动 (定身/渐隐),
        // 若不在解除条件达成时主动移除, 随从会被钉死在原地发呆整个光环时限。
        // 两态期间直接 return, 严禁下发任何走位/施法指令 (否则会与定身姿态互相拉扯)。
        // =====================================================================
        if (me->HasAura(FireMageSpells::ICE_BLOCK))
        {
            // 已卧冰箱时长由自管冷却反算: 施放成功时 iceBlockCooldown 被置为 CD_ICE_BLOCK,
            // 故 (CD_ICE_BLOCK - iceBlockCooldown) 即已定身毫秒数。
            uint32 const ibDuration = CD_ICE_BLOCK - iceBlockCooldown;

            // 三重解除门禁, 缺一不可 (铁律 21):
            // a) 最小保护时长: 防止开冰箱当帧因「未被近战压制」成立而闪解自杀;
            // b) 血线已被抬起: 治疗已把血线拉回安全区, 可以出来继续输出;
            // c) 威胁已完全解除: 既无近战压制也不再被敌对单位盯防;
            // 以及 2.5 秒硬性超时兜底: 无主坦或威胁判定未命中时,
            // 解除条件可能永远无法满足, 随从会躺满整个冰箱时限全程零输出。
            bool const ibMinHoldPassed = (ibDuration >= ICE_BLOCK_MIN_HOLD_MS);
            bool const ibHealedSafe    = (me->GetHealthPct() > ICE_BLOCK_SAFE_HP_PCT);
            bool const ibTimeout       = (ibDuration >= ICE_BLOCK_TIMEOUT_MS);

            if (ibTimeout || (ibMinHoldPassed && (ibHealedSafe || (!IsUnderPhysicalMelee(me) && !IsTopThreatTarget()))))
                me->RemoveAurasDueToSpell(FireMageSpells::ICE_BLOCK);

            return;
        }

        if (me->HasAura(FireMageSpells::INVISIBILITY))
        {
            // 隐形术在 3.3.5a 中必须跑满 3 秒淡入期, 底层才会真正结算仇恨清零。
            // 若沿用「仇恨已解除即主动现身」的判定, 会在渐隐尚未完成时提前点掉光环,
            // 结果是仇恨根本没被清除, 随从一现身就被原样追打, 白交 3 分钟底牌。
            // 故此处只保留超时放行, 必须等淡入期完整跑满再恢复输出。
            if (CD_INVISIBILITY - invisibilityCooldown >= INVISIBILITY_TIMEOUT_MS)
                me->RemoveAurasDueToSpell(FireMageSpells::INVISIBILITY);

            return;
        }

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (MaintainMoltenArmor())
                return;

            if (TryEngageCombat())
                return;

            UpdateFollowMaster(diff);
            return;
        }

        // =====================================================================
        // 2. 战斗内 APL
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() ||
            victim->GetMap() != me->GetMap() || !me->IsValidAttackTarget(victim))
        {
            MaintainMoltenArmor();
            return;
        }

        // 远程随从仅锚定敌对目标维持进战姿态供施法链路使用,
        // 第二参数传 false 绝不开启近战追击 (CONTEXT.md 铁律)。
        if (me->GetVictim() != victim)
            me->Attack(victim, false);

        // ---- P0: 极限自保与脱困 (铁律 21) ----
        if (TrySurvival(victim)) return;

        // ---- P1: 压秒打断 (铁律 52, Off-GCD) ----
        if (TryInterrupt(victim)) return;

        // ---- P2: 增益维持与法力续航 ----
        if (MaintainMoltenArmor()) return;
        if (TryManaMaintenance()) return;

        // ---- P3: 爆发大招 (Off-GCD, 严禁 return, 必须当帧顺下) ----
        TryBurstCooldowns(victim);

        // ---- P4: 核心火焰伤害循环 ----
        if (TryFireRotation(victim)) return;

        // ---- P5: 站位控制 ----
        MaintainRangedPositioning(victim);
    }

private:
    // =========================================================================
    // 自管冷却与状态登记
    // =========================================================================
    uint32 fireBlastCooldown{ 0 };
    uint32 dragonsBreathCooldown{ 0 };
    uint32 combustionCooldown{ 0 };
    uint32 mirrorImageCooldown{ 0 };
    uint32 evocationCooldown{ 0 };
    uint32 manaGemCooldown{ 0 };
    uint32 iceBlockCooldown{ 0 };
    uint32 invisibilityCooldown{ 0 };
    uint32 blinkCooldown{ 0 };
    uint32 frostNovaCooldown{ 0 };
    uint32 counterspellCooldown{ 0 };

    // 近战盲区撤离迟滞姿态标记 (铁律 17): 必须凭此标记主动重发走位指令,
    // 否则会在脱离 8 码的瞬间被清空, 与立定分支每帧交替触发形成原地抽搐。
    bool isRetreating{ false };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateMageTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(fireBlastCooldown);
        Tick(dragonsBreathCooldown);
        Tick(combustionCooldown);
        Tick(mirrorImageCooldown);
        Tick(evocationCooldown);
        Tick(manaGemCooldown);
        Tick(iceBlockCooldown);
        Tick(invisibilityCooldown);
        Tick(blinkCooldown);
        Tick(frostNovaCooldown);
        Tick(counterspellCooldown);
    }

    void ResetMageTimers()
    {
        fireBlastCooldown = 0;
        dragonsBreathCooldown = 0;
        combustionCooldown = 0;
        mirrorImageCooldown = 0;
        evocationCooldown = 0;
        manaGemCooldown = 0;
        iceBlockCooldown = 0;
        invisibilityCooldown = 0;
        blinkCooldown = 0;
        frostNovaCooldown = 0;
        counterspellCooldown = 0;

        isRetreating = false;
    }

    // =========================================================================
    // 天赋契约等级门禁
    // -------------------------------------------------------------------------
    // 3.3.5a 中天赋法术的 DBC SpellLevel 恒为 0, GetAppropriateRank 不会因等级而降阶,
    // 低等级下依旧返回最高 Rank 的 ID。若不加门禁, 40 级法师会直接搓出活动炸弹。
    // 故所有在 GetTalentSpellMinLevel 登记的天赋必须经此函数解析。
    // =========================================================================
    bool HasTalent(uint32 spellId) const
    {
        uint8 const minLevel = GetTalentSpellMinLevel(spellId);
        return minLevel == 0 || me->GetLevel() >= minLevel;
    }

    uint32 GetTalentRank(uint32 spellId) const
    {
        if (me->GetLevel() < GetTalentSpellMinLevel(spellId))
            return 0;

        return GetAppropriateRank(spellId, true);
    }

    // =========================================================================
    // 战场态势判定器
    // =========================================================================
    bool IsUnderPhysicalMelee(Unit* unit) const
    {
        if (!unit)
            return false;

        for (Unit* attacker : unit->getAttackers())
        {
            if (attacker && attacker->IsAlive() && attacker->GetMap() == me->GetMap() && attacker->IsWithinMeleeRange(unit))
                return true;
        }

        return false;
    }

    // 仇恨失控判定: 敌对单位越过主坦直接盯防随从本人, 即为 OT
    bool IsTopThreatTarget()
    {
        Unit* tank = GetGroupTank();
        bool const hasLivingTank = (tank && tank != me && tank->IsAlive());

        uint32 chasers = 0;
        for (Unit* attacker : me->getAttackers())
        {
            if (!attacker || !attacker->IsAlive() || attacker->GetMap() != me->GetMap())
                continue;

            if (attacker->GetVictim() != me)
                continue;

            // 团队存在主坦却被敌对单位盯防: 判定为随从抢走仇恨
            if (hasLivingTank)
                return true;

            ++chasers;
        }

        // 无主坦兜底: 被两只以上敌对单位同时盯防同样判定为仇恨失控
        return chasers >= 2;
    }

    // 压秒打断目标甄别 (铁律 52): 必须同时兼顾普通读条与长引导通道,
    // 仅检测 CURRENT_GENERIC_SPELL 会遗漏精神鞭笞、吸取生命等引导类灭团法术。
    bool IsInterruptibleTarget(Unit* target) const
    {
        if (!target || !target->IsAlive())
            return false;

        if (target->HasUnitState(UNIT_STATE_CASTING))
            return true;

        // 引导类法术不置位 UNIT_STATE_CASTING, 需按通道中断标记单独判定可打断性
        if (Spell* channeled = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return channeled->GetSpellInfo()->ChannelInterruptFlags != 0;

        return false;
    }

    bool IsEliteOrBossTarget(Unit* target) const
    {
        if (!target)
            return false;

        // PvP 场景直接视同精英
        if (target->IsPlayer())
            return true;

        if (Creature* creature = target->ToCreature())
            return creature->GetCreatureTemplate()->rank >= CREATURE_ELITE_ELITE;

        return false;
    }

    // 本随从施加的活动炸弹 DoT 是否仍在目标身上 (防剪切仲裁输入信号)
    bool HasOwnLivingBomb(Unit* victim) const
    {
        if (!victim)
            return false;

        return victim->GetAuraOfRankedSpell(FireMageSpells::LIVING_BOMB, me->GetGUID()) != nullptr;
    }

    // =========================================================================
    // P0: 极限自保与脱困 (铁律 21)
    // =========================================================================
    bool TrySurvival(Unit* victim)
    {
        // ---- 寒冰屏障: 生命濒危且被物理近战压制, 冰箱硬免伤 ----
        if (iceBlockCooldown == 0 && me->GetHealthPct() < ICE_BLOCK_HP_PCT && IsUnderPhysicalMelee(me))
        {
            uint32 const iceBlock = GetAppropriateRank(FireMageSpells::ICE_BLOCK, false);
            if (iceBlock && !me->HasAura(iceBlock) && CanCast(me, iceBlock, true) && ExecuteSpell(me, iceBlock, true))
            {
                iceBlockCooldown = CD_ICE_BLOCK;
                return true;
            }
        }

        // ---- 隐形术: 仇恨彻底失控且有活坦可接仇恨时渐隐脱困 ----
        if (invisibilityCooldown == 0 && IsTopThreatTarget())
        {
            Unit* tank = GetGroupTank();
            bool const hasLivingTank = (tank && tank != me && tank->IsAlive() && tank->IsInWorld() && tank->GetMap() == me->GetMap());

            // 无活坦时严禁隐形: 仇恨无可转移对象, 渐隐结束后会被原样追打,
            // 反而白交一张 3 分钟底牌 (应改为冰箱硬抗)。
            if (hasLivingTank)
            {
                uint32 const invisibility = GetAppropriateRank(FireMageSpells::INVISIBILITY, false);
                if (invisibility && !me->HasAura(invisibility) &&
                    CanCast(me, invisibility, true) && ExecuteSpell(me, invisibility, true))
                {
                    invisibilityCooldown = CD_INVISIBILITY;
                    return true;
                }
            }
        }

        // ---- 近战盲区脱困: 冰霜新星定身 + 闪现术背离突进 ----
        return TryMeleeEscape(victim);
    }

    bool TryMeleeEscape(Unit* victim)
    {
        if (!victim)
            return false;

        if (me->GetDistance(victim) >= DEADZONE_RETREAT_DIST)
            return false;

        // ---- 冰霜新星: 自身为原点的近身群体定身, 为闪现拉开距离争取窗口 ----
        if (frostNovaCooldown == 0)
        {
            uint32 const frostNova = GetAppropriateRank(FireMageSpells::FROST_NOVA, false);
            if (frostNova && CanCast(me, frostNova, true) && ExecuteSpell(me, frostNova, true))
            {
                frostNovaCooldown = CD_FROST_NOVA;
                isRetreating = true;
                return true;
            }
        }

        // ---- 闪现术: 必须背离目标突进, 严禁锁定朝向导致闪现扎进 Boss 怀里 ----
        if (blinkCooldown == 0)
        {
            uint32 const blink = GetAppropriateRank(FireMageSpells::BLINK, false);
            if (blink && CanCast(me, blink, true))
            {
                // victim->GetAngle(me) 即「由 victim 指向 me」的背离向量, 精确背对目标
                me->SetFacingTo(victim->GetAngle(me));

                if (ExecuteSpell(me, blink, true))
                {
                    blinkCooldown = CD_BLINK;
                    isRetreating = true;
                    return true;
                }
            }
        }

        return false;
    }

    // =========================================================================
    // P1: 压秒打断 (Off-GCD, 铁律 52)
    // =========================================================================
    bool TryInterrupt(Unit* victim)
    {
        if (counterspellCooldown > 0 || !victim)
            return false;

        if (!IsInterruptibleTarget(victim))
            return false;

        uint32 const counterspell = GetAppropriateRank(FireMageSpells::COUNTERSPELL, false);
        if (!counterspell || !CanCast(victim, counterspell, true))
            return false;

        if (ExecuteSpell(victim, counterspell, true))
        {
            counterspellCooldown = CD_COUNTERSPELL;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P2: 增益维持与法力续航
    // =========================================================================
    bool MaintainMoltenArmor()
    {
        // 护甲为分阶法术: 以分阶查询判定, 避免低 Rank 光环被误判为缺失而反复顶替
        if (me->GetAuraOfRankedSpell(FireMageSpells::MOLTEN_ARMOR))
            return false;

        uint32 const moltenArmor = GetAppropriateRank(FireMageSpells::MOLTEN_ARMOR, false);
        if (!moltenArmor || !CanCast(me, moltenArmor, true))
            return false;

        return ExecuteSpell(me, moltenArmor, true);
    }

    bool TryManaMaintenance()
    {
        if (me->getPowerType() != POWER_MANA)
            return false;

        float const manaPct = me->GetPowerPct(POWER_MANA);

        // ---- 法力红宝石: 法力枯竭时瞬发补蓝 (Off-GCD, 顺下不清空当帧决策流) ----
        // 等级门禁必不可少: 法力宝石在 MANA_GEM_MIN_LEVEL 以下尚未习得,
        // 若不加限制, 低等级随从会偷吃这发 80 级满级宝石效果 (非法高额回蓝), 直接破坏法力平衡。
        if (manaGemCooldown == 0 && manaPct <= MANA_GEM_USE_PCT && me->GetLevel() >= MANA_GEM_MIN_LEVEL)
        {
            // 法力宝石属物品触发类法术: 随从没有实体宝石物品,
            // 常规施法通道会被物品所有权/目标物品校验直接拒绝,
            // 必须以 triggered 通道注入, 绕过物品校验并避免占用读条与 GCD。
            me->CastSpell(me, FireMageSpells::MANA_GEM_EFFECT, true);
            manaGemCooldown = CD_MANA_GEM;
        }

        // ---- 唤醒: 宝石进入 CD 后的深度回蓝引导 (占用引导通道, 必须阻塞当帧) ----
        // 放行条件补充「尚未习得法力宝石的等级段」: 20 ~ 27 级没有宝石,
        // manaGemCooldown 恒为 0, 若仅以宝石已 CD 为前置, 随从法力耗尽后将永久停摆。
        bool const canEvocate = (manaGemCooldown > 0) || (me->GetLevel() < MANA_GEM_MIN_LEVEL);

        if (evocationCooldown == 0 && manaPct <= EVOCATION_MANA_PCT && canEvocate &&
            !IsUnderPhysicalMelee(me))
        {
            uint32 const evocation = GetAppropriateRank(FireMageSpells::EVOCATION, false);
            if (evocation && CanCast(me, evocation, true))
            {
                // 引导类法术必须先刹停: 否则会被跟随/走位指令当场掐断, 白白交掉 4 分钟 CD
                if (me->isMoving())
                    me->StopMoving();

                if (ExecuteSpell(me, evocation, true))
                {
                    evocationCooldown = CD_EVOCATION;
                    return true;
                }
            }
        }

        return false;
    }

    // =========================================================================
    // P3: 爆发大招 (Off-GCD, 当帧顺下绝不 return, 铁律 8)
    // =========================================================================
    void TryBurstCooldowns(Unit* victim)
    {
        if (!victim)
            return;

        // ---- 镜像: 首领/精英战起手召唤分身, 爆发同时压低初始仇恨 ----
        if (mirrorImageCooldown == 0 && IsEliteOrBossTarget(victim) && victim->GetHealthPct() > 50.0f)
        {
            uint32 const mirrorImage = GetAppropriateRank(FireMageSpells::MIRROR_IMAGE, false);
            if (mirrorImage && !me->HasAura(mirrorImage) &&
                CanCast(me, mirrorImage, true) && ExecuteSpell(me, mirrorImage, true))
                mirrorImageCooldown = CD_MIRROR_IMAGE;
        }

        // ---- 燃烧: 首领/精英且活动炸弹已覆盖时开启 ----
        // 前置要求活动炸弹覆盖是为了保证燃烧的火焰增伤窗口内,
        // 目标身上的火系 DoT 链路完整, 灼烧/火球每一跳暴击都能吃到叠层收益。
        // 严禁在此 return: Off-GCD 光环必须由当帧顺下的火焰咒术立即消费。
        if (combustionCooldown == 0 && IsEliteOrBossTarget(victim) && HasOwnLivingBomb(victim))
        {
            uint32 const combustion = GetTalentRank(FireMageSpells::COMBUSTION);
            if (combustion && !me->HasAura(combustion) &&
                CanCast(me, combustion, true) && ExecuteSpell(me, combustion, true))
                combustionCooldown = CD_COMBUSTION;
        }
    }

    // =========================================================================
    // P4: 核心火焰伤害循环
    // =========================================================================
    bool TryFireRotation(Unit* victim)
    {
        if (!victim)
            return false;

        float const dist = me->GetDistance(victim);

        // 近战盲区与超远脱节一律交由 P0 / P5 接管, 绝不在此硬读条
        if (dist < DEADZONE_RETREAT_DIST || dist > MAX_ENGAGE_DIST)
            return false;

        // ---- 1. 法术连击瞬发炎爆: 绝对最高优先级 ----
        if (TryHotStreakPyroblast(victim))
            return true;

        // ---- 2. 活动炸弹防剪切维持 ----
        if (TryLivingBomb(victim))
            return true;

        // ---- 3. 强化灼烧 5% 法术暴击易伤维持 ----
        if (TryImprovedScorch(victim))
            return true;

        // ---- 4. 龙息术: 近身锥形瘫痪控场 ----
        if (TryDragonsBreath(victim))
            return true;

        // ---- 5. 火焰冲击: 撤离途中与斩杀补刀的瞬发填充 ----
        if (TryFireBlast(victim))
            return true;

        // ---- 6. 火球术站桩填充 ----
        if (TryFireball(victim))
            return true;

        return false;
    }

    // -------------------------------------------------------------------------
    // 法术连击瞬发炎爆
    // -------------------------------------------------------------------------
    // 炎爆术基础读条 5 秒, 战斗中硬读会被任何一次走位/打断当场作废并浪费一整个 GCD。
    // 唯一合法通道是【法术连击】触发后的瞬发光环, 此处严禁放行任何硬读条分支。
    // 由于是瞬发, 允许在撤离跑位途中直接出手, 严禁 StopMoving 破坏机动性。
    // -------------------------------------------------------------------------
    bool TryHotStreakPyroblast(Unit* victim)
    {
        if (!victim)
            return false;

        if (!me->HasAura(FireMageSpells::AURA_HOT_STREAK))
            return false;

        uint32 const pyroblast = GetAppropriateRank(FireMageSpells::PYROBLAST, false);
        if (!pyroblast || !CanCast(victim, pyroblast, true))
            return false;

        return ExecuteSpell(victim, pyroblast, true);
    }

    // -------------------------------------------------------------------------
    // 活动炸弹防剪切维持
    // -------------------------------------------------------------------------
    // 活动炸弹的末跳爆炸伤害占其总伤害的绝大部分 (12s DoT + 一次范围爆炸)。
    // 若在 DoT 尚在跳动时提前刷新, 会直接顶掉已累计的剩余跳数与末跳爆炸,
    // 造成核心伤害永久吞噬。故必须硬性限制为「本随从施加的 DoT 完全缺失」才补挂。
    // -------------------------------------------------------------------------
    bool TryLivingBomb(Unit* victim)
    {
        if (!victim)
            return false;

        if (!HasTalent(FireMageSpells::LIVING_BOMB))
            return false;

        uint32 const livingBomb = GetTalentRank(FireMageSpells::LIVING_BOMB);
        if (!livingBomb)
            return false;

        // 防剪切门禁: 只要本随从的活动炸弹 DoT 仍在跳动 (剩余 > 0), 一律严禁补挂
        Aura* const dot = victim->GetAuraOfRankedSpell(FireMageSpells::LIVING_BOMB, me->GetGUID());
        if (dot && dot->GetDuration() > 0)
            return false;

        if (!CanCast(victim, livingBomb, true))
            return false;

        return ExecuteSpell(victim, livingBomb, true);
    }

    // -------------------------------------------------------------------------
    // 强化灼烧易伤维持
    // -------------------------------------------------------------------------
    // 【强化灼烧】使灼烧 100% 附加 5% 法术暴击易伤 Debuff, 是火法唯一的团队法术暴击增益。
    // 仅对首领/精英维持: 小怪转火频繁, 交灼烧读条只会白烧站桩窗口。
    // -------------------------------------------------------------------------
    bool TryImprovedScorch(Unit* victim)
    {
        if (!victim || !IsEliteOrBossTarget(victim))
            return false;

        uint32 const scorch = GetAppropriateRank(FireMageSpells::SCORCH, false);
        if (!scorch)
            return false;

        // 施法者归属鉴别: 必须确认目标身上挂的是「本随从自己施放」的灼烧易伤,
        // 队友法师的易伤会被误认为已挂, 导致本随从终生不再补易伤, 增伤链路彻底失效。
        Aura* const debuff = victim->GetAura(FireMageSpells::AURA_IMPROVED_SCORCH, me->GetGUID());
        if (debuff && debuff->GetDuration() > SCORCH_REFRESH_WINDOW)
            return false;

        // 施法资格必须先通过校验再刹停: CanCast 失败时提前立定,
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(victim, scorch, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, scorch, true);
    }

    // -------------------------------------------------------------------------
    // 龙息术 (正面锥形火焰伤害 + 瘫痪)
    // -------------------------------------------------------------------------
    // 3.3.5a 龙息术为正面锥形判定, 完全依赖施法者当前朝向结算范围。
    // 随从在风筝撤离或贴坦避难时通常背对敌对目标, 若直接施放会导致锥形落空;
    // 必须在施放判定通过后显式调用 SetFacingToObject(victim) 锁定朝向再执行施法 (铁律 35)。
    // -------------------------------------------------------------------------
    bool TryDragonsBreath(Unit* victim)
    {
        if (!victim || dragonsBreathCooldown > 0)
            return false;

        if (!HasTalent(FireMageSpells::DRAGONS_BREATH))
            return false;

        if (me->GetDistance(victim) > DRAGONS_BREATH_DIST)
            return false;

        uint32 const dragonsBreath = GetTalentRank(FireMageSpells::DRAGONS_BREATH);
        if (!dragonsBreath || !CanCast(victim, dragonsBreath, true))
            return false;

        me->SetFacingToObject(victim);

        if (ExecuteSpell(victim, dragonsBreath, true))
        {
            dragonsBreathCooldown = CD_DRAGONS_BREATH;
            return true;
        }

        return false;
    }

    // -------------------------------------------------------------------------
    // 火焰冲击 (瞬发直接伤害, 8s CD)
    // -------------------------------------------------------------------------
    // 仅用于两种场景: 撤离跑位途中无法站桩读条时的瞬发填充,
    // 以及目标进入斩杀线时的瞬发补刀。瞬发无弹道, 严禁 StopMoving 破坏机动性。
    // -------------------------------------------------------------------------
    bool TryFireBlast(Unit* victim)
    {
        if (!victim || fireBlastCooldown > 0)
            return false;

        if (!isRetreating && victim->GetHealthPct() > FIRE_BLAST_EXECUTE_HP_PCT)
            return false;

        uint32 const fireBlast = GetAppropriateRank(FireMageSpells::FIRE_BLAST, false);
        if (!fireBlast || !CanCast(victim, fireBlast, true))
            return false;

        if (ExecuteSpell(victim, fireBlast, true))
        {
            fireBlastCooldown = CD_FIRE_BLAST;
            return true;
        }

        return false;
    }

    // -------------------------------------------------------------------------
    // 火球术 (核心读条填充技)
    // -------------------------------------------------------------------------
    // 撤离途中严禁站桩读条: 走位与读条会互相打断形成原地抽搐。
    // 距离低于安全站桩下限 (15 码) 时改以瞬发链路过渡, 等待站位回正。
    // -------------------------------------------------------------------------
    bool TryFireball(Unit* victim)
    {
        if (!victim)
            return false;

        if (isRetreating)
            return false;

        if (me->GetDistance(victim) < MIN_ENGAGE_DIST)
            return false;

        uint32 const fireball = GetAppropriateRank(FireMageSpells::FIREBALL, false);
        if (!fireball)
            return false;

        // 施法资格必须先通过校验再刹停, 避免 CanCast 失败时被每帧 StopMoving 拉扯成抽搐
        if (!CanCast(victim, fireball, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, fireball, true);
    }

    // =========================================================================
    // P5: 站位控制 (盲区迟滞回正模型, 维持 15 ~ 35 码施法站位)
    // =========================================================================
    void MaintainRangedPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        me->SetFacingToObject(victim);

        float const dist = me->GetDistance(victim);
        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();

        // ---- A. 脱节过远 (> 35 码): 主动压进至理想施法站位 ----
        if (dist > MAX_ENGAGE_DIST)
        {
            isRetreating = false;

            if (moveType != CHASE_MOTION_TYPE)
                me->GetMotionMaster()->MoveChase(victim, IDEAL_SHOT_DIST);

            return;
        }

        // ---- 撤退迟滞门禁 (铁律 17) ----
        // 进入撤退沿用 DEADZONE_RETREAT_DIST (8 码), 退出撤退必须恢复到 RETREAT_SAFE_DIST (16 码)。
        // 严禁拆除 isRetreating 这一自我粘滞条件: 一旦在脱离 8 码的瞬间就被清空,
        // 撤退姿态会与立定分支每帧交替触发, 表现为原地反复起步/刹停的抽搐。
        bool const needsRetreat = (dist < DEADZONE_RETREAT_DIST) ||
                                  (isRetreating && dist < RETREAT_SAFE_DIST);

        // ---- B. 近战盲区 / 尚未拉足安全距离: 向坦克背身位撤离, 借主坦 AoE 仇恨把小怪拉走 ----
        if (needsRetreat)
        {
            isRetreating = true;

            Unit* tank = GetGroupTank();
            bool const canAnchorTank = (tank && tank != me && tank != victim && tank->IsAlive() &&
                                        tank->IsInWorld() && tank->GetMap() == me->GetMap());

            if (canAnchorTank)
            {
                if (moveType != FOLLOW_MOTION_TYPE)
                {
                    // angle 必须传正后方: 传 0.0f 会贴在坦克脸前, 被顺劈斩与正面吐息一并打死。
                    // 严禁自行叠加 tank->GetOrientation(), 引擎内部已按目标朝向结算偏移。
                    me->GetMotionMaster()->MoveFollow(tank, TANK_RETREAT_DIST, BEHIND_ANGLE);
                }
                return;
            }

            // 无坦克可依: 严禁对 victim 调用 MoveChase —— ChaseMovementGenerator 在
            // 施法者已位于指定距离「以内」时会原地立定, 而随从此刻恰处于 8 码盲区内,
            // 结果是发出指令却纹丝不动, 形成粘脸卡死。必须改用矢量外推硬位移:
            // 以 victim 为原点, 沿 victim -> me 的当前向量外推至 IDEAL_SHOT_DIST 落点。
            if (moveType != POINT_MOTION_TYPE)
            {
                float const angle = victim->GetAngle(me);
                float const retreatX = victim->GetPositionX() + IDEAL_SHOT_DIST * std::cos(angle);
                float const retreatY = victim->GetPositionY() + IDEAL_SHOT_DIST * std::sin(angle);

                me->GetMotionMaster()->MovePoint(0, retreatX, retreatY, me->GetPositionZ());
            }
            return;
        }

        // ---- C. 已回到有效射程: 立定施法, 清空遗留走位发生器 ----
        // 严禁放任 chase / follow 发生器常驻: 残余走位会持续拉扯随从,
        // 使火球术的读条与站位反复互相打断, 表现为原地抽搐。
        if (moveType == CHASE_MOTION_TYPE || moveType == FOLLOW_MOTION_TYPE)
        {
            isRetreating = false;
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveIdle();
            me->StopMoving();
        }
        else if (dist >= RETREAT_SAFE_DIST)
        {
            isRetreating = false;
        }
    }

    // =========================================================================
    // 火焰天赋被动光环与雕文补偿 (弥补 NPC 缺天赋树缺陷, 铁律 33)
    // -------------------------------------------------------------------------
    // 必须注入 Rank 3/5 满阶 Spell ID。若注入 DBC 默认 Rank 1 根源,
    // 触发概率与数值会严重缩水 (如法术连击 Rank 1 触发率大幅低于满阶)。
    // =========================================================================
    void ApplyPassiveTalents()
    {
        uint8 const level = me->GetLevel();

        auto SyncPassive = [this, level](uint8 minLevel, uint32 spellId)
        {
            if (level >= minLevel)
            {
                if (!me->HasAura(spellId))
                    me->AddAura(spellId, me);
            }
            else
            {
                me->RemoveAurasDueToSpell(spellId);
            }
        };

        // ---- 满阶被动天赋根源注入 ----
        SyncPassive(LEVEL_HOT_STREAK, FireMageSpells::HOT_STREAK);             // 法术连击 Rank 3: 连续 2 次火系暴击触发免费瞬发炎爆
        SyncPassive(LEVEL_IGNITE, FireMageSpells::IGNITE);                     // 点燃 Rank 5: 火系暴击附加 40% 伤害的流血
        SyncPassive(LEVEL_FIRE_POWER, FireMageSpells::FIRE_POWER);             // 火焰强化 Rank 5: 火系法术伤害 +10%
        SyncPassive(LEVEL_CRITICAL_MASS, FireMageSpells::CRITICAL_MASS);       // 火焰重击 Rank 3: 火系法术暴击率 +6%
        SyncPassive(LEVEL_PYROMANIAC, FireMageSpells::PYROMANIAC);             // 纵火 Rank 3: 暴击率 +3%, 蓝耗 -3%
        SyncPassive(LEVEL_EMPOWERED_FIRE, FireMageSpells::EMPOWERED_FIRE);     // 强化火球术 Rank 3: 法伤加成提高 15%
        SyncPassive(LEVEL_IMPROVED_SCORCH, FireMageSpells::IMPROVED_SCORCH);   // 强化灼烧 Rank 3: 灼烧 100% 附加 5% 易伤
        SyncPassive(LEVEL_WORLD_IN_FLAMES, FireMageSpells::WORLD_IN_FLAMES);   // 烈焰世界 Rank 3: 活动炸弹等暴击 +6%
        SyncPassive(LEVEL_MOLTEN_SHIELDS, FireMageSpells::MOLTEN_SHIELDS);     // 熔岩护盾 Rank 2

        // ---- 雕文补偿 ----
        SyncPassive(LEVEL_GLYPH, FireMageSpells::GLYPH_OF_FIREBALL);           // 火球术雕文: 读条缩短 0.15s
        SyncPassive(LEVEL_GLYPH, FireMageSpells::GLYPH_OF_LIVING_BOMB);        // 活动炸弹雕文: 末跳可以暴击
        SyncPassive(LEVEL_GLYPH, FireMageSpells::GLYPH_OF_MOLTEN_ARMOR);       // 熔甲术雕文: 精神转暴击提升至 55%
    }
};

void AddSC_bot_fire_mage()
{
    new AdaptiveBotScript<BotFireMageAI>("bot_fire_mage");
}
