/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "DemonologyWarlockSpells.h"
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

class BotDemonologyWarlockAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位与射程参数 (铁律 17 迟滞区间 / 铁律 36 射程收敛)
    // -------------------------------------------------------------------------
    // 恶魔术主力通道 (暗影箭 / 烧尽 / 灵魂之火 / 献祭) 射程均为 30~35 码,
    // 无短射程填充技拘束, 故最大交战距离维持 35 码, 理想站桩位 28 码。
    // =========================================================================
    static constexpr float MIN_ENGAGE_DIST       = 15.0f;  // 安全站桩读条下限线
    static constexpr float DEADZONE_RETREAT_DIST = 8.0f;   // 近战盲区撤退进入线
    static constexpr float RETREAT_SAFE_DIST     = 16.0f;  // 撤退姿态退出安全线 (必须 > 进入线)
    static constexpr float MAX_ENGAGE_DIST       = 35.0f;  // 最大交战距离 (脱节上限)
    static constexpr float IDEAL_SHOT_DIST       = 28.0f;  // 理想施法站位

    // 献祭光环以施法者为原点的近身 8 码火焰 AoE 半径
    static constexpr float IMMOLATION_AURA_DIST  = 8.0f;

    // 撤离锚定坦克背身位时的跟随距离: 必须 >= RETREAT_SAFE_DIST。
    // 若沿用贴坦距离, 随从与 Boss 的间距仍落在 8 码近战盲区内,
    // 会持续反复触发撤退, 永远无法恢复 15 码外的施法站位。
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
    static constexpr uint32 CD_METAMORPHOSIS       = 180000;  // 恶魔变形 3 分钟 (雕文适配由底层光环时限承担)
    static constexpr uint32 CD_IMMOLATION_AURA     = 30000;   // 献祭光环 30s
    static constexpr uint32 CD_DEMONIC_EMPOWERMENT = 60000;   // 恶魔强化 1 分钟
    static constexpr uint32 CD_DEATH_COIL          = 120000;  // 死亡缠绕 2 分钟
    static constexpr uint32 CD_SOUL_SHATTER        = 300000;  // 灵魂碎裂 5 分钟
    static constexpr uint32 LIFE_TAP_RETRY         = 3000;    // 生命分流节流: 防止雕文门禁在短窗口内逐帧空转重入

    // 恶魔变形持续时间结算: 基础 30s, 持有【恶魔变形雕文】延长 6s
    static constexpr uint32 METAMORPHOSIS_BASE_DURATION_MS  = 30000;
    static constexpr uint32 METAMORPHOSIS_GLYPH_BONUS_MS    = 6000;

    // DoT 断档预判窗口 (覆盖读条 + 网络延迟)
    static constexpr int32 CORRUPTION_REFRESH_MS = 2000;
    static constexpr int32 IMMOLATE_REFRESH_MS   = 1500;
    static constexpr int32 CURSE_REFRESH_MS      = 2000;

    // 生命与法力阈值
    static constexpr float DEATH_COIL_HP_PCT    = 30.0f;  // 死亡缠绕自保血线
    static constexpr float LIFE_TAP_SAFE_HP_PCT = 50.0f;  // 生命分流安全血线 (严禁濒死自残)
    static constexpr float LIFE_TAP_MANA_PCT    = 35.0f;  // 法力枯竭补蓝线
    static constexpr float EXECUTE_HP_PCT       = 35.0f;  // 灭杀斩杀期阈值 (死亡之拥同源 35% 门禁)

