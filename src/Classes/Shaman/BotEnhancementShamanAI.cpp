/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "EnhancementShamanSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "SpellAuras.h"
#include "Spell.h"
#include "Chat.h"
#include "Random.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

class BotEnhancementShamanAI : public AdaptiveBotAI
{
public:
    explicit BotEnhancementShamanAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // 增强萨满为纯物理双持近战: 走真实装备模型结算,
    // 严禁继承法系远程的 2.0x ~ 3.3x 法伤放大乘数。
    // =========================================================================
    bool IsHealerBot() const override { return false; }
    bool IsRangedBot() const override { return false; }
    bool IsRangedPhysicalBot() const override { return false; }

    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注: 3.3.5a 中纯天赋技能 DBC SpellLevel 恒为 0, GetAppropriateRank 无法降阶,
    //     必须在此登记最低解锁等级并在施法前显式门禁, 否则低等级会 100% CanCast 失败。
    //     基础法术 (闪电之盾/震击/闪电箭/闪电链/火焰新星/图腾/风剪) 严禁登记于此。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case EnhancementShamanSpells::STORMSTRIKE:      return 40;
            case EnhancementShamanSpells::LAVA_LASH:        return 40;
            case EnhancementShamanSpells::SHAMANISTIC_RAGE: return 50;
            case EnhancementShamanSpells::FERAL_SPIRIT:     return 60;
            default:                                        return 0;
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

        buffSweepTimer = 0;
        totemCheckTimer = 0;
        ResetEnhancementTimers();
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
        UpdateEnhancementTimers(diff);

        bool const shieldSweepDue = TickSweep(buffSweepTimer, diff, BUFF_SWEEP_INTERVAL);
        bool const totemSweepDue  = TickSweep(totemCheckTimer, diff, TOTEM_CHECK_INTERVAL);

        // 全局读条/引导双保险守卫: 读条期间引擎置位 UNIT_STATE_CASTING,
        // 引导类法术在部分状态下不置位, 故追加 CURRENT_CHANNELED_SPELL 显式判定,
        // 杜绝自身上层法术被跟随移动指令 (UpdateFollowMaster / MoveFollow) 掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护 (护盾 / 图腾常驻 + 跟随)
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            if (shieldSweepDue && MaintainLightningShield())
                return;

            if (totemSweepDue && MaintainTotems())
                return;

            UpdateFollowMaster(diff);
            return;
        }

        // =====================================================================
        // 2. 索敌仲裁
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() ||
            victim->GetMap() != me->GetMap() || !me->IsValidAttackTarget(victim))
            return;

        // =====================================================================
        // 3. 双持平砍与追击接管
        // 物理近战第二参数必须传 true, 开启底层自动白字挥砍与双持平砍循环
        // =====================================================================
        if (me->GetVictim() != victim)
            me->Attack(victim, true);

        // =====================================================================
        // 4. 常驻护盾与图腾矩阵维持 (占用 GCD, 需先于打击循环判定)
        // =====================================================================
        if (shieldSweepDue && MaintainLightningShield())
            return;

        if (totemSweepDue && MaintainTotems())
            return;

        // =====================================================================
        // 5. 核心 APL 决策流
        // =====================================================================
        PerformCombatAPL(victim);

        // =====================================================================
        // 6. 背后找背站位 + 白字平砍驱动
        // 本类已完整接管 ScriptedAI::UpdateAI, 引擎不会自动驱动平砍,
        // 必须由专精末帧显式调用 DoMeleeAttackIfReady(), 否则双持双段挥砍
        // 与漩涡武器/静电震击/乱舞的 Proc 链路永久缺失。
        // =====================================================================
        MaintainMeleeBehindPositioning(victim);
        DoMeleeAttackIfReady();

        // 漩涡武器白字叠层模拟必须紧随平砍结算之后, 且严格限定近战位:
        // Creature 无武器临时附魔与 ProcFlag 事件回调, 且只有贴身时才真实产生
        // 白字挥砍判定, 否则会退化为脱离近战也能叠层的伪实现。
        if (me->IsWithinMeleeRange(victim))
            SimulateMaelstromWeaponOnWhiteHit();
    }

