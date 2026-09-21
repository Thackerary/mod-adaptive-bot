/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "ArcaneMageSpells.h"
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

class BotArcaneMageAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位与射程参数
    // =========================================================================
    static constexpr float MELEE_BLIND_DIST  = 8.0f;   // 近战盲区阈值：进入即触发贴身避难后撤
    static constexpr float MAX_ENGAGE_DIST   = 35.0f;  // 脱节上限：超出必须主动压进
    static constexpr float IDEAL_SHOT_DIST   = 20.0f;  // 理想施法站位 20 ~ 30 码，站位目标点取 20 码

    // 撤离锚定坦克背身位时的跟随距离：必须 >= 15 码。
    // 若沿用贴脸距离，随从与 Boss 的间距仍落在 8 ~ 12 码近战盲区内，
    // 会持续反复触发后撤，永远无法恢复 20 ~ 30 码稳定施法窗口。
    static constexpr float TANK_RETREAT_DIST = 15.0f;

    // 撤退迟滞退出线：必须严格大于 MELEE_BLIND_DIST。
    // 若进退撤退姿态共用 8 码单一阈值，会出现「刚被拉出 8 码即退出撤退并立定，
    // 下一帧又被 Boss 追进 8 码内重新触发撤退」的高频抽搐死锁。
    static constexpr float RETREAT_EXIT_DIST = 15.0f;

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」，引擎内部已叠加目标朝向。
    // 严禁自行叠加 tank->GetOrientation()，否则站位会随坦克转向持续漂移。
    // M_PI 即锚点正后方：坦克背身位，可规避顺劈斩与正面吐息。
    static constexpr float BEHIND_ANGLE      = static_cast<float>(M_PI);

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪，凡无「持续光环保护」的 CD 技能必须由专精自行计时，
    // 否则会因 HasSpellCooldown 恒 false 而在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_ARCANE_POWER     = 120000;
    static constexpr uint32 CD_PRESENCE_OF_MIND = 120000;
    static constexpr uint32 CD_EVOCATION        = 240000;
    static constexpr uint32 CD_MANA_GEM         = 120000;
    static constexpr uint32 CD_COUNTERSPELL     = 24000;
    static constexpr uint32 CD_MIRROR_IMAGE     = 180000;
    static constexpr uint32 CD_INVISIBILITY     = 180000;
    static constexpr uint32 CD_ICE_BLOCK        = 300000;
    static constexpr uint32 CD_SLOW             = 5000;
    static constexpr uint32 CD_ARCANE_BARRAGE   = 3000;  // 弹幕本体 CD：瞬发零耗蓝，不节流会被逐帧空放

    // 生命阈值
    static constexpr float ICE_BLOCK_HP_PCT    = 20.0f;
    static constexpr float MANA_SHIELD_HP_PCT  = 35.0f;

    // 法力阈值
    static constexpr float MANA_GEM_USE_PCT     = 75.0f;  // 低于此线即吞法力宝石 (尽早进入 2 分钟冷却轮转)
    static constexpr float EVOCATION_MANA_PCT   = 25.0f;  // 低于此线且宝石 CD 中才引导唤醒
    static constexpr float ARCANE_POWER_MANA_PCT = 50.0f; // 奥术强化开启所需法力安全线
    static constexpr float ARCANE_DUMP_MANA_PCT  = 85.0f; // 法力充盈泄蓝线：满层后继续奥冲压榨伤害

    // 奥术冲击叠层上限 (4 层为最高增伤与最高耗蓝的临界点)
    static constexpr uint32 ARCANE_BLAST_MAX_STACKS = 4;

    // 冰箱/隐形的硬性超时兜底 (防止解除条件永不满足时长期躺平发呆)
    static constexpr uint32 ICE_BLOCK_TIMEOUT_MS   = 2500;
    static constexpr uint32 INVISIBILITY_TIMEOUT_MS = 3000;

    // 冰箱最小保护时长与安全血线：低于该时长严禁点掉冰箱。
    // 若缺少这一层门禁，开冰箱当帧「已不再被近战压制」即成立，
    // 会在 50ms 内闪解冰箱，白交 5 分钟 CD 且当场吃满致死伤害。
    static constexpr uint32 ICE_BLOCK_MIN_HOLD_MS  = 1500;
    static constexpr float  ICE_BLOCK_SAFE_HP_PCT  = 50.0f;

    // 法力宝石的最早可习得等级：低于此等级宝石自管冷却恒为 0，
    // 唤醒若以「宝石已进入 CD」为前置将永久无法引导，形成低级唤醒死锁。
    static constexpr uint8  MANA_GEM_MIN_LEVEL     = 28;