public:
    explicit BotDemonologyWarlockAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 远程随从按远程单位接管移动逻辑, 禁止迈入怪物近战范围
    bool IsRangedBot() const override { return true; }

    // 法系远程 (与猎人物理远程解耦): 伤害由法术强度与暗影/火焰乘数通道支撑
    bool IsRangedPhysicalBot() const override { return false; }

    // 专精契约：恶魔术携带恶魔卫士（近战内核，与恶魔变形爆发期协同）
    GuardianVisualType GetPreferredGuardianVisualType() const override { return GUARDIAN_VISUAL_WARLOCK_FELGUARD; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注: 3.3.5a 中纯天赋技能 DBC SpellLevel 恒为 0, GetAppropriateRank 无法降阶,
    //     必须在此登记最低解锁等级并在施法前显式门禁。
    //     基础法术 (暗影箭/烧尽/魂火/献祭/腐蚀/诅咒/邪甲/分流/死缠/碎魂) 严禁登记于此。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case DemonologyWarlockSpells::DEMONIC_EMPOWERMENT: return 50;
            case DemonologyWarlockSpells::METAMORPHOSIS:       return 60;
            default:                                           return 0;
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

        if (me->GetMaxPower(POWER_MANA) > 0)
            me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));

        ResetDemonologyTimers();
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
        UpdateDemonologyTimers(diff);

        // 全局读条/引导双保险守卫 (铁律 1): 读条期间引擎置位 UNIT_STATE_CASTING,
        // 引导类法术在部分状态下不置位, 故追加 CURRENT_CHANNELED_SPELL 显式判定,
        // 杜绝长读条 (灵魂之火/暗影箭/烧尽/献祭) 被跟随走位指令当场掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (MaintainFelArmor())
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
            MaintainFelArmor();
            return;
        }

        // 远程随从仅锚定敌对目标维持进战姿态供施法链路使用,
        // 第二参数传 false 绝不开启近战追击 (CONTEXT.md 铁律)。
        if (me->GetVictim() != victim)
            me->Attack(victim, false);

        // ---- P0: 极限自保与仇恨清除 ----
        if (TrySurvival(victim)) return;

        // ---- P1: 护甲维持与控蓝分流 ----
        if (MaintainFelArmor()) return;
        if (TryManaMaintenance()) return;

        // ---- P2: 爆发大招与形态机动 (Off-GCD, 严禁 return, 必须当帧顺下) ----
        TryBurstCooldowns(victim);

        // ---- P3/P4: 核心 DoT 维持与伤害循环 ----
        if (TryDemonologyRotation(victim)) return;

        // ---- P5: 站位控制 ----
        MaintainRangedPositioning(victim);
    }

