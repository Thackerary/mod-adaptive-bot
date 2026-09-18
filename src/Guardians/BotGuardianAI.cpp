/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BotGuardianAI.h"
#include "BotGuardianDisplays.h"
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

GuardianVisualType BotGuardianAI::ResolveVisualTypeFromMaster() const
{
    if (Unit* owner = me->GetCharmerOrOwner())
    {
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

void BotGuardianAI::Reset()
{
    ScriptedAI::Reset();

    // 被动反应状态：底层引擎不再自主指派仇恨目标，杜绝随从擅自警戒引怪
    me->SetReactState(REACT_PASSIVE);

    // 宿主属性镜像同步：阵营 / 位面 / 等级 / 移速
    // 随从仅供 AdaptiveBotAI 机器人搭配，属性投影必须与宿主严格一致，
    // 否则会出现「能打却打不到」「同队却互相不可见」等投影错位问题。
    if (Unit* owner = me->GetCharmerOrOwner())
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

    // 未显式指定外观池时，依据主人职业自动推断，避免全职业沦为猎人野兽
    if (!_visualTypeExplicit)
        _visualType = ResolveVisualTypeFromMaster();

    // 幂等：仅首次生成 Roll 点，脱战重聚只重新应用缓存结果
    if (!_displayApplied)
        ApplyRandomGuardianDisplay(_visualType);
    else
        RefreshGuardianDisplay();

    // 脱战重聚自动归位到伴随位，避免遗留在原地
    if (Unit* owner = me->GetCharmerOrOwner())
    {
        me->GetMotionMaster()->Clear();
        me->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
    }

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

    Unit* owner = me->GetCharmerOrOwner();

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

    // 50 码超距防走失拉回：放行猎人等 41 码极限射程开怪，避免在 40 码边界反复瞬移抽搐
    if (me->GetDistance(owner) > 50.0f)
    {
        me->CombatStop(true);
        me->GetMotionMaster()->Clear();
        me->NearTeleportTo(owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ(), owner->GetOrientation());
        me->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
        return;
    }

    // 1. 主人彻底脱战：随从同步停战并归位
    if (!owner->IsInCombat())
    {
        if (me->IsInCombat() || me->GetVictim())
        {
            me->CombatStop(true);
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
        }
        return;
    }

    Unit* masterTarget = owner->GetVictim();

    // 2. 主人仍在战斗中，但当前无活体/合法目标（小怪刚死、正在选怪）：
    //    仅停手防发呆，绝不 CombatStop 清空战斗状态，保证多怪连战转火平滑。
    if (!masterTarget || !masterTarget->IsAlive() || !me->IsValidAttackTarget(masterTarget))
    {
        if (me->GetVictim())
            me->AttackStop();

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        {
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
        }
        return;
    }

    // 3. 转火严格同步：唯一攻击目标源即宿主当前目标（宿主为机器人，无需
    //    任何玩家专用的选中目标/协助目标探测逻辑），切换后双向绑定进战状态，
    //    确保随从与目标互相进入战斗列表，平砍与仇恨链路完整成立。
    if (me->GetVictim() != masterTarget)
    {
        AttackStart(masterTarget);
        me->SetInCombatWith(masterTarget);
        masterTarget->SetInCombatWith(me);
    }

    // 白字平砍循环
    if (UpdateVictim())
    {
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
