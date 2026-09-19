/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "AdaptiveBotAI.h"
#include "BotGuardianAI.h"
#include "BotGuardianDisplays.h"
#include "ObjectAccessor.h"
#include "PetDefines.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

void BotGuardianAI::ApplyRandomGuardianDisplay(GuardianVisualType type)
{
    if (!me)
        return;

    std::vector<GuardianDisplayEntry> const* pool = nullptr;

    switch (type)
    {
        case GUARDIAN_VISUAL_HUNTER_BEAST:     pool = &HunterBeastDisplays; break;
        case GUARDIAN_VISUAL_DK_UNDEAD:        pool = &DkUndeadDisplays; break;
        case GUARDIAN_VISUAL_WARLOCK_IMP:      pool = &WarlockImpDisplays; break;
        case GUARDIAN_VISUAL_WARLOCK_FELHOUND: pool = &WarlockFelhoundDisplays; break;
        case GUARDIAN_VISUAL_WARLOCK_FELGUARD: pool = &WarlockFelguardDisplays; break;
        default: return;
    }

    if (!pool || pool->empty())
        return;

    uint32 const index = urand(0, static_cast<uint32>(pool->size()) - 1);

    // 缓存本次摇号结果：后续 Reset() 只做幂等复用，绝不重新摇号
    _chosenDisplay = (*pool)[index];
    _displayApplied = true;

    ApplyGuardianDisplay(_chosenDisplay);
}

void BotGuardianAI::RefreshGuardianDisplay()
{
    if (!me)
        return;

    // 尚未完成首次 Roll 点时补一次摇号，否则复用缓存外观
    if (!_displayApplied)
    {
        ApplyRandomGuardianDisplay(_visualType);
        return;
    }

    ApplyGuardianDisplay(_chosenDisplay);
}

void BotGuardianAI::ApplyGuardianDisplay(GuardianDisplayEntry const& entry)
{
    if (!me || entry.displayId == 0)
        return;

    // 双重接口设值：SetDisplayId 负责即时刷新表现层，SetNativeDisplayId 负责在
    // 受控、变形、状态刷新结束后不回退为 creature_template 的原生默认模型。
    me->SetDisplayId(entry.displayId);
    me->SetNativeDisplayId(entry.displayId);

    // 体型归一化必须放在最后：仅改动表现层 ObjectScale，
    // 严禁触碰服务端 combat_reach 与 bounding_radius。
    me->SetObjectScale(entry.scale);
}

/// @brief 小鬼内核【火焰箭】等级阶梯解析。
///        3.3.5a 中小鬼火焰箭为固定 DBC 阶数法术（非 Rank 链），无法通过
///        GetAppropriateRank 回溯降阶，必须手工按等级阶梯查表，
///        否则低等级随从会强行施放 80 级法术被底层拒绝而形成永久发呆。
static uint32 GetImpFireboltSpellId(uint8 level)
{
    if (level >= 80) return 47964;
    if (level >= 76) return 47963;
    if (level >= 68) return 27264;
    if (level >= 58) return 10939;
    if (level >= 48) return 10938;
    if (level >= 38) return 7802;
    if (level >= 28) return 7801;
    if (level >= 18) return 7800;
    if (level >= 8)  return 7799;
    return 3110;
}

Unit* BotGuardianAI::GetMaster() const
{
    if (!me)
        return nullptr;

    // 优先取魅惑者/拥有者：随从归属绑定完成后的常态路径
    if (Unit* owner = me->GetCharmerOrOwner())
        return owner;

    // 回退到创建者：SummonCreature 返回后到 SetOwnerGUID 生效前的窗口期内，
    // 只有 CreatorGUID 可查，必须靠它才能拿到宿主并解析出正确职业内核。
    if (ObjectGuid const& creatorGuid = me->GetCreatorGUID())
        return ObjectAccessor::GetUnit(*me, creatorGuid);

    return nullptr;
}