private:
    // =========================================================================
    // 站位参数 (铁律 16/38: 物理近战背后找背与同心圆死锁防范)
    // =========================================================================
    static constexpr float MELEE_REACH_DIST   = 4.0f;   // 近战判定区上限
    static constexpr float MELEE_COMFORT_DIST = 3.0f;   // 贴身阈值: 进入后保持平滑贴背输出
    static constexpr float MELEE_FOLLOW_DIST  = 1.5f;   // 理想站位: 目标正后方 1.5 码

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」, 引擎内部已叠加目标朝向。
    // 严禁自行叠加 victim->GetOrientation(), 否则站位会随目标转向持续漂移。
    // M_PI 即目标正后方背身位, 可规避正面顺劈、吐息与被招架加速。
    static constexpr float BEHIND_ANGLE = static_cast<float>(M_PI);

    // =========================================================================
    // 巡检与图腾矩阵节流参数
    // =========================================================================
    static constexpr uint32 BUFF_SWEEP_INTERVAL   = 3000;    // 护盾巡检周期
    static constexpr uint32 TOTEM_CHECK_INTERVAL  = 3000;    // 图腾矩阵巡检周期
    static constexpr uint32 TOTEM_CAST_INTERVAL   = 2000;    // 图腾插拔最小间隔
    static constexpr uint32 TOTEM_REFRESH_TIME    = 100000;  // 图腾矩阵重建周期
    static constexpr float  TOTEM_REPOSITION_DIST = 25.0f;   // 漂移超过此距离需重播图腾
    static constexpr uint8  TOTEM_COUNT           = 4;       // 大地 / 风怒 / 法力之泉 / 火焰
    static constexpr float  FIRE_TOTEM_AOE_RANGE  = 10.0f;   // 多目标火焰图腾切换判定半径
    static constexpr uint32 FIRE_TOTEM_DURATION_MAGMA   = 21000;  // 熔岩图腾存活模拟
    static constexpr uint32 FIRE_TOTEM_DURATION_SEARING = 60000;  // 灼热图腾存活模拟

    // =========================================================================
    // 大招自管冷却
    // Creature 不参与引擎技能 CD 追踪 (HasSpellCooldown 恒 false),
    // 凡无持续光环保护的 CD 技能必须由专精自行计时, 否则会逐帧空转重入。
    // =========================================================================
    static constexpr uint32 CD_FERAL_SPIRIT     = 180000;
    static constexpr uint32 CD_SHAMANISTIC_RAGE = 60000;
    static constexpr uint32 CD_BLOODLUST        = 300000;

    // =========================================================================
    // 打击节奏自管节流 (模拟技能 GCD/CD)
    // =========================================================================
    static constexpr uint32 CD_STORMSTRIKE = 8000;
    static constexpr uint32 CD_LAVA_LASH   = 6000;
    static constexpr uint32 CD_SHOCK       = 5000;
    static constexpr uint32 CD_FIRE_NOVA   = 10000;
    static constexpr uint32 CD_WIND_SHEAR  = 6000;

    // =========================================================================
    // 漩涡武器模拟参数 (铁律 19/42)
    // =========================================================================
    static constexpr uint32 MAELSTROM_PROC_INTERVAL = 1000;  // 白字命中模拟节流
    static constexpr uint32 MAELSTROM_PROC_CHANCE   = 25;    // 单次判定触发概率 (%)
    static constexpr uint8  MAX_MAELSTROM_STACKS    = 5;

    // =========================================================================
    // 危机与判定阈值
    // =========================================================================
    static constexpr float  EMERGENCY_HP_PCT   = 45.0f;
    static constexpr float  EMERGENCY_MANA_PCT = 30.0f;
    static constexpr float  WIND_SHEAR_RANGE   = 25.0f;
    static constexpr uint32 FLAME_SHOCK_REFRESH_WINDOW = 3000;
    static constexpr uint8  LIGHTNING_SHIELD_REFRESH_CHARGES = 2;

    // CreatureTemplate::rank: 1=精英 2=稀有精英 3=首领 4=稀有
    static constexpr uint32 CREATURE_RANK_ELITE = 1;

    // =========================================================================
    // 满阶被动天赋解锁等级 (1~79 级自强过渡平滑注入)
    // =========================================================================
    static constexpr uint8 LEVEL_GLYPH             = 20;
    static constexpr uint8 LEVEL_DUAL_WIELD_SPEC   = 20;
    static constexpr uint8 LEVEL_UNLEASHED_RAGE    = 20;
    static constexpr uint8 LEVEL_WEAPON_MASTERY    = 20;
    static constexpr uint8 LEVEL_STATIC_SHOCK      = 20;
    static constexpr uint8 LEVEL_FLURRY            = 20;
    static constexpr uint8 LEVEL_MENTAL_QUICKNESS  = 20;
    static constexpr uint8 LEVEL_SHAMANISTIC_FOCUS = 30;
    static constexpr uint8 LEVEL_MAELSTROM_WEAPON  = 30;
    static constexpr uint8 LEVEL_DUAL_WIELD        = 40;

    // =========================================================================
    // 专精自管状态
    // =========================================================================
    uint32 buffSweepTimer{ 0 };
    uint32 totemCheckTimer{ 0 };

    uint32 stormstrikeCooldown{ 0 };
    uint32 lavaLashCooldown{ 0 };
    uint32 shockCooldown{ 0 };
    uint32 fireNovaCooldown{ 0 };
    uint32 windShearCooldown{ 0 };

    uint32 shamanisticRageCooldown{ 0 };
    uint32 feralSpiritCooldown{ 0 };
    uint32 bloodlustCooldown{ 0 };

    uint32 maelstromProcCooldown{ 0 };

    // 图腾矩阵状态
    uint8  totemDeployIndex{ 0 };   // 0..TOTEM_COUNT-1 为展开游标, >=TOTEM_COUNT 表示已铺满
    bool   hasTotemPos{ false };
    float  lastTotemX{ 0.0f };
    float  lastTotemY{ 0.0f };
    float  lastTotemZ{ 0.0f };
    uint32 totemCastCooldown{ 0 };
    uint32 totemRefreshTimer{ 0 };
    uint32 fireTotemTimer{ 0 };     // 火焰图腾存活模拟 (火焰新星与灼热/熔岩 DPS 依赖)

    // 火焰图腾实际落点快照: 火焰新星以火焰图腾为原点结算 10 码范围爆破,
    // 缺少落点会导致「随从在 A 点插图腾, 随后跑到 B 点对着空气放新星」的空爆抽搐。
    float lastFireTotemX{ 0.0f };
    float lastFireTotemY{ 0.0f };
    float lastFireTotemZ{ 0.0f };

    // =========================================================================
    // 巡检计时器推进器
    // 返回 true 表示本轮周期已到并完成重置
    // =========================================================================
    static bool TickSweep(uint32& timer, uint32 diff, uint32 interval)
    {
        if (timer <= diff)
        {
            timer = interval;
            return true;
        }

        timer -= diff;
        return false;
    }

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateEnhancementTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(stormstrikeCooldown);
        Tick(lavaLashCooldown);
        Tick(shockCooldown);
        Tick(fireNovaCooldown);
        Tick(windShearCooldown);

        Tick(shamanisticRageCooldown);
        Tick(feralSpiritCooldown);
        Tick(bloodlustCooldown);

        Tick(maelstromProcCooldown);
        Tick(totemCastCooldown);
        Tick(totemRefreshTimer);
        Tick(fireTotemTimer);

        // 漩涡武器叠层模拟严禁挂载于此后台计时器: 白字挥砍只发生在近战位,
        // 在远程撤离/风筝阶段凭空叠层会制造不存在的 5 层核弹。
        // 该模拟必须下沉至 UpdateAI 末帧 DoMeleeAttackIfReady() 之后的近战位判定中。
    }

    void ResetEnhancementTimers()
    {
        stormstrikeCooldown = 0;
        lavaLashCooldown = 0;
        shockCooldown = 0;
        fireNovaCooldown = 0;
        windShearCooldown = 0;

        shamanisticRageCooldown = 0;
        feralSpiritCooldown = 0;
        bloodlustCooldown = 0;

        maelstromProcCooldown = 0;

        totemDeployIndex = 0;
        hasTotemPos = false;
        lastTotemX = 0.0f;
        lastTotemY = 0.0f;
        lastTotemZ = 0.0f;
        totemCastCooldown = 0;
        totemRefreshTimer = 0;
        fireTotemTimer = 0;
    }

    // =========================================================================
    // 状态判定器
    // =========================================================================
    // 天赋门禁: 登记在 GetTalentSpellMinLevel 的技能必须显式校验等级, 否则
    // GetAppropriateRank(..., true) 会无视等级返回最高 Rank ID, 造成「已习得」假象。
    bool HasTalent(uint32 spellId) const
    {
        uint8 const minLevel = GetTalentSpellMinLevel(spellId);
        return minLevel == 0 || me->GetLevel() >= minLevel;
    }

    bool IsBossOrEliteTarget(Unit* target) const
    {
        if (!target)
            return false;

        if (target->IsPlayer())
            return true;

        if (Creature* creature = target->ToCreature())
        {
            if (CreatureTemplate const* proto = creature->GetCreatureTemplate())
                return proto->rank >= CREATURE_RANK_ELITE;
        }

        return false;
    }

    // 可打断判定: 仅对「读条类且可被沉默打断」的敌对施法生效,
    // 避免把纯粹的物理动作/不可打断首领技一并浪费掉风剪。
    bool IsInterruptibleCast(Unit* victim) const
    {
        if (!victim)
            return false;

        // 压秒打断必须同时覆盖普通读条与长引导两类通道:
        // 仅检测 CURRENT_GENERIC_SPELL 会漏断吸血/精神鞭笞/火雨等引导类灭团技,
        // 造成风剪在 CD 内被白白浪费而敌方引导照常释放。
        Spell* casting = victim->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!casting)
            casting = victim->GetCurrentSpell(CURRENT_CHANNELED_SPELL);

        if (!casting)
            return false;

        SpellInfo const* info = casting->GetSpellInfo();
        if (!info)
            return false;

        return info->PreventionType == SPELL_PREVENTION_TYPE_SILENCE;
    }

    // =========================================================================
    // 周围可攻击敌人计数器 (跨来源去重采样: 自身 + 主人/队友 + 随从集群)
    // =========================================================================
    uint32 CountNearbyEnemies(float range)
    {
        std::vector<Unit*> enemies;

        auto Consider = [&](Unit* candidate)
        {
            if (!candidate || !candidate->IsAlive() || !candidate->IsInWorld())
                return;
            if (candidate == me || candidate->GetMap() != me->GetMap())
                return;
            if (!me->IsWithinDist(candidate, range) || !me->IsValidAttackTarget(candidate))
                return;
            if (std::find(enemies.begin(), enemies.end(), candidate) != enemies.end())
                return;

            enemies.push_back(candidate);
        };

        for (Unit* attacker : me->getAttackers())
            Consider(attacker);

        if (Player* master = GetMaster())
        {
            for (Unit* attacker : master->getAttackers())
                Consider(attacker);

            if (Unit* masterPet = master->GetPet())
            {
                for (Unit* attacker : masterPet->getAttackers())
                    Consider(attacker);
            }

            if (Group* group = master->GetGroup())
            {
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->GetSource();
                    if (!member || member == master)
                        continue;

                    for (Unit* attacker : member->getAttackers())
                        Consider(attacker);
                }
            }
        }

        return static_cast<uint32>(enemies.size());
    }

    // =========================================================================
    // 阵营自适应全团爆发口径 (部落嗜血 / 联盟英勇)
    // =========================================================================
    bool IsMasterHorde() const
    {
        Player* master = GetMaster();
        if (!master)
            return true; // 无指挥官时回落部落口径, 避免爆发通道永久死亡

        uint8 const race = master->getRace();
        return race == RACE_ORC || race == RACE_UNDEAD_PLAYER || race == RACE_TAUREN ||
               race == RACE_TROLL || race == RACE_BLOODELF;
    }

    uint32 GetBloodlustSpellId() const
    {
        return GetAppropriateRank(IsMasterHorde() ? EnhancementShamanSpells::BLOODLUST
                                                  : EnhancementShamanSpells::HEROISM, false);
    }

    // =========================================================================
    // 漩涡武器 (Maelstrom Weapon) 随从模拟补偿 (铁律 19/42)
    // -------------------------------------------------------------------------
    // Creature 缺乏玩家武器临时附魔与 ProcFlag 事件回调, 白字平砍与风暴打击/
    // 熔岩猛击命中后均无法自动叠层。此处以节流 + 概率方式手工补层,
    // 否则漩涡武器 5 层瞬发核弹通道将永久坏死。
    // =========================================================================
    void GrantMaelstromStack()
    {
        if (!me->HasAura(EnhancementShamanSpells::MAELSTROM_WEAPON))
            return;

        Aura* aura = me->GetAura(EnhancementShamanSpells::AURA_MAELSTROM_WEAPON);
        uint8 const stacks = aura ? aura->GetStackAmount() : 0;
        if (stacks >= MAX_MAELSTROM_STACKS)
            return;

        me->SetAuraStack(EnhancementShamanSpells::AURA_MAELSTROM_WEAPON, me, stacks + 1);
    }

    void SimulateMaelstromWeaponOnWhiteHit()
    {
        if (maelstromProcCooldown > 0)
            return;

        maelstromProcCooldown = MAELSTROM_PROC_INTERVAL;

        if (urand(1, 100) > MAELSTROM_PROC_CHANCE)
            return;

        GrantMaelstromStack();
    }

    void TryRollMaelstromOnSpecialAttack()
    {
        if (urand(1, 100) > MAELSTROM_PROC_CHANCE)
            return;

        GrantMaelstromStack();
    }

    // =========================================================================
    // APL 主决策流
    // =========================================================================
    void PerformCombatAPL(Unit* victim)
    {
        // ---- P0: 极限自保与续航 (Off-GCD 顺下, 严禁 return 中断当帧决策流) ----
        TryEmergencySurvival(victim);

        // ---- P1: 爆发大招 ----
        if (TryBurstCooldowns(victim))
            return;

        // ---- P2: 核心打击 FCFS ----
        PerformStrikeRotation(victim);
    }

    // =========================================================================
    // P0: 极限自保与续航
    // =========================================================================
    void TryEmergencySurvival(Unit* victim)
    {
        // ---- 萨满之怒: 濒死 / 断蓝时的核心减伤与回蓝大招 ----
        if (shamanisticRageCooldown == 0 &&
            HasTalent(EnhancementShamanSpells::SHAMANISTIC_RAGE) &&
            !me->HasAura(EnhancementShamanSpells::SHAMANISTIC_RAGE))
        {
            bool const lowHp = (me->GetHealthPct() < EMERGENCY_HP_PCT);
            bool const lowMana = (me->getPowerType() == POWER_MANA &&
                                  me->GetPowerPct(POWER_MANA) < EMERGENCY_MANA_PCT);

            if (lowHp || lowMana)
            {
                if (CanCast(me, EnhancementShamanSpells::SHAMANISTIC_RAGE, true) &&
                    ExecuteSpell(me, EnhancementShamanSpells::SHAMANISTIC_RAGE, true))
                {
                    shamanisticRageCooldown = CD_SHAMANISTIC_RAGE;
                    // 铁律 8: 自保大招严禁 return, 必须允许当帧决策流顺下
                }
            }
        }

        // ---- 风剪: 远程压秒打断 ----
        TryWindShear(victim);
    }

    bool TryWindShear(Unit* victim)
    {
        if (!victim || windShearCooldown > 0)
            return false;

        uint32 const windShear = GetAppropriateRank(EnhancementShamanSpells::WIND_SHEAR, false);
        if (!windShear)
            return false;

        // 接入阶段三基类记忆化压秒打断仲裁引擎 (Off-GCD)
        if (TryInterrupt(victim, windShear))
        {
            windShearCooldown = CD_WIND_SHEAR;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P1-a: 闪电之盾常驻维持 (充能型光环必须走 GetCharges)
    // -------------------------------------------------------------------------
    // 致命坑位: 闪电之盾底层为 ProcCharges 充能型光环, 不使用 StackAmount,
    // Aura::GetStackAmount() 恒返回 1, 用其判层会使门禁永久失效并导致每轮
    // 巡检无脑刷盾空烧 GCD。必须改走 Aura::GetCharges() 真实反映剩余次数。
    // =========================================================================
    bool MaintainLightningShield()
    {
        uint32 const lightningShield = GetAppropriateRank(EnhancementShamanSpells::LIGHTNING_SHIELD, false);
        if (!lightningShield)
            return false;

        if (Aura* aura = me->GetAura(lightningShield))
        {
            if (aura->GetCharges() > LIGHTNING_SHIELD_REFRESH_CHARGES)
                return false;
        }

        if (!CanCast(me, lightningShield, true))
            return false;

        return ExecuteSpell(me, lightningShield, true);
    }

    // =========================================================================
    // P1-b: 战斗图腾矩阵
    // =========================================================================
    uint32 GetTotemSpellForIndex(uint8 index)
    {
        switch (index)
        {
            case 0:  return GetAppropriateRank(EnhancementShamanSpells::STRENGTH_OF_EARTH_TOTEM, false);
            case 1:  return GetAppropriateRank(EnhancementShamanSpells::WINDFURY_TOTEM, false);
            case 2:  return GetAppropriateRank(EnhancementShamanSpells::MANA_SPRING_TOTEM, false);
            case 3:  return GetAppropriateRank(IsMultiTargetFireTotem()
                                                   ? EnhancementShamanSpells::MAGMA_TOTEM
                                                   : EnhancementShamanSpells::SEARING_TOTEM, false);
            default: return 0;
        }
    }

    bool IsMultiTargetFireTotem()
    {
        return CountNearbyEnemies(FIRE_TOTEM_AOE_RANGE) >= 2;
    }

    bool HasActiveFireTotem() const
    {
        return fireTotemTimer > 0;
    }

    bool MaintainTotems()
    {
        // ---------------------------------------------------------------------
        // 火焰图腾到期优先重建: 火焰新星与灼热/熔岩图腾 DPS 均强依赖其存活,
        // 若等待 100 秒矩阵全量重建周期, 火焰新星在整场战斗中将永久无法启动。
        // ---------------------------------------------------------------------
        if (hasTotemPos && totemDeployIndex >= TOTEM_COUNT && fireTotemTimer == 0)
        {
            if (totemCastCooldown > 0)
                return false;

            bool const multiTarget = IsMultiTargetFireTotem();
            uint32 const fireTotem = GetAppropriateRank(multiTarget
                                                            ? EnhancementShamanSpells::MAGMA_TOTEM
                                                            : EnhancementShamanSpells::SEARING_TOTEM, false);
            if (!fireTotem)
                return false;

            bool const casted = CanCast(me, fireTotem, true) && ExecuteSpell(me, fireTotem, true);
            totemCastCooldown = TOTEM_CAST_INTERVAL;

            if (casted)
            {
                fireTotemTimer = multiTarget ? FIRE_TOTEM_DURATION_MAGMA : FIRE_TOTEM_DURATION_SEARING;
                lastFireTotemX = me->GetPositionX();
                lastFireTotemY = me->GetPositionY();
                lastFireTotemZ = me->GetPositionZ();
            }

            return casted;
        }

        // ---------------------------------------------------------------------
        // 图腾「消失」以自管刷新计时为代理信号: 图腾 NPC 的存活无法通过可靠且
        // 稳定的引擎 API 逐槽判定, 改用「重建周期 + 位置漂移」双触发,
        // 既覆盖图腾到期自然消灭, 也覆盖跑位脱节, 且绝不会产生每帧重播死循环。
        // ---------------------------------------------------------------------
        bool const deployIncomplete = (totemDeployIndex > 0 && totemDeployIndex < TOTEM_COUNT);
        bool const drifted = hasTotemPos
                          && me->GetDistance(lastTotemX, lastTotemY, lastTotemZ) > TOTEM_REPOSITION_DIST;

        bool const needRedeploy = deployIncomplete
                               || !hasTotemPos
                               || (totemRefreshTimer == 0)
                               || drifted;

        if (!needRedeploy)
            return false;

        // 已完成矩阵但被位移/超时触发重播: 重置展开游标, 从头顺次补齐
        if (hasTotemPos && totemDeployIndex >= TOTEM_COUNT)
        {
            totemDeployIndex = 0;
            fireTotemTimer = 0;
        }

        // 平滑展开节流: 间隔不足一律不放行, 严禁单帧连续打出多个图腾
        if (totemCastCooldown > 0)
            return false;

        // 矩阵铺满: 落定坐标快照与重建周期
        if (totemDeployIndex >= TOTEM_COUNT)
        {
            hasTotemPos = true;
            lastTotemX = me->GetPositionX();
            lastTotemY = me->GetPositionY();
            lastTotemZ = me->GetPositionZ();
            totemRefreshTimer = TOTEM_REFRESH_TIME;
            return false;
        }

        uint8 const deployIndex = totemDeployIndex;
        uint32 const totemSpellId = GetTotemSpellForIndex(deployIndex);

        // 等级不足该图腾: 推进游标避免死循环
        if (!totemSpellId)
        {
            ++totemDeployIndex;
            return false;
        }

        bool const casted = CanCast(me, totemSpellId, true) && ExecuteSpell(me, totemSpellId, true);

        if (casted && deployIndex == 3)
        {
            fireTotemTimer = IsMultiTargetFireTotem() ? FIRE_TOTEM_DURATION_MAGMA
                                                      : FIRE_TOTEM_DURATION_SEARING;
            lastFireTotemX = me->GetPositionX();
            lastFireTotemY = me->GetPositionY();
            lastFireTotemZ = me->GetPositionZ();
        }

        // 无论成败均推进游标并压上节流: 施法失败 (缺法力/被控) 时若原地重试,
        // 会让随从在每一帧反复空转同一个图腾, 彻底饿死输出通道。
        ++totemDeployIndex;
        totemCastCooldown = TOTEM_CAST_INTERVAL;

        return casted;
    }

    // =========================================================================
    // P2: 爆发大招
    // =========================================================================
    bool TryBurstCooldowns(Unit* victim)
    {
        if (!victim || !IsBossOrEliteTarget(victim))
            return false;

        // ---- 野性狼魂: 3 分钟 CD, 双幽灵狼爆发 (自身召唤, 目标必须传 me) ----
        if (feralSpiritCooldown == 0 && HasTalent(EnhancementShamanSpells::FERAL_SPIRIT))
        {
            if (CanCast(me, EnhancementShamanSpells::FERAL_SPIRIT, true) &&
                ExecuteSpell(me, EnhancementShamanSpells::FERAL_SPIRIT, true))
            {
                feralSpiritCooldown = CD_FERAL_SPIRIT;
                return true;
            }
        }

        // ---- 嗜血 / 英勇: 全团爆发, 开局或 20% 斩杀阶段交出 ----
        return TryBloodlust(victim);
    }

    bool TryBloodlust(Unit* victim)
    {
        if (!victim || bloodlustCooldown > 0)
            return false;

        float const hpPct = victim->GetHealthPct();
        bool const opening = (hpPct > 90.0f);
        bool const execute = (hpPct <= 20.0f);
        if (!opening && !execute)
            return false;

        uint32 const bloodlustSpellId = GetBloodlustSpellId();
        if (!bloodlustSpellId || me->HasAura(bloodlustSpellId))
            return false;

        if (!CanCast(me, bloodlustSpellId, true))
            return false;

        if (ExecuteSpell(me, bloodlustSpellId, true))
        {
            bloodlustCooldown = CD_BLOODLUST;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P3: 核心打击 FCFS 优先级
    // =========================================================================
    bool PerformStrikeRotation(Unit* victim)
    {
        if (!victim)
            return false;

        // ---- 1. 漩涡武器 5 层瞬发核弹 (绝对不可在 < 5 层时读条) ----
        if (ConsumeMaelstromWeaponNuke(victim))
            return true;

        // ---- 2. 风暴打击: 主力双持打击 + 20% 自然易伤 ----
        if (PerformStormstrike(victim))
            return true;

        // ---- 3. 震击循环: 烈焰震击维持 / 大地震击吃易伤爆发 ----
        if (PerformShockRotation(victim))
            return true;

        // ---- 4. 火焰新星: 以存活火焰图腾为原点爆破 ----
        if (PerformFireNova(victim))
            return true;

        // ---- 5. 熔岩猛击: 副手火焰填充 ----
        if (PerformLavaLash(victim))
            return true;

        return false;
    }

    // =========================================================================
    // 漩涡武器 5 层瞬发核弹
    // -------------------------------------------------------------------------
    // 5 层时闪电箭/闪电链被漩涡武器减读条至瞬发, 可跑动中直接打出;
    // 未满 5 层时严禁读条 (在近战位硬读 2.5 秒等于放弃全部白字与打击节奏)。
    // =========================================================================
    bool ConsumeMaelstromWeaponNuke(Unit* victim)
    {
        if (!victim)
            return false;

        Aura* maelstrom = me->GetAura(EnhancementShamanSpells::AURA_MAELSTROM_WEAPON);
        if (!maelstrom || maelstrom->GetStackAmount() < MAX_MAELSTROM_STACKS)
            return false;

        // 周围敌人 >= 2 优先闪电链 (多目标收益), 但必须做降级兜底:
        // 低等级尚未习得闪电链, 或多个萨满随从共享同一法术 CD 时,
        // 若因多目标判定而硬走闪电链通道, CanCast 失败会让随从满 5 层原地发呆,
        // 核弹通道被彻底堵死。必须平滑回退至单体闪电箭。
        uint32 nukeSpellId = 0;
        if (IsMultiTargetFireTotem())
        {
            uint32 const chainLightning = GetAppropriateRank(EnhancementShamanSpells::CHAIN_LIGHTNING, false);
            if (chainLightning && CanCast(victim, chainLightning, true))
                nukeSpellId = chainLightning;
        }

        if (!nukeSpellId)
            nukeSpellId = GetAppropriateRank(EnhancementShamanSpells::LIGHTNING_BOLT, false);

        if (!nukeSpellId || !CanCast(victim, nukeSpellId, true))
            return false;

        if (!ExecuteSpell(victim, nukeSpellId, true))
            return false;

        // 核弹消费全部漩涡武器层数, 必须显式剥离光环
        me->RemoveAurasDueToSpell(EnhancementShamanSpells::AURA_MAELSTROM_WEAPON);
        return true;
    }

    // =========================================================================
    // 风暴打击: 双武器重击 + 20% 自然易伤, 是大地震击与闪电核弹的乘数前置
    // =========================================================================
    bool PerformStormstrike(Unit* victim)
    {
        if (!victim || stormstrikeCooldown > 0)
            return false;

        if (!HasTalent(EnhancementShamanSpells::STORMSTRIKE))
            return false;

        if (!me->IsWithinMeleeRange(victim))
            return false;

        uint32 const stormstrike = GetAppropriateRank(EnhancementShamanSpells::STORMSTRIKE, true);
        if (!stormstrike || !CanCast(victim, stormstrike, true))
            return false;

        if (ExecuteSpell(victim, stormstrike, true))
        {
            stormstrikeCooldown = CD_STORMSTRIKE;

            // 风暴打击本身即为近战打击, 命中后参与漩涡武器叠层模拟
            TryRollMaelstromOnSpecialAttack();
            return true;
        }

        return false;
    }

    // =========================================================================
    // 震击循环 (烈焰震击与大地震击共享冷却)
    // =========================================================================
    bool PerformShockRotation(Unit* victim)
    {
        if (!victim || shockCooldown > 0)
            return false;

        // 仅统计本专精自己施加的烈焰震击, 避免误判其他萨满随从的 DoT 而放弃维持
        Aura* flameShockAura = victim->GetAuraOfRankedSpell(EnhancementShamanSpells::FLAME_SHOCK, me->GetGUID());
        bool const needFlameShock = (!flameShockAura ||
                                     flameShockAura->GetDuration() <= static_cast<int32>(FLAME_SHOCK_REFRESH_WINDOW));

        if (needFlameShock)
        {
            uint32 const flameShock = GetAppropriateRank(EnhancementShamanSpells::FLAME_SHOCK, false);
            if (flameShock && CanCast(victim, flameShock, true) && ExecuteSpell(victim, flameShock, true))
            {
                shockCooldown = CD_SHOCK;
                return true;
            }
        }

        uint32 const earthShock = GetAppropriateRank(EnhancementShamanSpells::EARTH_SHOCK, false);
        if (earthShock && CanCast(victim, earthShock, true) && ExecuteSpell(victim, earthShock, true))
        {
            shockCooldown = CD_SHOCK;
            return true;
        }

        return false;
    }

    // =========================================================================
    // 火焰新星: 以存活火焰图腾为原点爆出火焰冲击波
    // -------------------------------------------------------------------------
    // 3.3.5a 中火焰新星为自身施法 (以火焰图腾为原点结算), 归属 TARGET_UNIT_CASTER;
    // 底层若判定需敌对目标则回落至当前交战目标, 双保险杜绝目标类型拒放 (铁律 40)。
    // =========================================================================
    bool PerformFireNova(Unit* victim)
    {
        if (!victim || fireNovaCooldown > 0)
            return false;

        if (!HasActiveFireTotem())
            return false;

        // 火焰新星以火焰图腾为原点结算 10 码范围爆破, 必须校验目标是否仍处于
        // 图腾爆炸半径内: 随从插完图腾后追击跑远时, 目标已被甩出爆破圈,
        // 此时施放纯属空烧 10 秒 CD 且白占一个 GCD。
        if (victim->GetDistance(lastFireTotemX, lastFireTotemY, lastFireTotemZ) > 10.0f)
            return false;

        uint32 const fireNova = GetAppropriateRank(EnhancementShamanSpells::FIRE_NOVA, false);
        if (!fireNova || !CanCast(me, fireNova, true))
            return false;

        bool const casted = ExecuteSpell(me, fireNova, true) ||
                            (victim && ExecuteSpell(victim, fireNova, true));

        if (casted)
        {
            fireNovaCooldown = CD_FIRE_NOVA;
            return true;
        }

        return false;
    }

    // =========================================================================
    // 熔岩猛击: 副手火焰伤害填充 (需装备副手武器)
    // =========================================================================
    bool PerformLavaLash(Unit* victim)
    {
        if (!victim || lavaLashCooldown > 0)
            return false;

        if (!HasTalent(EnhancementShamanSpells::LAVA_LASH))
            return false;

        if (!me->IsWithinMeleeRange(victim))
            return false;

        uint32 const lavaLash = GetAppropriateRank(EnhancementShamanSpells::LAVA_LASH, true);
        if (!lavaLash || !CanCast(victim, lavaLash, true))
            return false;

        if (ExecuteSpell(victim, lavaLash, true))
        {
            lavaLashCooldown = CD_LAVA_LASH;

            TryRollMaelstromOnSpecialAttack();
            return true;
        }

        return false;
    }

    // =========================================================================
    // P4: 物理近战背后找背站位模型
    // =========================================================================
    void MaintainMeleeBehindPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        // 铁律 38: 读条期间严禁下发任何走位指令, 否则读条会被同帧 MoveFollow 秒断
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        // ---- P0: APF 势场紧急避险 (火圈/顺劈强行接管，规避背后盲区站桩吃火) ----
        if (IsUnderDangerThreat(2.0f))
        {
            if (apfMoveUpdateTimer == 0 || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float nextX = 0.0f, nextY = 0.0f, nextZ = 0.0f;
                if (PotentialField::CalculateNextPosition(me, victim, MELEE_FOLLOW_DIST, true, false, activeDangerZones, nextX, nextY, nextZ))
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
        bool const isFollowing = (moveType == FOLLOW_MOTION_TYPE);
        bool const isChasing   = (moveType == CHASE_MOTION_TYPE);

        // 铁律 16: 怪物仇恨锚定随从本人时严禁绕后找背。
        // 怪会随随从移动实时转向, 绕背指令会让两者围绕同一圆心无限对转,
        // 形成「同心圆旋转木马」死锁, 全程贴不上背且一发技能打不出。
        if (victim->GetVictim() == me)
        {
            if (!isChasing || dist > MELEE_REACH_DIST)
                me->GetMotionMaster()->MoveChase(victim, MELEE_FOLLOW_DIST);

            return;
        }

        // ---- 已平滑贴身: 保持贴背输出, 不打断普攻节奏 ----
        if (isFollowing && dist <= MELEE_COMFORT_DIST)
            return;

        // ---- 怪物盯防主坦: 严格占住正后方 1.5 码, 规避顺劈/吐息与被招架加速 ----
        if (dist > MELEE_REACH_DIST || !isFollowing)
            me->GetMotionMaster()->MoveFollow(victim, MELEE_FOLLOW_DIST, BEHIND_ANGLE);
    }

    // =========================================================================
    // 增强天赋被动光环与雕文补偿 (弥补 NPC 缺天赋树缺陷, 铁律 33)
    // -------------------------------------------------------------------------
    // 必须注入 Rank 3/5 满阶 Spell ID。若注入 DBC 默认 Rank 1 根源, 触发概率
    // 与数值会严重缩水 (漩涡武器 Rank 1 仅 20% 触发率与极低层数上限)。
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
        SyncPassive(LEVEL_DUAL_WIELD, EnhancementShamanSpells::DUAL_WIELD);
        SyncPassive(LEVEL_DUAL_WIELD_SPEC, EnhancementShamanSpells::DUAL_WIELD_SPEC);
        SyncPassive(LEVEL_UNLEASHED_RAGE, EnhancementShamanSpells::UNLEASHED_RAGE);
        SyncPassive(LEVEL_WEAPON_MASTERY, EnhancementShamanSpells::WEAPON_MASTERY);
        SyncPassive(LEVEL_STATIC_SHOCK, EnhancementShamanSpells::STATIC_SHOCK);
        SyncPassive(LEVEL_FLURRY, EnhancementShamanSpells::FLURRY);
        SyncPassive(LEVEL_MENTAL_QUICKNESS, EnhancementShamanSpells::MENTAL_QUICKNESS);
        SyncPassive(LEVEL_SHAMANISTIC_FOCUS, EnhancementShamanSpells::SHAMANISTIC_FOCUS);
        SyncPassive(LEVEL_MAELSTROM_WEAPON, EnhancementShamanSpells::MAELSTROM_WEAPON);

        // ---- 雕文补偿 ----
        SyncPassive(LEVEL_GLYPH, EnhancementShamanSpells::GLYPH_OF_STORMSTRIKE);
        SyncPassive(LEVEL_GLYPH, EnhancementShamanSpells::GLYPH_OF_FERAL_SPIRIT);
        SyncPassive(LEVEL_GLYPH, EnhancementShamanSpells::GLYPH_OF_LIGHTNING_SHIELD);
    }
};

void AddSC_bot_enhancement_shaman()
{
    new AdaptiveBotScript<BotEnhancementShamanAI>("bot_enhancement_shaman");
}
