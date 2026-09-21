# mod-adaptive-bot 项目核心架构与开发上下文记忆\[cite: 68]

## 1\. 核心架构模式：分层自适应系统 (Hierarchical Adaptive System)\[cite: 68]

本项目基于 AzerothCore (WoW 3.3.5a) 构建轻量化、不侵入核心源码的随从 AI，采用三层解耦闭环架构\[cite: 68]：

```text
\\\[ 阶段三/顶层：独立持久化认知记忆库 (SQLite WAL 模式) ]
  - 存储介质：独立 SQLite 数据库文件 (bot\_memory.db)，与 AzerothCore 原生 MySQL 业务库 (world/characters) 100% 物理与连接解耦
  - 存储表结构：bot\_individual\_profile, bot\_individual\_hazard, bot\_individual\_interrupt
  - 唯一标识隔离：基于 (spawn\_id, boss\_entry) 复合主键，物理隔离世界常驻实体与一次性临时召唤物
  - 动态下发：战前 O(1) 预热 Boss 机制、危险禁区坐标半径、按技能学习的压秒打断余量 (learnedInterruptDelays)
         ▲
         │ 异步事务落盘 / 战前内存预热
\\\[ 阶段二/中层：战斗回溯归因分析器 + APF 人工势场避险系统 (Potential Field) ]
  - 归因时机：仅在团灭脱战 (EnterEvadeMode) 或战斗结算 (OnCombatEnded) 时由环形缓冲区 (BotCombatRingBuffer<256>) 异步结算，零战斗运行时 CPU 开销
  - 空间避险：APF 势场算法动态结算圆形火圈/黑水 (CIRCLE) 与正面顺劈锥形 (FRONTAL\_CONE)，300ms 帧节流 (apfMoveUpdateTimer) 杜绝路径抽搐
  - 坦克豁免：坦克专精正面硬接聚怪，输出与治疗随从在 IsUnderDangerThreat(2.0f) 成立时强行接管走位切线逃离
         ▲
         │ 运行时事件沉淀 / 动态势场引导
\\\[ 阶段一/底层：专家动作优先级列表 (APL Baseline) + 伴随型轻量战斗护卫体系 ]
  - 运行实体：轻量级 CreatureAI，基于 OnLevelSynced 动态属性投影与 ApplyPassiveTalents 满阶根源补偿
  - 动作时序：Off-GCD 技能当帧顺下、On-Next-Swing 物理挥砍驱动、终结技攒能挂起 (Energy Pooling)
  - 核心功能：提供开箱即用高战力 Baseline，执行由中顶层动态驱动的避火、压秒打断与仇恨协同
```

\---

## 2\. 基类约定与核心可用 API (AdaptiveBotAI.h)\[cite: 60, 67, 68]

所有随从专精必须继承自统一基类 `AdaptiveBotAI`，且必须优先复用以下基类契约与接口\[cite: 67, 68]：

### 虚函数重写契约规范\[cite: 67, 68]

* `bool IsTankBot() const override`：坦克专精返回 `true`，激活常驻 50% 装备减伤、1.3x 受疗乘数及正面顺劈豁免\[cite: 67]。
* `bool IsHealerBot() const override`：治疗专精返回 `true`，压低输出至 `0.25f`，激活系统治疗增效光环 (23569)\[cite: 67]。
* `bool IsRangedBot() const override`：远程与治疗返回 `true`，接管远程站位；物理近战返回 `false`\[cite: 67, 68]。
* `bool IsRangedPhysicalBot() const override`：猎人重写为 `true`，输出由远程 AP 支撑，与法系远程的法伤加成彻底解耦\[cite: 67, 68]。
* `float GetDamageDealtMultiplier() const override`：治疗专精返回 `0.25f`；纯物理输出与坦克专精固定返回 `1.0f`（严禁误继承法系随从的 2.0x\~3.3x 装备倍率通道）\[cite: 67, 68]。
* `uint8 GetTalentSpellMinLevel(uint32 spellId) const override`：以 `switch-case` 显式注册纯天赋技能的最低解锁等级（针对 3.3.5a DBC 中 SpellLevel 恒为 0 技能的门禁契约）\[cite: 67, 68]。
* `void Reset() override`：配置能量池类型，清空专精自管计时器与状态机标记，调用基类 `Reset()` 并执行 `ApplyPassiveTalents()`，触发 `EnsureGuardianAlive()`\[cite: 67, 68]。
* `void OnLevelSynced(uint8 level) override`：同步等级与对应资源池上限，调用 `ApplyPassiveTalents()` 注入被动光环补偿\[cite: 67, 68]。