GuardianVisualType BotGuardianAI::ResolveVisualTypeFromMaster() const
{
    // 使用自建 GetMaster() 而非 GetCharmerOrOwner()：随从刚召唤出的瞬间只有
    // CreatorGUID 有效，若只看 OwnerGuid 会解析失败并静默回退到默认的猎人野兽池，
    // 导致术士小鬼 / 死骑食尸鬼被灌成近战内核。
    if (Unit* owner = GetMaster())
    {
        // 首选：向宿主机器人查询「专精级」偏好。
        // 职业级 switch 无法区分术士三系 —— 痛苦术要地狱犬、恶魔术要恶魔卫士、
        // 毁灭术要小鬼，若只按 CLASS_WARLOCK 判定会把三系全部灌成小鬼内核。
        if (Creature* ownerCreature = owner->ToCreature())
        {
            if (auto* botAI = dynamic_cast<AdaptiveBotAI*>(ownerCreature->AI()))
                return botAI->GetPreferredGuardianVisualType();
        }

        // 兜底：宿主非模块机器人（或 AI 未就绪）时退回职业级推断
        switch (owner->getClass())
        {
            case CLASS_DEATH_KNIGHT:
                return GUARDIAN_VISUAL_DK_UNDEAD;
            case CLASS_WARLOCK:
                return GUARDIAN_VISUAL_WARLOCK_IMP;
            case CLASS_HUNTER:
                return GUARDIAN_VISUAL_HUNTER_BEAST;
            default:
                break;
        }
    }

    // 无法识别主人职业时保留构造期传入的外观池
    return _visualType;
}

void BotGuardianAI::ApplyGuardianCoreAuras()
{
    if (!me->IsAlive())
        return;

    // 1. 全随从通用：注入 AoE 范围伤害 90% 减伤（回避 / Avoidance）。
    //    随从作为普通 Creature 挂载，原生没有宠物训练师处习得的被动免伤，
    //    进入团本极易被环境 AoE / 顺劈瞬秒，必须无条件补齐。
    if (!me->HasAura(32233))
        me->CastSpell(me, 32233, true);

    uint8 const level = me->GetLevel();

    // 非小鬼内核必须清除法伤增效光环 23568：该光环会同时放大近战护卫的白字平砍
    // 与技能伤害，使座狼 / 食尸鬼 / 地狱犬 / 恶魔卫士出现数值溢出。
    // 必须保证该光环仅由小鬼分支独占持有。
    if (_visualType != GUARDIAN_VISUAL_WARLOCK_IMP)
        me->RemoveAurasDueToSpell(23568);

    // 2. 依据机制内核施加专属毕业功能光环（等级阶梯自适应）
    switch (_visualType)
    {
        case GUARDIAN_VISUAL_HUNTER_BEAST:
        {
            // 猎人全陆生野兽锁死【座狼】机制：首次生成与脱战立即补一发狂怒之嚎
            uint32 const howlId = (level >= 80) ? 64491 :
                                  (level >= 70) ? 24602 :
                                  (level >= 56) ? 24601 :
                                  (level >= 40) ? 24597 :
                                  (level >= 24) ? 24605 : 24604;
            me->CastSpell(me, howlId, true);
            _furiousHowlTimer = 40000;
            break;
        }
        case GUARDIAN_VISUAL_WARLOCK_IMP:
        {
            // 术士小鬼锁死【血之契印】耐力光环
            uint32 const pactId = (level >= 80) ? 47982 :
                                  (level >= 70) ? 27268 :
                                  (level >= 60) ? 27267 :
                                  (level >= 50) ? 11767 :
                                  (level >= 38) ? 11766 :
                                  (level >= 26) ? 7805  :
                                  (level >= 14) ? 7804  : 6307;
            if (!me->HasAura(pactId))
                me->CastSpell(me, pactId, true);

            // 小鬼法伤增效：随从作为普通 Creature 没有玩家级的装备法强与天赋加成，
            // 仅靠原生基础法伤会导致火焰箭造成个位数伤害，完全无法贡献有效输出。
            // 此处按等级阶梯注入系统伤害增效光环 23568，与治疗随从注入 23569 的
            // 通道同源，保证小鬼在 1~80 全等级段维持及格的远程秒伤。
            int32 const spellDamagePercent = (level >= 80) ? 120 : static_cast<int32>(40 + (level / 80.0f) * 60.0f);
            me->RemoveAurasDueToSpell(23568);
            me->CastCustomSpell(me, 23568, &spellDamagePercent, nullptr, nullptr, true);
            break;
        }
        case GUARDIAN_VISUAL_WARLOCK_FELHOUND:
        {
            // 痛苦术地狱犬锁死【恶魔智力】智力与精神光环
            uint32 const intelId = (level >= 80) ? 57567 :
                                   (level >= 66) ? 57566 :
                                   (level >= 54) ? 57565 : 57564;
            if (!me->HasAura(intelId))
                me->CastSpell(me, intelId, true);
            break;
        }
        default:
            break;
    }
}

