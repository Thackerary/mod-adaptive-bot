/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "UnholyDeathKnightSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "SpellAuras.h"
#include "Spell.h"
#include "Chat.h"
#include <algorithm>
#include <cmath>
#include <vector>

class BotUnholyDeathKnightAI : public AdaptiveBotAI
{
public:
    explicit BotUnholyDeathKnightAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // 纯物理近战: 走真实装备模型结算, 严禁继承法系远程的 2.0x ~ 3.3x 法伤放大乘数
    // =========================================================================
    bool IsHealerBot() const override { return false; }
    bool IsRangedBot() const override { return false; }
    bool IsRangedPhysicalBot() const override { return false; }

    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case UnholyDeathKnightSpells::BONE_SHIELD:     return UnholyDeathKnightSpells::BONE_SHIELD_MIN_LEVEL;
            case UnholyDeathKnightSpells::SCOURGE_STRIKE:  return UnholyDeathKnightSpells::SCOURGE_STRIKE_MIN_LEVEL;
            case UnholyDeathKnightSpells::SUMMON_GARGOYLE: return UnholyDeathKnightSpells::SUMMON_GARGOYLE_MIN_LEVEL;
            default:                                       return 0;
        }
    }

    // =========================================================================
    // 生命周期
    // =========================================================================
    void Reset() override
    {
        // 符能通道必须先于基类 Reset 完成配置, 保证等级同步走符能分支
        me->setPowerType(POWER_RUNIC_POWER);
        me->SetMaxPower(POWER_RUNIC_POWER, MAX_RUNIC_POWER);
        me->SetPower(POWER_RUNIC_POWER, 0);

        AdaptiveBotAI::Reset();

        ResetUnholyTimers();
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 level) override
    {
        AdaptiveBotAI::OnLevelSynced(level);

        // 符能池上限固定 1000 (即 100 符能), 等级同步后严禁溢出
        me->SetMaxPower(POWER_RUNIC_POWER, MAX_RUNIC_POWER);
        if (me->GetPower(POWER_RUNIC_POWER) > MAX_RUNIC_POWER)
            me->SetPower(POWER_RUNIC_POWER, MAX_RUNIC_POWER);

        ApplyPassiveTalents();
    }

    // =========================================================================
    // 核心决策循环
    // =========================================================================
    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);
        UpdateUnholyTimers(diff);

        // 巡检计时器: 每 3 秒复核一次常驻姿态与增益
        bool presenceCheckNow = false;
        if (presenceCheckTimer <= diff)
        {
            presenceCheckTimer = PRESENCE_CHECK_INTERVAL;
            presenceCheckNow = true;
        }
        else
        {
            presenceCheckTimer -= diff;
        }

        // =====================================================================
        // 全局读条/引导守卫 (双保险: 状态位置位 + 显式通道判定)
        // =====================================================================
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护 (灵气 / 白骨之盾 / 号角常驻 + 跟随)
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            UpdateFollowMaster(diff);

            if (presenceCheckNow)
            {
                MaintainBloodPresence();
                MaintainBoneShield();
                MaintainHornOfWinter();
            }
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
        // 3. 双手武器平砍与追击接管
        // 物理近战第二参数必须传 true, 开启底层自动白字挥砍循环
        // =====================================================================
        if (me->GetVictim() != victim)
            me->Attack(victim, true);

        // =====================================================================
        // 4. 核心 APL 决策流
        // =====================================================================
        PerformCombatAPL(victim);

        // =====================================================================
        // 5. 背后找背站位 + 平砍驱动
        // 本类已完整接管 ScriptedAI::UpdateAI, 引擎不会自动驱动平砍,
        // 必须由专精末帧显式调用 DoMeleeAttackIfReady(), 否则白字挥砍
        // 与坏死/鲜血染红之刃/末日突降的 Proc 链路永久缺失。
        // =====================================================================
        MaintainMeleeBehindPositioning(victim);
        DoMeleeAttackIfReady();
    }