### 伴随护卫体系虚函数与接口契约\[cite: 67, 68]

* `virtual bool ShouldHaveGuardian() const`：判定当前专精是否常驻伴随护卫\[cite: 67, 68]。基类默认仅放行 `CLASS\_HUNTER` 与 `CLASS\_WARLOCK`\[cite: 67, 68]；邪 DK 显式覆写返回 `true`\[cite: 49]，血 DK 与冰 DK 显式覆写返回 `false`\[cite: 47, 48]。
* `virtual GuardianVisualType GetPreferredGuardianVisualType() const`：专精级随从内核与外观池偏好分流契约\[cite: 67, 68]。基类提供职业默认，专精类（痛苦术/恶魔术/毁灭术/邪 DK/猎人）通过重写精准下发绑定类型\[cite: 49, 67, 68]。
* `void EnsureGuardianAlive()`：护卫保活仲裁器\[cite: 67, 68]。在宿主出生/重置（`Reset`）、战后结算（`OnCombatEnded`）及脱战 3 秒轮询中巡检，阵亡或丢失时以 `TEMPSUMMON\_MANUAL\_DESPAWN` 模式唤出 `NPC\_BOT\_GUARDIAN (990001)` 并同帧完成归属绑定与从属 `Reset()`\[cite: 67, 68]。

### 阶段二（APF 势场）与阶段三（认知打断）核心 API\[cite: 67, 68]

* `bool IsUnderDangerThreat(float buffer = 2.0f) const`：自检随从自身是否处于任一圆形危险区或正面顺劈锥形区内\[cite: 67]。
* `bool PotentialField::CalculateNextPosition(...)`：势场合力寻路外推核心函数，规划规避危险区并维持目标攻击射程的最佳落点 $(X, Y, Z)$\[cite: 67]。
* `bool ShouldInterruptTarget(Unit\* target, uint32 interruptSpellId = 0)`：记忆化打断裁决器\[cite: 67]。引导类法术零延时抢断，读条法术按 SQLite 预热的 `learnedInterruptDelays`（未命中默认 350ms）精准压秒\[cite: 67]。
* `bool TryInterrupt(Unit\* target, uint32 interruptSpellId)`：一键打断接口，结合压秒时机判定、条件检验与法术施放\[cite: 67]。若打断技为自身原点 PBAoE（如台风），调用无技能 ID 参数版\[cite: 58, 67]。
* `void PreloadBossKnowledge(uint32 bossEntry)`：进战锁定首领瞬间单行读取 SQLite 记忆库，预热危险禁区与打断经验\[cite: 67]。
* `uint32 GetBotSpawnId() const`：通过 `me->GetSpawnId()` 获取世界物理刷新 ID，天然隔离常驻随从与临时召唤物\[cite: 67]。

\---

## 3\. 必须严格遵守的底层引擎铁律 (Avoidance Guide)\[cite: 68]