void BotGuardianAI::Reset()
{
    ScriptedAI::Reset();

    // 被动反应状态：底层引擎不再自主指派仇恨目标，杜绝随从擅自警戒引怪
    me->SetReactState(REACT_PASSIVE);

    // 1. 外观池与职业内核必须最先就绪：后续的法力池配置、属性镜像与光环注入
    //    全部以 _visualType 为分派依据。若把该推断留在函数末尾（原实现顺序），
    //    首次生成时会先按构造期的默认猎人内核把小鬼灌成近战资源池，造成
    //    「法系内核 + 近战资源」的倒挂，远程读条能力彻底失效。
    if (!_visualTypeExplicit)
        _visualType = ResolveVisualTypeFromMaster();

    // 2. 宿主属性镜像同步：阵营 / 位面 / 等级 / 移速
    // 随从仅供 AdaptiveBotAI 机器人搭配，属性投影必须与宿主严格一致，
    // 否则会出现「能打却打不到」「同队却互相不可见」等投影错位问题。
    if (Unit* owner = GetMaster())
    {
        me->SetFaction(owner->GetFaction());
        me->SetPhaseMask(owner->GetPhaseMask(), true);

        if (me->GetLevel() != owner->GetLevel())
        {
            me->SetLevel(owner->GetLevel());
            me->SetHealth(me->GetMaxHealth());
        }

        me->SetSpeed(MOVE_RUN, owner->GetSpeedRate(MOVE_RUN));
        me->SetSpeed(MOVE_WALK, owner->GetSpeedRate(MOVE_WALK));
    }

    // 3. 小鬼内核：配置独立法力池（必须在 _visualType 判定之后执行）。
    //    小鬼作为法系随从必须以法力驱动远程读条，若沿用近战模板的原生资源池
    //    会出现「无蓝可读」的瘫疾；此处按等级线性投影法力上限并立即灌满。
    if (_visualType == GUARDIAN_VISUAL_WARLOCK_IMP)
    {
        me->setPowerType(POWER_MANA);
        uint32 const impMana = me->GetLevel() * 120 + 2000;
        me->SetMaxPower(POWER_MANA, impMana);
        me->SetPower(POWER_MANA, impMana);
    }

    // 4. 表现层外观应用：幂等，仅首次生成 Roll 点，脱战重聚只重新应用缓存结果
    if (!_displayApplied)
        ApplyRandomGuardianDisplay(_visualType);
    else
        RefreshGuardianDisplay();

    // 5. 脱战重聚自动归位到伴随位，避免遗留在原地
    if (Unit* owner = GetMaster())
    {
        me->GetMotionMaster()->Clear();
        me->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
    }

    // 6. 外观与属性就绪后注入毕业机制内核光环（外观千人千面，内核永远毕业）。
    //    Reset() 为脱战重聚的必经入口，故 30 分钟时长的常驻光环（血之契印 /
    //    恶魔智力）在每次脱战归位时都会被幂等补刷，无需额外高频维护。
    ApplyGuardianCoreAuras();

    OnGuardianReset();
}