private:
    // =========================================================================
    // 自管冷却与状态登记
    // =========================================================================
    uint32 metamorphosisCooldown{ 0 };
    uint32 immolationAuraCooldown{ 0 };
    uint32 demonicEmpowermentCooldown{ 0 };
    uint32 deathCoilCooldown{ 0 };
    uint32 soulShatterCooldown{ 0 };
    uint32 lifeTapRetryTimer{ 0 };

    // 恶魔变形剩余时长 (自管结算, 用于雕文延长后的形态有效性兜底)
    uint32 metamorphosisActiveTimer{ 0 };

    // 近战盲区撤离迟滞姿态标记 (铁律 17): 必须凭此标记主动重发走位指令,
    // 否则会在脱离 8 码的瞬间被清空, 与立定分支每帧交替触发形成原地抽搐。
    bool isRetreating{ false };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateDemonologyTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(metamorphosisCooldown);
        Tick(immolationAuraCooldown);
        Tick(demonicEmpowermentCooldown);
        Tick(deathCoilCooldown);
        Tick(soulShatterCooldown);
        Tick(lifeTapRetryTimer);
        Tick(metamorphosisActiveTimer);
    }

    void ResetDemonologyTimers()
    {
        metamorphosisCooldown = 0;
        immolationAuraCooldown = 0;
        demonicEmpowermentCooldown = 0;
        deathCoilCooldown = 0;
        soulShatterCooldown = 0;
        lifeTapRetryTimer = 0;
        metamorphosisActiveTimer = 0;

        isRetreating = false;
    }

    // =========================================================================
    // 天赋契约等级门禁
    // -------------------------------------------------------------------------
    // 3.3.5a 中天赋法术的 DBC SpellLevel 恒为 0, GetAppropriateRank 不会因等级而降阶,
    // 低等级下依旧返回最高 Rank 的 ID。若不加门禁, 40 级术士会直接搓出恶魔变形。
    // 故所有在 GetTalentSpellMinLevel 登记的天赋必须经此函数解析。
    // =========================================================================
    uint32 GetTalentRank(uint32 spellId) const
    {
        if (me->GetLevel() < GetTalentSpellMinLevel(spellId))
            return 0;

        return GetAppropriateRank(spellId, true);
    }

    // =========================================================================
    // 变身状态与持续时间结算
    // =========================================================================
    bool IsInMetamorphosis() const
    {
        return me->HasAura(DemonologyWarlockSpells::AURA_METAMORPHOSIS) || metamorphosisActiveTimer > 0;
    }

    // 恶魔变形持续时间: 基础 30s, 持有【恶魔变形雕文】延长 6s (36s)
    uint32 GetMetamorphosisDurationMs() const
    {
        return METAMORPHOSIS_BASE_DURATION_MS +
               (me->HasAura(DemonologyWarlockSpells::GLYPH_OF_METAMORPHOSIS) ? METAMORPHOSIS_GLYPH_BONUS_MS : 0);
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

    // 仇恨失控判定: 敌对单位越过主坦直接盯防随从本人, 即为 OT (维度 C 归因输入信号)
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

    // 取自身施加的 DoT 剩余时间 (毫秒); 不存在则返回 0
    int32 GetOwnDotRemaining(Unit* victim, uint32 rankedSpellId) const
    {
        if (!victim)
            return 0;

        Aura* const aura = victim->GetAuraOfRankedSpell(rankedSpellId, me->GetGUID());
        return aura ? aura->GetDuration() : 0;
    }

    // =========================================================================
    // 通用施法通道 (立定判定与瞬发解耦)
    // =========================================================================
    // 非瞬发读条: CanCast 通过后立即刹停并当帧起手。
    // 严禁写成「移动中直接 return false」——随从在风筝跑位期间会判定读条永久失败,
    // 决策流被 P5 站位分支反复抢占, DoT 无限期缺席。
    // 瞬发技能 (allowMoving = true) 则完整保留机动性, 严禁刹停。
    bool TryCastSpell(Unit* victim, uint32 spellId, bool allowMoving)
    {
        if (!spellId || !victim)
            return false;

        // 施法资格必须先通过校验再刹停: CanCast 失败时提前立定,
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(victim, spellId, true))
            return false;

        if (!allowMoving && me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, spellId, true);
    }

    // DoT 维持统一通道: 以「分阶查询 + 自身 GUID」双重判定 DoT 是否存在。
    // 严禁直接用满级 ID 做 HasAura —— 低等级降阶施放时判定必然失败, 会陷入每帧空转重刷。
    // allowMoving = true 仅授予瞬发 DoT (腐蚀术 / 痛苦诅咒),
    // 献祭等读条 DoT 必须传 false 走刹停通道。
    bool TryMaintainDot(Unit* victim, uint32 rankedSpellId, uint32 castSpellId, int32 refreshWindowMs = 0, bool allowMoving = false)
    {
        if (!victim || !castSpellId)
            return false;

        if (GetOwnDotRemaining(victim, rankedSpellId) > refreshWindowMs)
            return false;

        return TryCastSpell(victim, castSpellId, allowMoving);
    }

    // =========================================================================
    // P0: 极限自保与仇恨清除 (铁律 21)
    // =========================================================================
    bool TrySurvival(Unit* victim)
    {
        // ---- 死亡缠绕: 濒死或被物理近战压制时瞬发恐惧拉开距离并回血 ----
        if (deathCoilCooldown == 0 && victim &&
            (me->GetHealthPct() < DEATH_COIL_HP_PCT || IsUnderPhysicalMelee(me)))
        {
            uint32 const deathCoil = GetAppropriateRank(DemonologyWarlockSpells::DEATH_COIL, false);
            if (deathCoil && CanCast(victim, deathCoil, true) && ExecuteSpell(victim, deathCoil, true))
            {
                deathCoilCooldown = CD_DEATH_COIL;
                return true;
            }
        }

        // ---- 灵魂碎裂: 仇恨彻底失控时瞬发削减 50% 仇恨, 把目标还给主坦 ----
        if (soulShatterCooldown == 0 && IsTopThreatTarget())
        {
            uint32 const soulShatter = GetAppropriateRank(DemonologyWarlockSpells::SOUL_SHATTER, false);
            if (soulShatter && CanCast(me, soulShatter, true) && ExecuteSpell(me, soulShatter, true))
            {
                soulShatterCooldown = CD_SOUL_SHATTER;
                return true;
            }
        }

        return false;
    }

    // =========================================================================
    // P1: 护甲维持与控蓝分流
    // =========================================================================
    bool MaintainFelArmor()
    {
        // 护甲为分阶法术: 以分阶查询判定, 避免低 Rank 光环被误判为缺失而反复顶替
        if (me->GetAuraOfRankedSpell(DemonologyWarlockSpells::FEL_ARMOR))
            return false;

        uint32 const felArmor = GetAppropriateRank(DemonologyWarlockSpells::FEL_ARMOR, false);
        if (!felArmor || !CanCast(me, felArmor, true))
            return false;

        return ExecuteSpell(me, felArmor, true);
    }

    bool TryManaMaintenance()
    {
        if (me->getPowerType() != POWER_MANA)
            return false;

        // 分流节流: 雕文门禁在短窗口内逐帧判定会形成空转重入
        if (lifeTapRetryTimer > 0)
            return false;

        float const hpPct   = me->GetHealthPct();
        float const manaPct = me->GetPowerPct(POWER_MANA);

        // 安全血线门禁: 严禁在濒死血线自残, 补蓝与维持雕文均让路给治疗
        if (hpPct <= LIFE_TAP_SAFE_HP_PCT)
            return false;

        // a) 雕文法强维持通道: 仅在【生命分流雕文】本体已注入时才启用该通道 ——
        //    若雕文缺失, 63321 光环永远不会出现, 本分支将退化为每 3 秒自残一次的无限空转。
        // b) 法力枯竭补蓝通道: 低于 35% 蓝线且血线安全 (> 50%)
        bool const hasLifeTapGlyph = me->HasAura(DemonologyWarlockSpells::GLYPH_OF_LIFE_TAP);
        bool const needGlyphBuff   = hasLifeTapGlyph &&
                                     !me->HasAura(DemonologyWarlockSpells::AURA_GLYPH_OF_LIFE_TAP);
        bool const needManaRefill  = (manaPct < LIFE_TAP_MANA_PCT);

        if (!needGlyphBuff && !needManaRefill)
            return false;

        uint32 const lifeTap = GetAppropriateRank(DemonologyWarlockSpells::LIFE_TAP, false);
        if (!lifeTap || gcdTimer > 0)
            return false;

        // 生命分流的消耗通道为「生命值」(POWER_HEALTH): CanCast / ExecuteSpell 的能量校验
        // 会对该非法能量索引执行 GetPower 读取, 存在读到邻域字段垃圾值并误判
        // 「能量不足」从而把分流永久阻断的风险。故此处彻底绕开通用施法通道,
        // 直接以引擎底层 CastSpell 直放, 并以自管蓝线/血线/GCD 三重门禁替代资源校验。
        // 必须传 triggered = true: 非触发式通道仍会在底层执行 CheckPower 校验并越界读取
        // POWER_HEALTH 字段, 只有触发式直放才能 100% 绕过能量与目标类型门禁。
        if (me->CastSpell(me, lifeTap, true) != SPELL_CAST_OK)
            return false;

        gcdTimer = 1500;
        lifeTapRetryTimer = LIFE_TAP_RETRY;
        return true;
    }

    // =========================================================================
    // P2: 爆发大招与形态机动 (Off-GCD, 当帧顺下绝不 return, 铁律 8)
    // =========================================================================
    void TryBurstCooldowns(Unit* victim)
    {
        if (!victim)
            return;

        // ---- 恶魔变形: 首领/精英战开启 3 分钟核心爆发形态 (+20% 全伤害) ----
        if (metamorphosisCooldown == 0 && IsEliteOrBossTarget(victim))
        {
            uint32 const metamorphosis = GetTalentRank(DemonologyWarlockSpells::METAMORPHOSIS);
            if (metamorphosis && !me->HasAura(DemonologyWarlockSpells::AURA_METAMORPHOSIS) &&
                CanCast(me, metamorphosis, true) && ExecuteSpell(me, metamorphosis, true))
            {
                metamorphosisCooldown = CD_METAMORPHOSIS;
                // 雕文适配: 基础 30s, 持【恶魔变形雕文】延长 6s, 用于形态有效性兜底结算
                metamorphosisActiveTimer = GetMetamorphosisDurationMs();

                // Creature 随从缺少雕文对底层光环时限的自动修饰: 雕文注入后变形光环
                // 仍会按基础 30s 到期, 必须手工把持续时间与最大持续时间同步至 36s,
                // 否则白白损失 6 秒爆发窗口与献祭光环的可用期。
                if (Aura* meta = me->GetAura(DemonologyWarlockSpells::AURA_METAMORPHOSIS))
                {
                    if (me->HasAura(DemonologyWarlockSpells::GLYPH_OF_METAMORPHOSIS))
                    {
                        meta->SetDuration(36000);
                        meta->SetMaxDuration(36000);
                    }
                }
            }
        }

        // ---- 恶魔强化: 全力增伤团队与恶魔, 1 分钟 CD ----
        // 严禁在此 return: Off-GCD 增益必须由当帧顺下的核心咒术立即消费。
        if (demonicEmpowermentCooldown == 0)
        {
            uint32 const demonicEmpowerment = GetTalentRank(DemonologyWarlockSpells::DEMONIC_EMPOWERMENT);
            if (demonicEmpowerment && CanCast(me, demonicEmpowerment, true) &&
                ExecuteSpell(me, demonicEmpowerment, true))
                demonicEmpowermentCooldown = CD_DEMONIC_EMPOWERMENT;
        }

        // ---- 献祭光环: 仅变身期间可用, 8 码内近身火焰 AoE 反制 ----
        // 以施法者为原点的自身 AoE 法术 (TARGET_UNIT_CASTER),
        // 目标类型校验会拒绝以 victim 为目标的施法请求, 故 CanCast / ExecuteSpell 必须传 me。
        // Off-GCD 瞬发, 严禁 return 打断当帧决策流。
        if (immolationAuraCooldown == 0 && IsInMetamorphosis() &&
            me->GetDistance(victim) <= IMMOLATION_AURA_DIST)
        {
            // 献祭光环为变身期专属固定法术, 严禁走 GetAppropriateRank 降阶通道 ——
            // 降阶会带回非变身期的低 Rank ID, 底层因形态/前置光环门禁直接拒放,
            // 造成每帧空转重入且永远打不出近身反制伤害。
            uint32 const immolationAura = DemonologyWarlockSpells::IMMOLATION_AURA;
            if (CanCast(me, immolationAura, true) && ExecuteSpell(me, immolationAura, true))
                immolationAuraCooldown = CD_IMMOLATION_AURA;
        }
    }

    // =========================================================================
    // P3/P4: 核心 DoT 维持与伤害循环
    // =========================================================================
    bool TryDemonologyRotation(Unit* victim)
    {
        if (!victim)
            return false;

        float const dist = me->GetDistance(victim);

        // 近战盲区与超远脱节一律交由 P0 / P5 接管, 绝不在此硬读条
        if (dist < DEADZONE_RETREAT_DIST || dist > MAX_ENGAGE_DIST)
            return false;

        // ---- P3-1. 诅咒分配 (curse 类别互斥, 只维持一种) ----
        if (TryCurse(victim))
            return true;

        // ---- P3-2. 腐蚀术: 维持熔火之心触发链路 ----
        if (TryCorruption(victim))
            return true;

        // ---- P3-3. 献祭: 读条火焰 DoT 维持 ----
        if (TryImmolate(victim))
            return true;

        // ---- P4-1. 灭杀斩杀期: 灵魂之火绝对最高优先级 ----
        if (TryDecimationSoulFire(victim))
            return true;

        // ---- P4-2. 熔火之心: 消费充能打出加速高额烧尽 ----
        if (TryMoltenCoreIncinerate(victim))
            return true;

        // ---- P4-3. 常规暗影箭填充 (兼维持团队 5% 法术暴击易伤) ----
        if (TryShadowBolt(victim))
            return true;

        return false;
    }

    // -------------------------------------------------------------------------
    // 诅咒调度 (末日灾祸 / 痛苦诅咒)
    // -------------------------------------------------------------------------
    // 末日灾祸为 60 秒单发超高伤害诅咒, 首跳与末跳时间跨度过长,
    // 只适用于血线稳定的长线首领战, 且必须「仅完全缺失时施放」——
    // 任何提前重刷都会把已累计的整段跳数连同末跳末日守卫一并吞掉。
    // 普通小怪与 35% 以下斩杀期改维持平滑的【痛苦诅咒】。
    // -------------------------------------------------------------------------
    bool TryCurse(Unit* victim)
    {
        if (!victim)
            return false;

        // 若目标身上已有未爆炸的末日灾祸, 严禁覆盖 (防止末跳巨大伤害被吞)。
        // 该门禁必须置于最前: 任何后续的末日/痛苦分支都会顶掉已累计的整段跳数,
        // 且痛苦诅咒与末日灾祸同属 curse 类别, 互相施加必定覆盖。
        if (GetOwnDotRemaining(victim, DemonologyWarlockSpells::CURSE_OF_DOOM) > 0)
            return false;

        uint32 const curseOfDoom = GetAppropriateRank(DemonologyWarlockSpells::CURSE_OF_DOOM, false);

        // 等级自适应: 末日灾祸未习得 (未满 60 级) 时严禁进入该分支,
        // 否则会取到非法/空法术 ID 造成每帧空转, 并让低等级随从彻底裸奔无诅咒。
        bool const useCurseOfDoom = (curseOfDoom != 0) && IsEliteOrBossTarget(victim) &&
                                    victim->GetHealthPct() > EXECUTE_HP_PCT;

        // 末日灾祸为瞬发诅咒, 允许风筝跑动途中直接挂上, 严禁刹停丢机动性
        if (useCurseOfDoom)
            return TryCastSpell(victim, curseOfDoom, true);

        // 痛苦诅咒为瞬发 DoT, 同样允许跑动中直接补挂
        uint32 const curseOfAgony = GetAppropriateRank(DemonologyWarlockSpells::CURSE_OF_AGONY, false);
        return TryMaintainDot(victim, DemonologyWarlockSpells::CURSE_OF_AGONY, curseOfAgony, CURSE_REFRESH_MS, true);
    }

    // -------------------------------------------------------------------------
    // 腐蚀术 (瞬发核心暗影 DoT)
    // -------------------------------------------------------------------------
    // 腐蚀术跳数是【熔火之心】天赋的唯一触发源, 必须在目标身上持续在线;
    // 判定必须绑定自身 GUID, 多术士场景下严禁把他人的腐蚀术误判为自己的,
    // 否则本随从的熔火之心触发链路将永久坏死。
    // -------------------------------------------------------------------------
    bool TryCorruption(Unit* victim)
    {
        uint32 const corruption = GetAppropriateRank(DemonologyWarlockSpells::CORRUPTION, false);
        return TryMaintainDot(victim, DemonologyWarlockSpells::CORRUPTION, corruption, CORRUPTION_REFRESH_MS, true);
    }

    // -------------------------------------------------------------------------
    // 献祭 (1.5s 读条核心火焰 DoT)
    // -------------------------------------------------------------------------
    // 撤离途中严禁站桩读条: 走位与读条会互相打断形成原地抽搐。
    // -------------------------------------------------------------------------
    bool TryImmolate(Unit* victim)
    {
        if (isRetreating || !victim)
            return false;

        // 斩杀期主动让路: 目标进入 35% 以下且【灭杀】光环在身时, 灵魂之火的单位时间伤害
        // 与读条效率全面碾压献祭, 任何补献祭的读条窗口都是在偷跑斩杀伤害。
        if (victim->GetHealthPct() <= EXECUTE_HP_PCT && me->HasAura(DemonologyWarlockSpells::AURA_DECIMATION))
            return false;

        uint32 const immolate = GetAppropriateRank(DemonologyWarlockSpells::IMMOLATE, false);
        return TryMaintainDot(victim, DemonologyWarlockSpells::IMMOLATE, immolate, IMMOLATE_REFRESH_MS, false);
    }

    // -------------------------------------------------------------------------
    // 灭杀斩杀期灵魂之火 (Decimation Execute Phase)
    // -------------------------------------------------------------------------
    // 【灭杀】使 35% 以下目标在光环持续期内将【灵魂之火】读条缩短 40% (4.0s -> 2.4s),
    // 且灵魂之火基础伤害远高于暗影箭, 是恶魔术斩杀期的绝对核心输出通道。
    // 判定必须依赖 AURA_DECIMATION (灭杀触发光环) 而非仅看目标血线:
    // 光环未触发时强读 4 秒魂火会被任何一次走位作废, 反而严重掉 DPS。
    // 若目标 <= 35% 而自身缺失光环, 则自然顺下执行烧尽/暗影箭, 以跳数摸索诱发灭杀特效。
    // -------------------------------------------------------------------------
    bool TryDecimationSoulFire(Unit* victim)
    {
        if (!victim)
            return false;

        if (victim->GetHealthPct() > EXECUTE_HP_PCT)
            return false;

        if (!me->HasAura(DemonologyWarlockSpells::AURA_DECIMATION))
            return false;

        if (isRetreating)
            return false;

        uint32 const soulFire = GetAppropriateRank(DemonologyWarlockSpells::SOUL_FIRE, false);
        return TryCastSpell(victim, soulFire, false);
    }

    // -------------------------------------------------------------------------
    // 熔火之心烧尽 (Molten Core Incinerate)
    // -------------------------------------------------------------------------
    // 熔火之心提供 3 次充能, 使【烧尽】伤害 +18% 且读条缩短 30%,
    // 是腐蚀术跳数换取的核心高收益消费窗口, 必须优先于常规暗影箭填充。
    // 烧尽对点燃目标 (献祭 DoT) 有额外加成, 故以献祭维持为前置。
    // -------------------------------------------------------------------------
    bool TryMoltenCoreIncinerate(Unit* victim)
    {
        if (!victim || !me->HasAura(DemonologyWarlockSpells::AURA_MOLTEN_CORE) || isRetreating)
            return false;

        uint32 const incinerate = GetAppropriateRank(DemonologyWarlockSpells::INCINERATE, false);
        if (!TryCastSpell(victim, incinerate, false))
            return false;

        // 随从缺乏 Player 的 SpellModOwner 机制: 烧尽消费熔火之心充能的逻辑
        // 由玩家的法术修饰器在施法成功时自动扣除, Creature 实体不会触发该回调。
        // 若不手工剥层, 3 层充能会永久挂身, 使烧尽退化为无消耗的无限加速通道。
        if (Aura* mc = me->GetAura(DemonologyWarlockSpells::AURA_MOLTEN_CORE))
            mc->DropCharge();

        return true;
    }

    // -------------------------------------------------------------------------
    // 暗影箭 (核心读条填充技)
    // -------------------------------------------------------------------------
    // 暗影箭是【暗影与烈焰】天赋的载体: 命中即为目标叠加 5% 法术暴击易伤,
    // 并同时提供 3 层暗影之拥增伤底座, 是团队法术增益的必要维持通道。
    // 注: 暗影箭射程为 30 ~ 35 码全域技能, 严禁在此叠加 MIN_ENGAGE_DIST 下限门禁 ——
    //     8 ~ 15 码区间会让暗影箭与献祭双双失效, 形成「既不读条也不前压」的人造盲区呆滞死锁。
    // -------------------------------------------------------------------------
    bool TryShadowBolt(Unit* victim)
    {
        if (!victim)
            return false;

        if (isRetreating)
            return false;

        uint32 const shadowBolt = GetAppropriateRank(DemonologyWarlockSpells::SHADOW_BOLT, false);
        return TryCastSpell(victim, shadowBolt, false);
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
        // 使读条与站位反复互相打断, 表现为原地抽搐。
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
    // 恶魔学识天赋被动光环与雕文补偿 (弥补 NPC 缺天赋树缺陷, 铁律 33)
    // -------------------------------------------------------------------------
    // 必须注入 Rank 3/5 满阶 Spell ID。若注入 DBC 默认 Rank 1 根源,
    // 触发概率与数值会严重缩水 (如熔火之心 Rank 1 触发率大幅低于满阶)。
    // 注: 灭杀/熔火之心属「触发型」被动, 必须注入天赋根源而非触发光环本身 (铁律 37),
    //     否则会退化为永久常驻或被 15s 光环自然脱落断层。
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
        SyncPassive(20, DemonologyWarlockSpells::DECIMATION);          // 灭杀 Rank 2: 目标 <= 35% 时腐蚀/献祭跳数诱发极速魂火 (63167)
        SyncPassive(20, DemonologyWarlockSpells::MOLTEN_CORE);         // 熔火之心 Rank 3: 腐蚀术跳数触发 3 层充能 (47383)
        SyncPassive(20, DemonologyWarlockSpells::DEMONIC_PACT);        // 恶魔契约 Rank 5: 全团 10% 法术强度光环
        SyncPassive(20, DemonologyWarlockSpells::DEMONIC_AEGIS);       // 恶魔庇护 Rank 3: 邪甲术效果提高 30%
        SyncPassive(20, DemonologyWarlockSpells::SHADOW_AND_FLAME);    // 暗影与烈焰 Rank 5: 暗影箭/烧尽法伤加成 +20% 并附 5% 法术暴击易伤
        SyncPassive(20, DemonologyWarlockSpells::RUIN);                // 毁灭 Rank 5: 毁灭系法术暴击伤害 +100%
        SyncPassive(20, DemonologyWarlockSpells::IMPROVED_IMMOLATE);   // 强化献祭 Rank 3: 献祭伤害 +30%

        // ---- 雕文补偿 ----
        SyncPassive(20, DemonologyWarlockSpells::GLYPH_OF_LIFE_TAP);      // 生命分流雕文: 分流后提供 SP 增益 63321
        SyncPassive(20, DemonologyWarlockSpells::GLYPH_OF_METAMORPHOSIS); // 恶魔变形雕文: 变形持续时间延长 6s
        SyncPassive(20, DemonologyWarlockSpells::GLYPH_OF_QUICK_DECAY);   // 急速凋零雕文: 急速缩短腐蚀术跳数间隔
    }
};

void AddSC_bot_demonology_warlock()
{
    new AdaptiveBotScript<BotDemonologyWarlockAI>("bot_demonology_warlock");
}