1. **全局施法与通道双保险守卫：** 读条/引导职业在 `UpdateAI()` 顶部必须执行 `if (me->HasUnitState(UNIT\_STATE\_CASTING) || me->GetCurrentSpell(CURRENT\_CHANNELED\_SPELL)) return;`，杜绝读条被跟随与站位微移打断\[cite: 67, 68]。
2. **MoveFollow 相对弧度机制：** `MoveFollow(target, dist, angle)` 中的 `angle` 原生代表相对目标当前朝向的弧度偏移（引擎内部已叠加目标朝向）\[cite: 68]。**严禁写成 `target->GetOrientation() + angle`**\[cite: 68]。正后方背身位恒传 `static\_cast<float>(M\_PI)`\[cite: 68]。
3. **天赋契约参数闭环：** 3.3.5a 中纯天赋法术在 DBC 中的 `SpellLevel` 均为 0\[cite: 68]。调用 `GetAppropriateRank(id, allowRankOneFallback)` 时，纯天赋技能若未在外部校验 `HasTalent`，第二参数必须传 `false`，严禁传 `true` 导致低等级随从拿到满阶未习得大招反复空放\[cite: 68]。
4. **驱散正向光环过滤与类型解耦：** 校验 `if (application->IsPositive()) continue;` 防止驱散友方增益；萨满仅解诅咒/疾病/中毒，德鲁伊仅解诅咒/中毒，圣骑士解魔法/疾病/中毒\[cite: 68]。
5. **宠物特权与血线彻底剥离：** 打地鼠雷达严禁统计宠物（`unit->ToPet() != nullptr`）；救命大招与核心 HoT 严禁对宠物施放\[cite: 68]。
6. **战时急救门禁覆盖（防治疗倒挂）：** 全队出现濒死重伤（`< 60%\~70%`）时，维护性增益、常规 HoT 与长读条群刷必须无条件让渡给单体急救通道\[cite: 68]。
7. **瞬发跑位施法与 StopMoving 解耦：** `me->StopMoving()` 必须精准下沉至确认施放非瞬发读条法术前夕；瞬发法术放行跑动施法，严禁刹停破坏机动性\[cite: 68]。
8. **Off-GCD 技能防空转与当帧顺下：** 对不占公共冷却的瞬发法术（树皮术、冷血、心灵冰冻、脚踢、法术反制、狂暴、冲动等）施放成功后**严禁 `return true`**，必须当帧顺下执行，使增益当帧立即被后续技能消费\[cite: 68]。
9. **光环防顶守卫：** 唯一类或递增类光环（愈合祷言、活动炸弹、回春术）必须全队/目标查验光环持续时间，避免重复施放吞掉跳数或浪费 GCD\[cite: 68]。
10. **充能型 Buff 扣减铁律：** 寒冰指、爆燃、荷枪实弹等充能光环，随从施法后必须显式调用 `DropCharge()` 或原地削减 `SetStackAmount()`，严禁整层 `RemoveAurasDueToSpell` 后重新注入（会导致持续时间重置刷新）\[cite: 68]。
11. **物理近战背后找背与防同心圆死锁：** 目标看主坦时严格站位正后方 1.5 码规避顺劈与招架加速；怪物看随从本人时（`victim->GetVictim() == me`）禁止绕后，切回 `MoveChase(victim, 1.5f)` 直线贴身硬刚\[cite: 68]。
12. **远程近战盲区撤离迟滞区间：** 猎人/法系远程必须建立迟滞区间：`< 8.0f` 触发撤退（`isRetreating = true`），且必须退至 `>= 15.0f`（或 16.0f）安全线后才允许立定施法，杜绝原地高频抽搐走停\[cite: 68]。
13. **猎人远程自动射击通道维持：** 8\~35 码内必须显式维持 `CURRENT\_AUTOREPEAT\_SPELL` 通道的 `AUTO\_SHOT`（`checkGcd = false`），保证白字输出与蝰蛇回蓝\[cite: 68]。
14. **随从武器毒药与命中模拟：** 潜行者专精必须在白字平砍与技能命中后以节流方式（1000ms）向目标触发注入 `DEADLY\_POISON`（维持 5 层）与 `INSTANT\_POISON`\[cite: 65, 66, 67, 68]。
15. **近战终结技攒能挂起与产星通道锁死：** 连击点达到 4\~5 星门槛但能量不足支付终结技（< 35 能量）时，必须原地挂起等待能量跳动；达到 4\~5 星后直接锁死产星通道，严禁降级施放低级技能偷跑能量\[cite: 65, 66, 67, 68]。
16. **自保免疫/假死/隐形脱困保护期与超时逃逸：** 寒冰屏障设立 1500ms 最小保护期、50% 安全血线与 2.5s 超时兜底；假死设立 1500ms 硬性超时逃逸；隐形术强制跑满 3000ms 淡入期确保仇恨清零\[cite: 68]。
17. **多阶被动天赋满级 Rank 注入契约：** 必须显式核对并注入 **Rank 3/5 满阶 Spell ID**，严禁注入 DBC 默认 Rank 1 根源造成触发率与数值缩水\[cite: 68]。
18. **日月蚀双相记忆状态机：** 引入 `PHASE\_SEEKING\_LUNAR` 与 `PHASE\_SEEKING\_SOLAR` 记忆相位：月蚀期间打星火并锁入找日蚀相；日蚀期间打愤怒并锁入找月蚀相；空窗期坚决沿用上一相位压制以维持暴击源\[cite: 58, 68]。
19. **随从平砍与下一次挥砍强化驱动铁律：** 近战专精（防战/狂暴/武器战、血/冰/邪 DK、惩戒/防骑、猫/熊德、盗贼）彻底重写 `UpdateAI` 且未调用基类时，引擎底层不会自动挥砍。**决策流末尾必须显式调用 `DoMeleeAttackIfReady()`**，否则白字伤害归零，挂在 `CURRENT\_MELEE\_SPELL` 上的强化技能（符文打击、重殴、英勇打击）将永久挂起无法落地\[cite: 47, 48, 49, 59, 60, 65, 66, 67, 68]。
20. **怒气/符文能量底层 x10 定点放大铁律：** 在 WoW 3.3.5a 底层中，`POWER\_RAGE` 与 `POWER\_RUNIC\_POWER` 均采用 10 倍定点存储（100 点能量对应底层数值 1000）\[cite: 47, 59, 68]。基类 `CanCast` 自动执行 `cost \*= 10`\[cite: 67]。所有涉及怒气与符能的专精，其 `MAX\_POWER` 必须设为 `1000`，保底阈值与单次补充量必须放大 10 倍，严禁设为 100 导致技能因“能量不足”全线瘫痪\[cite: 47, 59, 68]。
21. **全专精站位 APF 势场避险第一优先级铁律：** 无论是近战找背（`MaintainMeleeBehindPositioning`）、远程风筝（`MaintainRangedPositioning`）还是治疗跟随（`MaintainHealerPositioning`），函数头部必须第一优先级判断 `if (IsUnderDangerThreat(2.0f))`，配合 `apfMoveUpdateTimer` 300ms 节流由势场接管走位，严禁在火圈与顺劈区内站桩发呆\[cite: 48, 49, 58, 60, 61, 65, 66, 67, 68]。
22. **PBAoE 自身原点法术打断解耦铁律：** 台风等以自身为原点（DBC 基础射程为 0）的范围击退/打断法术，严禁把技能 ID 传入 `ShouldInterruptTarget(victim, id)`，否则基类会对目标做定向距离检验而在 >5 码时直接拒放\[cite: 58, 67, 68]。必须调用无参版 `ShouldInterruptTarget(victim)` 仅作读条时间判定，有效半径由外层单独把关\[cite: 58, 67, 68]。
23. **打断同名函数遮蔽防范铁律：** 专精类严禁在私有域声明同名单参数函数 `bool TryInterrupt(Unit\* victim)`，这会遮蔽基类的阶段三压秒打断虚函数，导致随从无法读取记忆库提前量而退化为盲目秒断\[cite: 67, 68]。
24. **活动炸弹防剪切铁律：** 活动炸弹末跳爆炸占总伤大部，必须严格限制为目标身上本随从的炸弹 DoT 完全缺失（`GetDuration() <= 0`）才允许补挂，严禁提前刷新剪切末跳\[cite: 68]。
25. **清晰预兆 (节能施法) 旁路直放铁律：** 猫德/鸟德等持有 `AURA\_CLEARCASTING` 时，必须以 `triggered = true` 触发式施放主力耗能技，旁路底层 `CanCast` 的基础能量/法力检验，并手工消费光环与置位 GCD，解决 NPC 随从无玩家 SpellMod 导致免费技能被拒放的问题\[cite: 60, 68]。