public:
    explicit BotArcaneMageAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 远程随从按远程单位接管移动逻辑，禁止迈入怪物近战范围
    bool IsRangedBot() const override { return true; }

    // 法系远程 (与猎人物理远程解耦)：伤害由法术强度与奥术乘数通道支撑
    bool IsRangedPhysicalBot() const override { return false; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注：基础法术 (奥冲/飞弹/寒冰箭/唤醒/法师护甲/冰箱/反制等) 严禁登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case ArcaneMageSpells::PRESENCE_OF_MIND: return 30;
            case ArcaneMageSpells::ARCANE_POWER:     return 40;
            case ArcaneMageSpells::SLOW:             return 50;
            case ArcaneMageSpells::ARCANE_BARRAGE:   return 60;
            default:                                 return 0;
        }
    }

    // =========================================================================
    // 生命周期
    // =========================================================================
    void Reset() override
    {
        // 法力通道必须在基类 Reset 之前配置，以便等级同步时正确补满法力池
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

        // 全局读条/通道双保险守卫：奥术冲击等读条施法期间引擎会置位 UNIT_STATE_CASTING，
        // 但奥术飞弹/唤醒等引导类法术在部分状态下并不置位该标记，故追加
        // CURRENT_CHANNELED_SPELL 显式判定，杜绝长读条与引导被跟随/走位指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 0. 冰箱 / 隐形脱困唤醒闸门 (最高优先级，覆盖脱战与战斗双分支)
        // ---------------------------------------------------------------------
        // 寒冰屏障与隐形术在底层会持续压制随从行动 (定身/渐隐)，
        // 若不在解除条件达成时主动移除，随从会被钉死在原地发呆整个光环时限。
        // 两态期间直接 return，严禁下发任何走位/施法指令 (否则会与定身姿态互相拉扯)。
        // =====================================================================
        if (me->HasAura(ArcaneMageSpells::ICE_BLOCK))
        {
            // 已卧冰箱时长由自管冷却反算：施放成功时 iceBlockCooldown 被置为 CD_ICE_BLOCK，
            // 故 (CD_ICE_BLOCK - iceBlockCooldown) 即已定身毫秒数。
            uint32 const ibDuration = CD_ICE_BLOCK - iceBlockCooldown;

            // 三重解除门禁，缺一不可：
            // a) 最小保护时长：防止开冰箱当帧因「未被近战压制」成立而闪解自杀；
            // b) 血线已被抬起：治疗已把血线拉回安全区，可以出来继续输出；
            // c) 威胁已完全解除：既无近战压制也不再被敌对单位盯防；
            // 以及 2.5 秒硬性超时兜底：无主坦或威胁判定未命中时，
            // 解除条件可能永远无法满足，随从会躺满整个冰箱时限全程零输出。
            bool const ibMinHoldPassed = (ibDuration >= ICE_BLOCK_MIN_HOLD_MS);
            bool const ibHealedSafe    = (me->GetHealthPct() > ICE_BLOCK_SAFE_HP_PCT);
            bool const ibTimeout       = (ibDuration >= ICE_BLOCK_TIMEOUT_MS);

            if (ibTimeout || (ibMinHoldPassed && (ibHealedSafe || (!IsUnderPhysicalMelee(me) && !IsTopThreatTarget()))))
                me->RemoveAurasDueToSpell(ArcaneMageSpells::ICE_BLOCK);

            return;
        }

        if (me->HasAura(ArcaneMageSpells::INVISIBILITY))
        {
            // 隐形术在 3.3.5a 中必须跑满 3 秒淡入期，底层才会真正结算仇恨清零。
            // 若沿用「仇恨已解除即主动现身」的判定，会在渐隐尚未完成时提前点掉光环，
            // 结果是仇恨根本没被清除，随从一现身就被原样追打，白交 3 分钟底牌。
            // 故此处只保留超时放行，必须等淡入期完整跑满再恢复输出。
            if (CD_INVISIBILITY - invisibilityCooldown >= INVISIBILITY_TIMEOUT_MS)
                me->RemoveAurasDueToSpell(ArcaneMageSpells::INVISIBILITY);

            return;
        }

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (MaintainMageArmor())
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
            MaintainMageArmor();
            return;
        }

        // 远程随从仅锚定敌对目标维持进战姿态供施法链路使用，
        // 第二参数传 false 绝不开启近战追击 (CONTEXT.md 铁律)。
        if (me->GetVictim() != victim)
            me->Attack(victim, false);

        // ---- P0: 极限自保与仇恨控制 (维度 C 仇恨协同) ----
        if (TrySurvival())
            return;

        // ---- P1: 核心增益维持与续航管理 ----
        if (MaintainMageArmor())
            return;

        if (TryManaMaintenance())
            return;

        // ---- P2: 爆发与打断 (Off-GCD，严禁 return true，必须当帧顺下) ----
        TryBurstAndInterrupt(victim);

        // ---- P3: 核心奥术伤害循环 ----
        if (TryArcaneRotation(victim))
            return;

        // ---- P4: 远程站位与贴身避难 ----
        MaintainRangedPositioning(victim);
    }