private:
    // =========================================================================
    // 站位参数 (铁律 16/38: 物理近战背后找背与同心圆死锁防范)
    // =========================================================================
    static constexpr float MELEE_REACH_DIST    = 4.0f;   // 近战判定区上限
    static constexpr float MELEE_COMFORT_DIST  = 3.0f;   // 贴身阈值: 进入后保持平滑贴背输出
    static constexpr float MELEE_FOLLOW_DIST   = 1.5f;   // 理想站位: 目标正后方 1.5 码

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」, 引擎内部已叠加目标朝向。
    // 严禁自行叠加 victim->GetOrientation(), 否则站位会随目标转向持续漂移。
    // M_PI 即目标正后方背身位, 可规避正面顺劈、吐息与被招架加速。
    static constexpr float BEHIND_ANGLE = static_cast<float>(M_PI);

    // =========================================================================
    // 资源池
    // 3.3.5a 符能底层按 x10 存储 (0 ~ 1000), 与基类 CanCast 的 cost *= 10 口径一致。
    // Creature 不参与玩家符文系统, 符能收入由本专精自管模拟 (见 UpdateUnholyTimers)。
    // =========================================================================
    static constexpr uint32 MAX_RUNIC_POWER                = 1000;
    static constexpr uint32 RUNIC_POWER_REGEN_INTERVAL     = 1000;
    static constexpr uint32 RUNIC_POWER_REGEN_AMOUNT       = 100; // 10 符能/秒 (模拟符文轮转产符能)
    static constexpr uint32 RUNIC_POWER_PER_SCOURGE_STRIKE = 200; // 天灾打击命中 +20 符能
    static constexpr uint32 RUNIC_POWER_PER_RUNE_STRIKE    = 100; // 鲜血打击/冰触/暗打/传染 +10 符能
    static constexpr uint32 RUNIC_POWER_PER_EMPOWER        = 250; // 符文武器增效 +25 符能
    static constexpr uint32 RUNIC_POWER_PER_HORN           = 100; // 寒冬号角 +10 符能

    // =========================================================================
    // 凋零缠绕泄能与石像鬼蓄水仲裁阈值
    // =========================================================================
    static constexpr uint32 GARGOYLE_RUNIC_POWER_REQUIRED = 600;   // 石像鬼需求 60 符能
    static constexpr uint32 GARGOYLE_STOCKPILE_WINDOW_MS  = 10000; // 天鬼 <=10s 就绪时进入蓄水窗口
    static constexpr uint32 DEATH_COIL_OVERFLOW_THRESHOLD = 800;   // 防溢能硬阈值
    static constexpr uint32 DEATH_COIL_NORMAL_THRESHOLD   = 400;   // 常态泄能阈值
    static constexpr uint32 EMPOWER_RUNIC_POWER_LOW       = 200;   // 20 符能
    static constexpr uint32 HORN_RUNIC_POWER_LOW          = 200;   // 20 符能

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪 (HasSpellCooldown 恒 false),
    // 凡无持续光环保护的 CD 技能必须由专精自行计时, 否则会逐帧空转重入。
    // =========================================================================
    static constexpr uint32 CD_SUMMON_GARGOYLE     = 180000;
    static constexpr uint32 CD_BONE_SHIELD         = 60000;
    static constexpr uint32 CD_EMPOWER_RUNE_WEAPON = 300000;
    static constexpr uint32 CD_ICEBOUND_FORTITUDE  = 120000;
    static constexpr uint32 CD_ANTI_MAGIC_SHELL    = 45000;
    static constexpr uint32 CD_HORN_OF_WINTER      = 20000;

    // 打击节奏自管节流 (模拟符文轮转: 天灾打击/鲜血打击/传染 的符文消耗节奏)
    static constexpr uint32 SCOURGE_STRIKE_COOLDOWN_MS = 3500;
    static constexpr uint32 BLOOD_STRIKE_COOLDOWN_MS   = 4000;
    static constexpr uint32 PESTILENCE_COOLDOWN_MS     = 8000;

    // 疾病/增益断档预刷新窗口: 覆盖一次 GCD 与弹道延迟
    static constexpr uint32 DISEASE_REFRESH_WINDOW_MS    = 3000;
    static constexpr uint32 DESOLATION_REFRESH_WINDOW_MS = 3000;

    static constexpr uint32 PRESENCE_CHECK_INTERVAL = 3000;

    // 危机与压力阈值
    static constexpr float  ICEBOUND_HP_PCT            = 35.0f;
    static constexpr uint32 MAGIC_PRESSURE_ENEMY_COUNT = 3;

    // CreatureTemplate::rank: 1=精英 2=稀有精英 3=首领 4=稀有
    static constexpr uint32 CREATURE_RANK_ELITE = 1;

    // =========================================================================
    // 满阶被动天赋解锁等级 (1~79 级自强过渡平滑注入)
    // =========================================================================
    static constexpr uint8 LEVEL_GLYPH              = 20;
    static constexpr uint8 LEVEL_NECROSIS           = 30;
    static constexpr uint8 LEVEL_BLOOD_CAKED_BLADE  = 30;
    static constexpr uint8 LEVEL_MASTER_OF_GHOULS   = 30;
    static constexpr uint8 LEVEL_DESOLATION         = 40;
    static constexpr uint8 LEVEL_SUDDEN_DOOM        = 40;
    static constexpr uint8 LEVEL_IMPURITY           = 45;
    static constexpr uint8 LEVEL_REAPING            = 50;
    static constexpr uint8 LEVEL_EBON_PLAGUEBRINGER = 55;
    static constexpr uint8 LEVEL_RAGE_OF_RIVENDARE  = 55;

    // =========================================================================
    // 专精自管状态
    // =========================================================================
    uint32 presenceCheckTimer{ 0 };
    uint32 boneShieldCooldown{ 0 };
    uint32 summonGargoyleCooldown{ 0 };
    uint32 empowerRuneWeaponCooldown{ 0 };
    uint32 iceboundFortitudeCooldown{ 0 };
    uint32 antiMagicShellCooldown{ 0 };
    uint32 hornOfWinterCooldown{ 0 };
    uint32 scourgeStrikeCooldown{ 0 };
    uint32 bloodStrikeCooldown{ 0 };
    uint32 pestilenceCooldown{ 0 };
    uint32 runicRegenTimer{ 0 };

    // =========================================================================
    // 计时器维护
    // =========================================================================
    void UpdateUnholyTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(boneShieldCooldown);
        Tick(summonGargoyleCooldown);
        Tick(empowerRuneWeaponCooldown);
        Tick(iceboundFortitudeCooldown);
        Tick(antiMagicShellCooldown);
        Tick(hornOfWinterCooldown);
        Tick(scourgeStrikeCooldown);
        Tick(bloodStrikeCooldown);
        Tick(pestilenceCooldown);

        // 符文轮转产符能模拟: Creature 不参与玩家符文系统, 也不会自然获得符能,
        // 此处按 1000ms 粒度补充 10 符能, 等效于三系打击技的符文消耗回能节奏。
        if (me->IsInCombat())
        {
            runicRegenTimer += diff;
            while (runicRegenTimer >= RUNIC_POWER_REGEN_INTERVAL)
            {
                runicRegenTimer -= RUNIC_POWER_REGEN_INTERVAL;
                if (me->GetPower(POWER_RUNIC_POWER) < MAX_RUNIC_POWER)
                    me->ModifyPower(POWER_RUNIC_POWER, RUNIC_POWER_REGEN_AMOUNT);
            }
        }
        else
        {
            runicRegenTimer = 0;
        }
    }

    void ResetUnholyTimers()
    {
        presenceCheckTimer = 0;
        boneShieldCooldown = 0;
        summonGargoyleCooldown = 0;
        empowerRuneWeaponCooldown = 0;
        iceboundFortitudeCooldown = 0;
        antiMagicShellCooldown = 0;
        hornOfWinterCooldown = 0;
        scourgeStrikeCooldown = 0;
        bloodStrikeCooldown = 0;
        pestilenceCooldown = 0;
        runicRegenTimer = 0;
    }

    // =========================================================================
    // 状态判定器
    // =========================================================================
    // 天赋门禁: 登记在 GetTalentSpellMinLevel 的技能必须显式校验等级, 否则
    // GetAppropriateRank(..., true) 会无视等级返回最高 Rank ID, 造成"已习得"假象。
    bool HasTalent(uint32 spellId) const
    {
        uint8 const minLevel = GetTalentSpellMinLevel(spellId);
        return minLevel == 0 || me->GetLevel() >= minLevel;
    }

    bool HasHornOfWinter() const
    {
        // 寒冬号角为分阶法术, 直接 HasAura(最高 Rank) 会漏检低等级施放的低阶光环
        return me->GetAuraOfRankedSpell(UnholyDeathKnightSpells::HORN_OF_WINTER) != nullptr;
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

    // 法术压力判定: 敌对目标正在读条非近战法术, 或自身已携带魔法系负面效果,
    // 或同时被 3 个以上敌对单位围攻 (AoE 高压, 预开绿坝吸收即将到来的法术伤害)。
    bool IsUnderMagicPressure(Unit* victim)
    {
        if (victim && victim->IsNonMeleeSpellCast(false))
            return true;

        uint32 const dispelMask = (1u << DISPEL_MAGIC) | (1u << DISPEL_CURSE);
        for (auto const& pair : me->GetAppliedAuras())
        {
            AuraApplication* app = pair.second;
            if (!app || app->IsPositive())
                continue;

            Aura* aura = app->GetBase();
            if (!aura)
                continue;

            SpellInfo const* spellInfo = aura->GetSpellInfo();
            if (spellInfo && (spellInfo->GetDispelMask() & dispelMask))
                return true;
        }

        return CountNearbyEnemies(10.0f) >= MAGIC_PRESSURE_ENEMY_COUNT;
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
    // APL 主决策流
    // =========================================================================
    void PerformCombatAPL(Unit* victim)
    {
        // ---- P0: 极限自保 (Off-GCD 顺下, 严禁 return 中断当帧决策流) ----
        TryEmergencySurvival(victim);

        // ---- P1: 姿态与常驻增益维持 ----
        if (MaintainBloodPresence())
            return;

        if (MaintainBoneShield())
            return;

        if (MaintainHornOfWinter())
            return;

        // ---- P2: 爆发大招 (召唤石像鬼 / 符文武器增效) ----
        if (TryBurstCooldowns(victim))
            return;

        // ---- P3: 核心打击 FCFS ----
        PerformStrikeRotation(victim);
    }

    // =========================================================================
    // P0: 极限自保
    // =========================================================================
    void TryEmergencySurvival(Unit* victim)
    {
        // ---- 冰封之韧: 濒死 50% 硬减伤 + 免疫昏迷 ----
        if (iceboundFortitudeCooldown == 0 && me->GetHealthPct() < ICEBOUND_HP_PCT &&
            !me->HasAura(UnholyDeathKnightSpells::ICEBOUND_FORTITUDE))
        {
            if (CanCast(me, UnholyDeathKnightSpells::ICEBOUND_FORTITUDE, true) &&
                ExecuteSpell(me, UnholyDeathKnightSpells::ICEBOUND_FORTITUDE, true))
            {
                iceboundFortitudeCooldown = CD_ICEBOUND_FORTITUDE;
                // 铁律 8: 自保瞬发大招严禁 return, 必须允许当帧顺下继续消费后续增益窗口
            }
        }

        // ---- 反魔法护罩: 法术尖刺前摇 / 自身魔法 Debuff / 群体高压 ----
        if (antiMagicShellCooldown == 0 && !me->HasAura(UnholyDeathKnightSpells::ANTI_MAGIC_SHELL) &&
            IsUnderMagicPressure(victim))
        {
            if (CanCast(me, UnholyDeathKnightSpells::ANTI_MAGIC_SHELL, true) &&
                ExecuteSpell(me, UnholyDeathKnightSpells::ANTI_MAGIC_SHELL, true))
            {
                antiMagicShellCooldown = CD_ANTI_MAGIC_SHELL;
            }
        }
    }

    // =========================================================================
    // P1: 姿态与常驻增益维护
    // =========================================================================
    bool MaintainBloodPresence()
    {
        // 3.3.5a 邪恶 DPS 唯一指定灵气为鲜血灵气 (+15% 伤害), 严禁开启冰霜/邪恶灵气
        if (me->HasAura(UnholyDeathKnightSpells::BLOOD_PRESENCE))
            return false;

        if (!CanCast(me, UnholyDeathKnightSpells::BLOOD_PRESENCE, true))
            return false;

        return ExecuteSpell(me, UnholyDeathKnightSpells::BLOOD_PRESENCE, true);
    }

    bool MaintainBoneShield()
    {
        // 白骨之盾为纯天赋技能 (40 级解锁), 未习得时严禁硬放最高 Rank
        if (!HasTalent(UnholyDeathKnightSpells::BONE_SHIELD))
            return false;

        if (me->HasAura(UnholyDeathKnightSpells::AURA_BONE_SHIELD))
            return false;

        if (boneShieldCooldown > 0)
            return false;

        if (!CanCast(me, UnholyDeathKnightSpells::BONE_SHIELD, true))
            return false;

        if (ExecuteSpell(me, UnholyDeathKnightSpells::BONE_SHIELD, true))
        {
            boneShieldCooldown = CD_BONE_SHIELD;
            return true;
        }

        return false;
    }

    bool MaintainHornOfWinter()
    {
        bool const inCombat = me->IsInCombat();
        bool const hasAura = HasHornOfWinter();

        if (!inCombat)
        {
            // 脱战: 力量敏捷全团增益必须无条件常驻
            if (hasAura)
                return false;
        }
        else
        {
            // 战时: 仅增益断档或符能枯竭时按 20 秒节流补号角, 蹭 10 点符能
            if (hasAura && me->GetPower(POWER_RUNIC_POWER) >= HORN_RUNIC_POWER_LOW)
                return false;

            if (hornOfWinterCooldown > 0)
                return false;
        }

        // 寒冬号角为分阶法术: 必须走 GetAppropriateRank 做基础法术降阶,
        // 直接硬放最高 Rank 会在低等级被底层以「法术等级超限」拒绝, 造成增益永久断档。
        uint32 const hornOfWinter = GetAppropriateRank(UnholyDeathKnightSpells::HORN_OF_WINTER, false);
        if (!hornOfWinter || !CanCast(me, hornOfWinter, true))
            return false;

        if (ExecuteSpell(me, hornOfWinter, true))
        {
            hornOfWinterCooldown = CD_HORN_OF_WINTER;

            // 3.3.5a 寒冬号角施放成功附带产出 10 符能
            me->ModifyPower(POWER_RUNIC_POWER, RUNIC_POWER_PER_HORN);
            return true;
        }

        return false;
    }

    // =========================================================================
    // P2: 爆发大招
    // =========================================================================
    bool TryBurstCooldowns(Unit* victim)
    {
        if (!victim || !IsBossOrEliteTarget(victim))
            return false;

        if (!me->IsWithinMeleeRange(victim))
            return false;

        // ---- 召唤石像鬼: 3 分钟大招, 需 60 符能蓄水完成后一次性交出 ----
        if (summonGargoyleCooldown == 0 && HasTalent(UnholyDeathKnightSpells::SUMMON_GARGOYLE) &&
            me->GetPower(POWER_RUNIC_POWER) >= GARGOYLE_RUNIC_POWER_REQUIRED)
        {
            if (CanCast(me, UnholyDeathKnightSpells::SUMMON_GARGOYLE, true) &&
                ExecuteSpell(me, UnholyDeathKnightSpells::SUMMON_GARGOYLE, true))
            {
                summonGargoyleCooldown = CD_SUMMON_GARGOYLE;
                return true;
            }
        }

        // ---- 符文武器增效: 5 分钟大招, 符能枯竭且天灾打击进入符文轮转空窗时开启 ----
        if (empowerRuneWeaponCooldown == 0 &&
            me->GetPower(POWER_RUNIC_POWER) < EMPOWER_RUNIC_POWER_LOW &&
            scourgeStrikeCooldown > 0)
        {
            if (CanCast(me, UnholyDeathKnightSpells::EMPOWER_RUNE_WEAPON, true) &&
                ExecuteSpell(me, UnholyDeathKnightSpells::EMPOWER_RUNE_WEAPON, true))
            {
                empowerRuneWeaponCooldown = CD_EMPOWER_RUNE_WEAPON;

                // 大招附带效果模拟: 立即重置全部符文, 解除打击技的自管节流
                scourgeStrikeCooldown = 0;
                bloodStrikeCooldown = 0;

                // 3.3.5a 符文武器增效施放成功立即产出 25 符能
                me->ModifyPower(POWER_RUNIC_POWER, RUNIC_POWER_PER_EMPOWER);
                return true;
            }
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

        // ---- 1. 双疾病检测与维持 (传染无损刷新 / 冰冷触摸 / 暗影打击) ----
        if (MaintainDiseases(victim))
            return true;

        // ---- 2. 荒芜增益维持 (+5% 全伤害, 鲜血打击补齐) ----
        if (MaintainDesolation(victim))
            return true;

        // ---- 3. 凋零缠绕 (末日突降免费 / 石像鬼蓄水与溢能泄能仲裁) ----
        if (PerformDeathCoil(victim))
            return true;

        // ---- 4. 天灾打击: 核心双疾病物理/暗影重击 ----
        if (PerformScourgeStrike(victim))
            return true;

        // ---- 5. 鲜血打击: 鲜血符文填充, 配合收割转死亡符文 ----
        if (PerformBloodStrike(victim))
            return true;

        return false;
    }

    // =========================================================================
    // 疾病链: 传染无损刷新 / 冰冷触摸 / 暗影打击
    // =========================================================================
    bool MaintainDiseases(Unit* victim)
    {
        if (!victim)
            return false;

        // 仅统计本专精自己施加的疫病, 避免误判其他随从 DK 的疾病而放弃维持
        Aura* frostFever = victim->GetAura(UnholyDeathKnightSpells::AURA_FROST_FEVER, me->GetGUID());
        Aura* bloodPlague = victim->GetAura(UnholyDeathKnightSpells::AURA_BLOOD_PLAGUE, me->GetGUID());

        // ---- 步骤一: 双疾病齐备且任一即将断档, 用传染无损刷新 (传染雕文) ----
        if (frostFever && bloodPlague && pestilenceCooldown == 0)
        {
            bool const needRefresh = (frostFever->GetDuration() <= static_cast<int32>(DISEASE_REFRESH_WINDOW_MS)) ||
                                     (bloodPlague->GetDuration() <= static_cast<int32>(DISEASE_REFRESH_WINDOW_MS));

            if (needRefresh && CanCast(victim, UnholyDeathKnightSpells::PESTILENCE, true) &&
                ExecuteSpell(victim, UnholyDeathKnightSpells::PESTILENCE, true))
            {
                pestilenceCooldown = PESTILENCE_COOLDOWN_MS;

                // 符文消耗产出 10 符能
                me->ModifyPower(POWER_RUNIC_POWER, RUNIC_POWER_PER_RUNE_STRIKE);
                return true;
            }
        }

        // ---- 步骤二: 缺失冰霜疫病 ----
        if (!frostFever)
        {
            uint32 const icyTouch = GetAppropriateRank(UnholyDeathKnightSpells::ICY_TOUCH, false);
            if (icyTouch && CanCast(victim, icyTouch, true) &&
                ExecuteSpell(victim, icyTouch, true))
            {
                // 符文消耗产出 10 符能
                me->ModifyPower(POWER_RUNIC_POWER, RUNIC_POWER_PER_RUNE_STRIKE);
                return true;
            }
        }

        // ---- 步骤三: 缺失血之疫病, 近战位读打暗影打击 ----
        if (!bloodPlague && me->IsWithinMeleeRange(victim))
        {
            uint32 const plagueStrike = GetAppropriateRank(UnholyDeathKnightSpells::PLAGUE_STRIKE, false);
            if (plagueStrike && CanCast(victim, plagueStrike, true) &&
                ExecuteSpell(victim, plagueStrike, true))
            {
                // 符文消耗产出 10 符能
                me->ModifyPower(POWER_RUNIC_POWER, RUNIC_POWER_PER_RUNE_STRIKE);
                return true;
            }
        }

        return false;
    }

    // =========================================================================
    // 荒芜增益维持: 缺失或剩余 <= 3s 时用鲜血打击补齐 (+5% 全伤害)
    // =========================================================================
    bool MaintainDesolation(Unit* victim)
    {
        if (!victim || bloodStrikeCooldown > 0)
            return false;

        Aura* desolation = me->GetAura(UnholyDeathKnightSpells::AURA_DESOLATION);
        if (desolation && desolation->GetDuration() > static_cast<int32>(DESOLATION_REFRESH_WINDOW_MS))
            return false;

        return TryBloodStrike(victim);
    }

    // =========================================================================
    // 凋零缠绕: 末日突降免费消费 + 石像鬼符能蓄水 / 防溢能泄能仲裁
    // =========================================================================
    bool PerformDeathCoil(Unit* victim)
    {
        if (!victim)
            return false;

        uint32 const deathCoil = GetAppropriateRank(UnholyDeathKnightSpells::DEATH_COIL, false);
        if (!deathCoil)
            return false;

        // ---- 末日突降: 免费且零符能消耗, 绝对最高优先级消费 ----
        if (me->HasAura(UnholyDeathKnightSpells::AURA_SUDDEN_DOOM))
        {
            if (CanCast(victim, deathCoil, true))
                return ExecuteSpell(victim, deathCoil, true);

            return false;
        }

        uint32 const runicPower = me->GetPower(POWER_RUNIC_POWER);

        // ---- 防溢能硬阈值: 无论石像鬼是否临近都必须泄能, 严禁符能溢出浪费 ----
        if (runicPower >= DEATH_COIL_OVERFLOW_THRESHOLD)
        {
            if (CanCast(victim, deathCoil, true))
                return ExecuteSpell(victim, deathCoil, true);

            return false;
        }

        // ---- 符能蓄水: 石像鬼即将就绪 (<=10s) 且不足以支付 60 符能时封锁常规泄能 ----
        bool const gargoyleImminent = HasTalent(UnholyDeathKnightSpells::SUMMON_GARGOYLE) &&
                                      summonGargoyleCooldown > 0 &&
                                      summonGargoyleCooldown <= GARGOYLE_STOCKPILE_WINDOW_MS;

        if (gargoyleImminent && runicPower < GARGOYLE_RUNIC_POWER_REQUIRED)
            return false;

        // ---- 常规泄能: 石像鬼处于长 CD 且符能已达常态阈值 ----
        if (!gargoyleImminent && runicPower < DEATH_COIL_NORMAL_THRESHOLD)
            return false;

        if (CanCast(victim, deathCoil, true))
            return ExecuteSpell(victim, deathCoil, true);

        return false;
    }

    // =========================================================================
    // 天灾打击: 核心双疾病物理/暗影重击
    // =========================================================================
    bool PerformScourgeStrike(Unit* victim)
    {
        if (!victim || scourgeStrikeCooldown > 0)
            return false;

        // 天灾打击为纯天赋技能 (55 级解锁), 未习得时严禁返回最高 Rank ID 硬放
        if (!HasTalent(UnholyDeathKnightSpells::SCOURGE_STRIKE))
            return false;

        if (!me->IsWithinMeleeRange(victim))
            return false;

        // 双疾病齐备才吃满天灾打击的暗影加成, 缺病时让路给疾病链补病
        if (!victim->HasAura(UnholyDeathKnightSpells::AURA_FROST_FEVER) ||
            !victim->HasAura(UnholyDeathKnightSpells::AURA_BLOOD_PLAGUE))
            return false;

        uint32 const scourgeStrike = GetAppropriateRank(UnholyDeathKnightSpells::SCOURGE_STRIKE, true);
        if (!scourgeStrike || !CanCast(victim, scourgeStrike, true))
            return false;

        if (ExecuteSpell(victim, scourgeStrike, true))
        {
            scourgeStrikeCooldown = SCOURGE_STRIKE_COOLDOWN_MS;

            // 天灾打击命中即产出 20 符能
            me->ModifyPower(POWER_RUNIC_POWER, RUNIC_POWER_PER_SCOURGE_STRIKE);
            return true;
        }

        return false;
    }

    // =========================================================================
    // 鲜血打击: 鲜血符文填充技 (被荒芜维持与常规 FCFS 共用)
    // =========================================================================
    bool PerformBloodStrike(Unit* victim)
    {
        return TryBloodStrike(victim);
    }

    bool TryBloodStrike(Unit* victim)
    {
        if (!victim || bloodStrikeCooldown > 0)
            return false;

        if (!me->IsWithinMeleeRange(victim))
            return false;

        uint32 const bloodStrike = GetAppropriateRank(UnholyDeathKnightSpells::BLOOD_STRIKE, false);
        if (!bloodStrike || !CanCast(victim, bloodStrike, true))
            return false;

        if (ExecuteSpell(victim, bloodStrike, true))
        {
            bloodStrikeCooldown = BLOOD_STRIKE_COOLDOWN_MS;

            // 鲜血打击消耗鲜血符文, 命中即产出 10 符能
            me->ModifyPower(POWER_RUNIC_POWER, RUNIC_POWER_PER_RUNE_STRIKE);
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
    // 邪恶天赋被动光环与雕文补偿 (弥补 NPC 缺天赋树缺陷, 铁律 33)
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

        // ---- 满阶被动天赋根源注入 (严禁注入 Rank 1 导致触发率/数值缩水) ----
        SyncPassive(LEVEL_DESOLATION, UnholyDeathKnightSpells::DESOLATION);
        SyncPassive(LEVEL_EBON_PLAGUEBRINGER, UnholyDeathKnightSpells::EBON_PLAGUEBRINGER);
        SyncPassive(LEVEL_RAGE_OF_RIVENDARE, UnholyDeathKnightSpells::RAGE_OF_RIVENDARE);
        SyncPassive(LEVEL_IMPURITY, UnholyDeathKnightSpells::IMPURITY);
        SyncPassive(LEVEL_REAPING, UnholyDeathKnightSpells::REAPING);
        SyncPassive(LEVEL_NECROSIS, UnholyDeathKnightSpells::NECROSIS);
        SyncPassive(LEVEL_BLOOD_CAKED_BLADE, UnholyDeathKnightSpells::BLOOD_CAKED_BLADE);
        SyncPassive(LEVEL_SUDDEN_DOOM, UnholyDeathKnightSpells::SUDDEN_DOOM);
        SyncPassive(LEVEL_MASTER_OF_GHOULS, UnholyDeathKnightSpells::MASTER_OF_GHOULS);

        // ---- 雕文补偿 ----
        SyncPassive(LEVEL_GLYPH, UnholyDeathKnightSpells::GLYPH_OF_DISEASE);
        SyncPassive(LEVEL_GLYPH, UnholyDeathKnightSpells::GLYPH_OF_SCOURGE_STRIKE);
        SyncPassive(LEVEL_GLYPH, UnholyDeathKnightSpells::GLYPH_OF_DARK_DEATH);
    }
};

void AddSC_bot_unholy_death_knight()
{
    new AdaptiveBotScript<BotUnholyDeathKnightAI>("bot_unholy_death_knight");
}