\---

## 4\. 阶段三：记忆化认知预热与治疗预读 (TryPredictivePreHoT)\[cite: 61, 68]

在缺乏硬打断手段的治疗专精（如恢复德）中，阶段三认知记忆库以“机制预警预读”形态落地\[cite: 61, 68]：

```cpp
bool TryPredictivePreHoT()
{
    if (!me->IsInCombat() || currentEnemyCastingSpellId == 0 || currentEnemyCastingTotalMs == 0)
        return false;

    // 匹配当前读条是否命中个体记忆库中预热的高危禁区机制
    bool isLearnedHazard = false;
    for (auto const\& zone : activeDangerZones)
    {
        if (zone.spellId == currentEnemyCastingSpellId)
        {
            isLearnedHazard = true;
            break;
        }
    }
    if (!isLearnedHazard) return false;

    uint32 const remainingMs = (currentEnemyCastingTotalMs > currentEnemyCastingElapsedMs)
        ? (currentEnemyCastingTotalMs - currentEnemyCastingElapsedMs) : 0;

    // 读条滑入最后 1500ms 危险窗口且本轮未预铺：提前为主坦抢挂回春/三花
    if (remainingMs <= 1500 \&\& !isPreHealing)
    {
        Unit\* tank = groupSnapshot.mainTank;
        if (tank \&\& TryRejuvenation(tank))
        {
            isPreHealing = true;
            return true;
        }
    }
    return false;
}
```