void BotGuardianAI::MoveInLineOfSight(Unit* /*who*/)
{
    // 伴随型护卫严禁自主警戒引怪，索敌权完全交由主人控制
    return;
}

void BotGuardianAI::UpdateAI(uint32 diff)
{
    if (!me->IsAlive())
        return;

    Unit* owner = GetMaster();

    // 宿主阵亡 / 离开世界 / 不存在：随从连带销毁。
    // 随从仅供机器人搭配，宿主失效后自身毫无存在意义，绝不允许作为
    // 无主孤儿木桩留在原地（同时避免其被野怪视作独立仇恨实体反复攻击）。
    if (!owner || !owner->IsAlive() || !owner->IsInWorld())
    {
        me->DespawnOrUnsummon();
        return;
    }

    // 防跨图断言崩溃：主人跨地图传送/进本时，随从与主人不在同一 Map，
    // 此时严禁调用 GetDistance()（底层含 ASSERT 同一 Map 校验），直接销毁自身。
    if (me->GetMap() != owner->GetMap())
    {
        me->DespawnOrUnsummon();
        return;
    }

    // 实时位面同步：宿主切换位面（进本 / 相位任务）时随从必须同帧对齐，
    // 否则会在宿主视野中凭空消失。
    if (me->GetPhaseMask() != owner->GetPhaseMask())
        me->SetPhaseMask(owner->GetPhaseMask(), true);

    // 1. 50 码超距防走失拉回：放行猎人等 41 码极限射程开怪，避免在 40 码边界反复瞬移抽搐。
    //    必须先于技能结算执行，确保随从归位到主人身边后，后续光环才生效于主人。
    if (me->GetDistance(owner) > 50.0f)
    {
        me->CombatStop(true);
        me->GetMotionMaster()->Clear();
        me->NearTeleportTo(owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ(), owner->GetOrientation());
        me->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
        return;
    }

    // 2. 座狼内核【狂怒之嚎】40s 周期刷新：
    //    原始主动增益 CD 40s / 持续 20s，此处以触发式施法绕过 CD 独立计时，
    //    保证脱战归途与战时窗口内全程无缝覆盖，杜绝随从 AP 增益断档。
    //    放在超距拉回之后结算，保证嚎叫必然在 50 码内释放，主人 100% 吃到增益。
    if (_visualType == GUARDIAN_VISUAL_HUNTER_BEAST && me->IsAlive())
    {
        if (_furiousHowlTimer <= diff)
        {
            _furiousHowlTimer = 40000;
            uint8 const level = me->GetLevel();
            uint32 const howlId = (level >= 80) ? 64491 :
                                  (level >= 70) ? 24602 :
                                  (level >= 56) ? 24601 :
                                  (level >= 40) ? 24597 :
                                  (level >= 24) ? 24605 : 24604;
            me->CastSpell(me, howlId, true);
        }
        else
        {
            _furiousHowlTimer -= diff;
        }
    }

    // 3. 主人彻底脱战：随从停战并执行一次完整 Reset()。
    //    必须走 Reset() 而非手写归位：脱战重聚契约包含「属性重投影 + 位面修正 +
    //    常驻机制光环幂等补刷（血之契印 / 恶魔智力）+ 外观缓存复用 + 跟随复位」
    //    一整套动作，仅下发 MoveFollow 会让长时长的核心光环在连战间隙静默脱落。
    if (!owner->IsInCombat())
    {
        if (me->IsInCombat() || me->GetVictim())
        {
            me->CombatStop(true);
            Reset();
        }
        return;
    }

    Unit* masterTarget = owner->GetVictim();

    // 4. 主人仍在战斗中，但当前无活体/合法目标（小怪刚死、正在选怪）：
    //    仅停手防发呆，绝不 CombatStop 清空战斗状态，保证多怪连战转火平滑。
    if (!masterTarget || !masterTarget->IsAlive() || !me->IsValidAttackTarget(masterTarget))
    {
        // 先掐断读条再停手：否则会发生「对已死亡目标继续读条」的无效施法，
        // 读完才发现目标已消失，白白浪费一次 GCD 与读条窗口。
        if (me->HasUnitState(UNIT_STATE_CASTING))
            me->InterruptNonMeleeSpells(false);

        if (me->GetVictim())
            me->AttackStop();

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        {
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
        }
        return;
    }

    // 5. 转火严格同步：唯一攻击目标源即宿主当前目标（宿主为机器人，无需
    //    任何玩家专用的选中目标/协助目标探测逻辑）。
    //    —— 小鬼为纯远程法系内核，与近战内核彻底分流，故不再走统一步骤。
    if (_visualType == GUARDIAN_VISUAL_WARLOCK_IMP)
    {
        // 5.0 远程目标锚定：小鬼为纯法系内核，必须显式建立 Victim 锚点。
        //     第二参数传 false 仅完成法术目标锁定，绝不触发底层近战追击与白字
        //     挥砍；同时补齐双向进战绑定，保证远程施法链路与仇恨统计完整成立。
        if (me->GetVictim() != masterTarget)
        {
            me->Attack(masterTarget, false);
            me->SetInCombatWith(masterTarget);
            masterTarget->SetInCombatWith(me);
        }

        // 5.1 蓝量锁定（解决耗蓝）：每帧读条前清空法力消耗顾虑。
        //     随从没有玩家级的回蓝装备与精神回蓝，若按原生消耗结算，
        //     连续几发火焰箭即 OOM 陷入永久发呆，此处直接置满根除。
        me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));

        // 5.2 正在读条 / 引导时放行本帧，严禁下发任何走位指令掐断读条。
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        // 5.3 射程与视线仲裁：超出 30 码或视线受阻时贴近至 20 码甜蜜点。
        if (!me->IsWithinLOSInMap(masterTarget) || me->GetDistance(masterTarget) > 30.0f)
        {
            me->GetMotionMaster()->MoveChase(masterTarget, 20.0f);
            return;
        }

        // 5.4 30 码射程内立定施法：先刹车锁定朝向，再打出火焰箭。
        if (me->isMoving())
            me->StopMoving();

        me->SetFacingToObject(masterTarget);
        me->CastSpell(masterTarget, GetImpFireboltSpellId(me->GetLevel()), false);
    }
    else
    {
        // 5.5 近战内核（座狼 / 食尸鬼 / 恶魔卫士 / 地狱猎犬）：
        //     切换后双向绑定进战状态，确保随从与目标互相进入战斗列表，
        //     平砍与仇恨链路完整成立。
        if (me->GetVictim() != masterTarget)
        {
            AttackStart(masterTarget);
            me->SetInCombatWith(masterTarget);
            masterTarget->SetInCombatWith(me);
        }
        else if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
        {
            // 目标未变但追击链已断（被 Boss 击飞、昏迷/恐惧醒来、地形挤出等）：
            // 重新下发 MoveChase 恢复贴身追击，杜绝原地发呆直到目标死亡。
            // 注意 REACT_PASSIVE 下底层不会自动补发 Chase，必须在此显式自愈。
            me->GetMotionMaster()->MoveChase(masterTarget);
        }

        // 6. 白字平砍循环：直接调用，绝不使用 UpdateVictim() 作为守卫。
        //    随从强制 REACT_PASSIVE，底层 UpdateVictim() 恒返回 false，
        //    若以其为门禁会导致 DoMeleeAttackIfReady() 永远无法触发（平砍瘫痪）。
        DoMeleeAttackIfReady();
    }
}

class BotGuardianScript : public CreatureScript
{
public:
    BotGuardianScript() : CreatureScript("BotGuardianAI") { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new BotGuardianAI(creature);
    }
};

void AddSC_bot_guardian()
{
    new BotGuardianScript();
}