private:
    // 自管冷却登记
    uint32 arcanePowerCooldown{ 0 };
    uint32 presenceOfMindCooldown{ 0 };
    uint32 evocationCooldown{ 0 };
    uint32 manaGemCooldown{ 0 };
    uint32 counterspellCooldown{ 0 };
    uint32 mirrorImageCooldown{ 0 };
    uint32 invisibilityCooldown{ 0 };
    uint32 iceBlockCooldown{ 0 };
    uint32 slowCooldown{ 0 };
    uint32 arcaneBarrageCooldown{ 0 };

    // 贴身撤离姿态标记：处于该姿态时必须凭此标记主动重发走位指令，
    // 否则会永久粘在坦克身后而无法恢复 20 ~ 30 码施法站位。
    bool isRetreatingToTank{ false };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateMageTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(arcanePowerCooldown);
        Tick(presenceOfMindCooldown);
        Tick(evocationCooldown);
        Tick(manaGemCooldown);
        Tick(counterspellCooldown);
        Tick(mirrorImageCooldown);
        Tick(invisibilityCooldown);
        Tick(iceBlockCooldown);
        Tick(slowCooldown);
        Tick(arcaneBarrageCooldown);
    }

    void ResetMageTimers()
    {
        arcanePowerCooldown = 0;
        presenceOfMindCooldown = 0;
        evocationCooldown = 0;
        manaGemCooldown = 0;
        counterspellCooldown = 0;
        mirrorImageCooldown = 0;
        invisibilityCooldown = 0;
        iceBlockCooldown = 0;
        slowCooldown = 0;
        arcaneBarrageCooldown = 0;

        isRetreatingToTank = false;
    }

    // =========================================================================
    // 天赋契约等级门禁
    // -------------------------------------------------------------------------
    // 3.3.5a 中天赋法术的 DBC SpellLevel 恒为 0，GetAppropriateRank 不会因等级而降阶，
    // 低等级下依旧返回最高 Rank 的 ID。若不加门禁，40 级法师会直接搓出奥术弹幕。
    // 故所有在 GetTalentSpellMinLevel 登记的天赋必须经此函数解析。
    // =========================================================================
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

    // 仇恨失控判定：敌对单位越过主坦直接盯防随从本人，即为 OT (维度 C 归因输入信号)
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

            // 团队存在主坦却被敌对单位盯防：判定为随从抢走仇恨
            if (hasLivingTank)
                return true;

            ++chasers;
        }

        // 无主坦兜底：被两只以上敌对单位同时盯防同样判定为仇恨失控
        return chasers >= 2;
    }

    bool IsInterruptibleTarget(Unit* target) const
    {
        if (!target || !target->IsAlive())
            return false;

        if (target->HasUnitState(UNIT_STATE_CASTING))
            return true;

        // 引导类法术不置位 UNIT_STATE_CASTING，需按通道中断标记单独判定可打断性
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

    // 奥术冲击当前叠层 (0 ~ 4)：光环挂在本随从自身
    uint32 GetArcaneBlastStacks() const
    {
        Aura* const blastAura = me->GetAura(ArcaneMageSpells::AURA_ARCANE_BLAST);
        return blastAura ? blastAura->GetStackAmount() : 0;
    }

    // =========================================================================
    // P0: 极限自保与仇恨控制
    // =========================================================================
    bool TrySurvival()
    {
        // ---- 寒冰屏障：生命濒危且被物理近战压制，冰箱硬免伤 ----
        if (iceBlockCooldown == 0 && me->GetHealthPct() < ICE_BLOCK_HP_PCT && IsUnderPhysicalMelee(me))
        {
            uint32 const iceBlock = GetAppropriateRank(ArcaneMageSpells::ICE_BLOCK, false);
            if (iceBlock && !me->HasAura(iceBlock) && CanCast(me, iceBlock, true) && ExecuteSpell(me, iceBlock, true))
            {
                iceBlockCooldown = CD_ICE_BLOCK;
                return true;
            }
        }

        // ---- 隐形术：仇恨彻底失控且有活坦可接仇恨时渐隐脱困 ----
        if (invisibilityCooldown == 0 && IsTopThreatTarget())
        {
            Unit* tank = GetGroupTank();
            bool const hasLivingTank = (tank && tank != me && tank->IsAlive() && tank->IsInWorld() && tank->GetMap() == me->GetMap());

            // 无活坦时严禁隐形：仇恨无可转移对象，渐隐结束后会被原样追打，
            // 反而白交一张 3 分钟底牌 (应改为冰箱/护盾硬抗)。
            if (hasLivingTank)
            {
                uint32 const invisibility = GetAppropriateRank(ArcaneMageSpells::INVISIBILITY, false);
                if (invisibility && !me->HasAura(invisibility) &&
                    CanCast(me, invisibility, true) && ExecuteSpell(me, invisibility, true))
                {
                    invisibilityCooldown = CD_INVISIBILITY;
                    return true;
                }
            }
        }

        // ---- 法力护盾：冰箱进入 CD 后的应急吸收盾 (防 20% ~ 35% 血线裸奔窗口) ----
        if (me->GetHealthPct() < MANA_SHIELD_HP_PCT && iceBlockCooldown > 0)
        {
            uint32 const manaShield = GetAppropriateRank(ArcaneMageSpells::MANA_SHIELD, false);
            if (manaShield && !me->HasAura(manaShield) &&
                CanCast(me, manaShield, true) && ExecuteSpell(me, manaShield, true))
            {
                return true;
            }
        }

        return false;
    }

    // =========================================================================
    // P1: 核心增益维持与续航管理
    // =========================================================================
    bool MaintainMageArmor()
    {
        // 护甲为分阶法术：以分阶查询判定，避免低 Rank 光环被误判为缺失而反复顶替
        if (me->GetAuraOfRankedSpell(ArcaneMageSpells::MAGE_ARMOR))
            return false;

        uint32 const mageArmor = GetAppropriateRank(ArcaneMageSpells::MAGE_ARMOR, false);
        if (!mageArmor || !CanCast(me, mageArmor, true))
            return false;

        return ExecuteSpell(me, mageArmor, true);
    }

    bool TryManaMaintenance()
    {
        if (me->getPowerType() != POWER_MANA)
            return false;

        float const manaPct = me->GetPowerPct(POWER_MANA);

        // ---- 法力宝石：法力枯竭时瞬发补蓝 (Off-GCD，顺下不清空当帧决策流) ----
        // 等级门禁必不可少：法力宝石在 MANA_GEM_MIN_LEVEL 以下尚未习得，
        // 若不加限制，低等级随从会偷吃这发 80 级满级宝石效果 (非法高额回蓝)，直接破坏法力平衡。
        if (manaGemCooldown == 0 && manaPct < MANA_GEM_USE_PCT && me->GetLevel() >= MANA_GEM_MIN_LEVEL)
        {
            // 法力宝石属物品触发类法术：随从没有实体宝石物品，
            // 常规施法通道会被物品所有权/目标物品校验直接拒绝，
            // 必须以 triggered 通道注入，绕过物品校验并避免占用读条与 GCD。
            me->CastSpell(me, ArcaneMageSpells::MANA_GEM_EFFECT, true);
            manaGemCooldown = CD_MANA_GEM;
        }

        // ---- 唤醒：宝石进入 CD 后的深度回蓝引导 (占用引导通道，必须阻塞当帧) ----
        // 放行条件补充「尚未习得法力宝石的等级段」：20 ~ 27 级没有宝石，
        // manaGemCooldown 恒为 0，若仅以宝石已 CD 为前置，随从法力耗尽后将永久停摆。
        bool const canEvocate = (manaGemCooldown > 0) || (me->GetLevel() < MANA_GEM_MIN_LEVEL);

        if (evocationCooldown == 0 && manaPct < EVOCATION_MANA_PCT && canEvocate &&
            !IsUnderPhysicalMelee(me))
        {
            uint32 const evocation = GetAppropriateRank(ArcaneMageSpells::EVOCATION, false);
            if (evocation && CanCast(me, evocation, true))
            {
                // 引导类法术必须先刹停：否则会被跟随/走位指令当场掐断，白白交掉 4 分钟 CD
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
    // P2: 爆发与打断 (Off-GCD，当帧顺下绝不 return)
    // =========================================================================
    void TryBurstAndInterrupt(Unit* victim)
    {
        if (!victim)
            return;

        // ---- 法术反制：接入阶段三基类记忆化压秒打断仲裁引擎 (Off-GCD) ----
        if (counterspellCooldown == 0)
        {
            uint32 const counterspell = GetAppropriateRank(ArcaneMageSpells::COUNTERSPELL, false);
            if (counterspell && TryInterrupt(victim, counterspell))
                counterspellCooldown = CD_COUNTERSPELL;
        }

        // ---- 镜像：首领/精英起手，召唤分身并压低初始仇恨 ----
        if (mirrorImageCooldown == 0 && IsEliteOrBossTarget(victim) && victim->GetHealthPct() > 50.0f)
        {
            uint32 const mirrorImage = GetAppropriateRank(ArcaneMageSpells::MIRROR_IMAGE, false);
            if (mirrorImage && !me->HasAura(mirrorImage) &&
                CanCast(me, mirrorImage, true) && ExecuteSpell(me, mirrorImage, true))
                mirrorImageCooldown = CD_MIRROR_IMAGE;
        }

        // ---- 奥术强化：首领战且法力充盈时开启，当帧顺下让奥冲立刻吃满增伤 ----
        if (arcanePowerCooldown == 0 && IsEliteOrBossTarget(victim) &&
            me->GetPowerPct(POWER_MANA) > ARCANE_POWER_MANA_PCT)
        {
            uint32 const arcanePower = GetTalentRank(ArcaneMageSpells::ARCANE_POWER);
            if (arcanePower && !me->HasAura(arcanePower) &&
                CanCast(me, arcanePower, true) && ExecuteSpell(me, arcanePower, true))
                arcanePowerCooldown = CD_ARCANE_POWER;
        }

        // ---- 减速维护：首领/精英目标常驻 SLOW，激活欺凌弱小的 12% 增伤通道 ----
        MaintainSlowDebuff(victim);

        // ---- 气定神闲：奥冲叠满 4 层且未触发飞弹速射时开启 ----
        // 必须排除飞弹速射：若开着气定又触发速射，当帧决策流会优先被免费飞弹吸走，
        // 气定光环被白白留存到后续低级填充技上，等于把 2 分钟底牌喂给了寒冰箭/弹幕。
        // 加此门禁后，气定必定用于瞬发打出一发最高伤害的 4 层奥冲。
        if (presenceOfMindCooldown == 0 && GetArcaneBlastStacks() >= ARCANE_BLAST_MAX_STACKS &&
            !me->HasAura(ArcaneMageSpells::AURA_MISSILE_BARRAGE))
        {
            uint32 const presenceOfMind = GetTalentRank(ArcaneMageSpells::PRESENCE_OF_MIND);
            if (presenceOfMind && !me->HasAura(presenceOfMind) &&
                CanCast(me, presenceOfMind, true) && ExecuteSpell(me, presenceOfMind, true))
                presenceOfMindCooldown = CD_PRESENCE_OF_MIND;

            // Off-GCD 铁律：气定光环必须由当帧顺下的奥冲立即吃掉，
            // 严禁在此 return，否则光环会被后续低级填充技白白消耗。
        }
    }

    // =========================================================================
    // P3: 核心奥术伤害循环
    // =========================================================================
    bool TryArcaneRotation(Unit* victim)
    {
        if (!victim)
            return false;

        float const dist = me->GetDistance(victim);

        // 近战盲区与超远脱节一律交由 P0 / P4 接管，绝不在此硬读条
        if (dist < MELEE_BLIND_DIST || dist > MAX_ENGAGE_DIST)
            return false;

        uint32 const arcaneBlast    = GetAppropriateRank(ArcaneMageSpells::ARCANE_BLAST, false);
        uint32 const arcaneMissiles = GetAppropriateRank(ArcaneMageSpells::ARCANE_MISSILES, false);
        uint32 const arcaneBarrage  = GetTalentRank(ArcaneMageSpells::ARCANE_BARRAGE);
        uint32 const frostbolt      = GetAppropriateRank(ArcaneMageSpells::FROSTBOLT, false);

        uint32 const stacks = GetArcaneBlastStacks();
        bool const hasBarrageProc = me->HasAura(ArcaneMageSpells::AURA_MISSILE_BARRAGE);

        // 气定神闲使奥冲当帧瞬发，是移动中唯一允许的读条豁免
        bool const pomActive = me->HasAura(ArcaneMageSpells::PRESENCE_OF_MIND);

        // =====================================================================
        // 一、已习得奥术冲击 (64 级及以上)
        // =====================================================================
        if (arcaneBlast)
        {
            // ---- 4 层满层：消层与泄蓝仲裁 ----
            if (stacks >= ARCANE_BLAST_MAX_STACKS)
            {
                // 气定神闲已开启：最优先用瞬发奥冲吃掉该光环。
                // 若放任后续的免费飞弹或瞬发弹幕先行，4 层会被提前消掉，
                // 气定只能打在 0 层的低伤奥冲上，白白浪费这发 2 分钟底牌。
                if (pomActive && TryArcaneBlast(victim, arcaneBlast, true))
                    return true;

                // 飞弹速射：零耗蓝 + 引导减半，最优消层手段
                if (hasBarrageProc && TryArcaneMissiles(victim, arcaneMissiles))
                    return true;

                // 泄蓝期 (奥术强化中 或 法力充盈)：继续奥冲极限压榨伤害，暂不消层
                bool const manaDumping = me->HasAura(ArcaneMageSpells::ARCANE_POWER) ||
                                         me->GetPowerPct(POWER_MANA) > ARCANE_DUMP_MANA_PCT;

                if (manaDumping && TryArcaneBlast(victim, arcaneBlast, pomActive))
                    return true;

                // 常规消层：瞬发弹幕清空 4 层，进入下一轮循环
                if (TryArcaneBarrage(victim, arcaneBarrage))
                    return true;

                // 弹幕未习得/冷却中的兜底：继续读条奥冲 (满层伤害仍高于空转)
                if (TryArcaneBlast(victim, arcaneBlast, pomActive))
                    return true;
            }

            // ---- 0 ~ 3 层：产层 ----
            if (TryArcaneBlast(victim, arcaneBlast, pomActive))
                return true;

            // 移动中无法站桩读条：退化为瞬发弹幕 / 减速填充
            if (TryArcaneBarrage(victim, arcaneBarrage))
                return true;

            return TrySlow(victim);
        }

        // =====================================================================
        // 二、低等级降级 (未习得奥术冲击)
        // =====================================================================
        // 飞弹速射触发即免费快打 (低等级段无奥冲，速射直接消化为零耗蓝引导)
        if (hasBarrageProc && TryArcaneMissiles(victim, arcaneMissiles))
            return true;

        // 法力充足时以奥术飞弹填充
        if (arcaneMissiles && me->GetPowerPct(POWER_MANA) > ARCANE_POWER_MANA_PCT &&
            TryArcaneMissiles(victim, arcaneMissiles))
            return true;

        // 末位兜底：寒冰箭读条填充
        if (frostbolt && TryFrostbolt(victim, frostbolt))
            return true;

        return TryArcaneBarrage(victim, arcaneBarrage);
    }

    bool TryArcaneBlast(Unit* victim, uint32 spellId, bool instantCast)
    {
        if (!spellId || !victim)
            return false;

        // 移动中严禁落地读条：读条会被后续走位指令当场掐断，白白浪费一帧决策窗口。
        // 唯一豁免是气定神闲 (instantCast = true)，此时奥冲当帧瞬发，可边跑边打。
        if (me->isMoving() && !instantCast)
            return false;

        if (!CanCast(victim, spellId, true))
            return false;

        // 气定神闲生效时该发奥冲为瞬发，严禁刹停：随从必须能边跑位边交出这发 4 层奥冲。
        // 只有真正的站桩读条才需要立定，否则会当场掐断跑位机动性，
        // 使气定这张 2 分钟底牌反而成为拖慢站位的负担。
        if (!instantCast && me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, spellId, true);
    }

    bool TryArcaneMissiles(Unit* victim, uint32 spellId)
    {
        if (!spellId || !victim)
            return false;

        // 施法资格必须先通过校验再刹停：CanCast 失败时提前立定，
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(victim, spellId, true))
            return false;

        // 引导类法术：必须立定后引导，否则会被跟随/走位指令掐断整段飞弹
        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, spellId, true);
    }

    bool TryArcaneBarrage(Unit* victim, uint32 spellId)
    {
        if (!spellId || arcaneBarrageCooldown > 0 || !victim)
            return false;

        if (!CanCast(victim, spellId, true))
            return false;

        // 瞬发无耗蓝：允许在跑位途中直接施放，严禁 StopMoving 破坏机动性
        if (ExecuteSpell(victim, spellId, true))
        {
            arcaneBarrageCooldown = CD_ARCANE_BARRAGE;
            return true;
        }

        return false;
    }

    bool TryFrostbolt(Unit* victim, uint32 spellId)
    {
        if (!spellId || !victim)
            return false;

        if (me->isMoving())
            return false;

        if (!CanCast(victim, spellId, true))
            return false;

        return ExecuteSpell(victim, spellId, true);
    }

    bool TrySlow(Unit* victim)
    {
        if (slowCooldown > 0 || !victim)
            return false;

        uint32 const slow = GetTalentRank(ArcaneMageSpells::SLOW);
        if (!slow || victim->HasAura(slow))
            return false;

        if (!CanCast(victim, slow, true))
            return false;

        if (ExecuteSpell(victim, slow, true))
        {
            slowCooldown = CD_SLOW;
            return true;
        }

        return false;
    }

    // 首领/精英减速维护：瞬发 SLOW 是【欺凌弱小】12% 增伤的唯一触发器，
    // 增伤通道一旦断档，奥术冲击/飞弹/弹幕全线掉 12% 伤害。
    // 仅对首领/精英维持：小怪转火频繁，交减速只会白烧瞬发窗口。
    void MaintainSlowDebuff(Unit* victim)
    {
        // 奥术强化 15 秒核心泄蓝窗口内严禁交减速：减速本身零伤害，
        // 在此期占用 GCD 等于直接把爆发窗口从满层奥冲手里偷跑掉。
        // 强化结束后欺凌弱小的增伤链路会由本函数自动补回。
        if (me->HasAura(ArcaneMageSpells::ARCANE_POWER))
            return;

        if (!victim || slowCooldown > 0)
            return;

        if (!IsEliteOrBossTarget(victim))
            return;

        uint32 const slow = GetTalentRank(ArcaneMageSpells::SLOW);
        if (!slow || victim->HasAura(slow))
            return;

        if (!CanCast(victim, slow, true))
            return;

        // 瞬发无弹道：允许在跑位途中直接施放，严禁 StopMoving 破坏机动性
        if (ExecuteSpell(victim, slow, true))
            slowCooldown = CD_SLOW;
    }

    // =========================================================================
    // P4: 远程站位与贴身避难 (风筝与拉开状态机，维持 20 ~ 30 码施法站位)
    // =========================================================================
    void MaintainRangedPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        // ---- P0: APF 势场紧急避险 (火圈/顺劈强行接管) ----
        if (IsUnderDangerThreat(2.0f))
        {
            if (apfMoveUpdateTimer == 0 || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float nextX = 0.0f, nextY = 0.0f, nextZ = 0.0f;
                float const optDist = std::clamp(me->GetDistance(victim), 15.0f, IDEAL_SHOT_DIST);
                if (PotentialField::CalculateNextPosition(me, victim, optDist, false, false, activeDangerZones, nextX, nextY, nextZ))
                {
                    me->GetMotionMaster()->MovePoint(1, nextX, nextY, nextZ);
                    apfMoveUpdateTimer = 300;
                    return;
                }
            }
            else
            {
                return;
            }
        }

        me->SetFacingToObject(victim);

        float const dist = me->GetDistance(victim);
        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();

        // ---- A. 脱节过远 (> 35 码)：主动压进至理想施法站位 ----
        if (dist > MAX_ENGAGE_DIST)
        {
            isRetreatingToTank = false;

            if (moveType != CHASE_MOTION_TYPE)
                me->GetMotionMaster()->MoveChase(victim, IDEAL_SHOT_DIST);
            return;
        }

        // ---- 撤退迟滞门禁 ----
        // 进入撤退沿用 MELEE_BLIND_DIST (8 码)，退出撤退必须恢复到 RETREAT_EXIT_DIST (15 码)。
        // 严禁拆除 isRetreatingToTank 这一自我粘滞条件：它是迟滞状态的载体，
        // 一旦在脱离 8 码的瞬间就被清空，撤退姿态会与立定分支每帧交替触发，
        // 表现为随从在原地反复起步/刹停的抽搐，且永远无法进入稳定施法窗口。
        bool const needsRetreat = (dist < MELEE_BLIND_DIST) || (isRetreatingToTank && dist < RETREAT_EXIT_DIST);

        // ---- B. 贴身盲区 / 尚未拉足安全距离：向坦克背身位撤离，借坦克 AoE 仇恨把小怪拉走 ----
        if (needsRetreat)
        {
            Unit* tank = GetGroupTank();
            bool const canAnchorTank = (tank && tank != me && tank != victim && tank->IsAlive() &&
                                        tank->IsInWorld() && tank->GetMap() == me->GetMap());

            if (canAnchorTank)
            {
                if (!isRetreatingToTank || moveType != FOLLOW_MOTION_TYPE)
                {
                    isRetreatingToTank = true;
                    // angle 必须传正后方：传 0.0f 会贴在坦克脸前，被顺劈斩与正面吐息一并打死。
                    // 严禁自行叠加 tank->GetOrientation()，引擎内部已按目标朝向结算偏移。
                    me->GetMotionMaster()->MoveFollow(tank, TANK_RETREAT_DIST, BEHIND_ANGLE);
                }
                return;
            }

            // 无坦克可依：严禁对 victim 调用 MoveChase —— ChaseMovementGenerator 在
            // 施法者已位于指定距离「以内」时会原地立定，而随从此刻恰处于 8 码盲区内，
            // 结果是发出指令却纹丝不动，形成粘脸卡死。必须改用矢量外推硬位移：
            // 以 victim 为原点，沿 victim -> me 的当前向量外推至 IDEAL_SHOT_DIST 落点。
            if (!isRetreatingToTank || moveType != POINT_MOTION_TYPE)
            {
                isRetreatingToTank = true;

                float const angle = victim->GetAngle(me);
                float const retreatX = victim->GetPositionX() + IDEAL_SHOT_DIST * std::cos(angle);
                float const retreatY = victim->GetPositionY() + IDEAL_SHOT_DIST * std::sin(angle);

                me->GetMotionMaster()->MovePoint(0, retreatX, retreatY, me->GetPositionZ());
            }
            return;
        }

        // ---- C. 已回到有效射程：立定施法，清空遗留走位发生器 ----
        // 严禁放任 chase / follow 发生器常驻：残余走位会持续拉扯随从，
        // 使奥术冲击的读条与站位反复互相打断，表现为原地抽搐。
        if (moveType == CHASE_MOTION_TYPE || moveType == FOLLOW_MOTION_TYPE)
        {
            isRetreatingToTank = false;
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveIdle();
            me->StopMoving();
        }
    }

    // =========================================================================
    // 奥术天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
    // -------------------------------------------------------------------------
    // 注入等级取各天赋前置点数的近似门槛，保证任何等级段都有可用的基础被动。
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

        SyncPassive(20, ArcaneMageSpells::GLYPH_OF_ARCANE_BLAST);         // 奥术冲击雕文：奥冲增伤提升
        SyncPassive(20, ArcaneMageSpells::GLYPH_OF_ARCANE_MISSILES);      // 奥术飞弹雕文：飞弹暴击伤害提升
        SyncPassive(30, ArcaneMageSpells::ARCANE_MEDITATION);             // 奥术冥想：施法中保持 50% 回蓝
        SyncPassive(40, ArcaneMageSpells::TORMENT_THE_WEAK);              // 欺凌弱小：被减速目标伤害 +12%
        SyncPassive(40, ArcaneMageSpells::SPELL_POWER);                   // 法术能量：法术暴击伤害加成 +50%
        SyncPassive(50, ArcaneMageSpells::ARCANE_EMPOWERMENT);            // 奥术增效：奥冲伤害 +9%，团队伤害 +3%
    }
};

void AddSC_bot_arcane_mage()
{
    new AdaptiveBotScript<BotArcaneMageAI>("bot_arcane_mage");
}