\---

## 5\. 当前各专精阶段二/三审计与基类对齐进展\[cite: 68]

|职业|专精|阶段二 APF 避火顺劈|阶段三 记忆化压秒打断|特殊机制与底层契约闭环|审核状态|
|-|-|:-:|:-:|-|:-:|
|**圣骑士**|神圣 / 防护 / 惩戒|已对齐|已对齐|969 循环、自律解耦自保、单体道标折射|**全部封存**|
|**牧师**|戒律 / 神圣 / 暗影|已对齐|已对齐|24 码射程收敛、灵魂护体、痛自动续期|**全部封存**|
|**战士**|武器 / 狂暴 / 防护|已对齐|已对齐|控怒队列、平砍驱动、嗜血防斩杀饥饿|**全部封存**|
|**死亡骑士**|鲜血 / 冰霜 / 邪恶|已对齐|已对齐|符能 x10 放大、末帧平砍挥砍、天鬼蓄水|**全部封存**|
|**德鲁伊**|平衡 / 巨熊 / 猎豹 / 恢复|已对齐|已对齐|怒气 x10、重殴出刀、台风 PBAoE 解耦、三花滚动|**全部封存**|
|**潜行者**|刺杀 / 战斗 / 敏锐|已对齐|已对齐|攒能挂起、杀戮盛宴锁帧、毒药模拟、脚踢对齐|**全部封存**|
|**猎人**|兽王 / 射击 / 生存|已对齐|已对齐|自动射击维持、假死超时、荷枪实弹防吞跳|**全部封存**|
|**法师**|奥术 / 火焰 / 冰霜|已对齐|已对齐|4层奥冲泄蓝、瞬发炎爆剥层、深冻穿透、寒冰指扣层|**全部封存**|
|**萨满祭司**|元素 / 增强 / 恢复|待审查|待审查|漩涡武器降级、火新星落点追踪、地盾排己|下一阶段|
|**术士**|痛苦 / 恶魔 / 毁灭|待审查|待审查|斩杀破锁重挂、生命分流底层直放、末日自适应|下一阶段|



