# mod-adaptive-bot 项目核心架构与开发上下文记忆

## 1. 核心架构模式：分层自适应系统 (Hierarchical Adaptive System)
本项目基于 AzerothCore (WoW 3.3.5a) 构建轻量化、不侵入核心源码的随从 AI，采用三层解耦闭环架构：

```text
[ 顶层：持久化机制记忆库 (SQLite) ]
  - 存储索引：(Boss_Entry_ID, Bot_GUID / Spec_ID)
  - 动态下发：Boss 技能轴、高危禁区坐标、打断压秒时机、玩家坦克 TPS 仇恨延后阈值
         ▲
         │ 异步反哺 / 参数覆盖
[ 中层：战斗回溯与归因分析器 (Post-Combat Attribution) ]
  - 触发时机：仅在团灭脱战 (EnterEvadeMode) 或 Boss 击杀 (JustDied) 时异步执行，零战斗运行时 CPU 开销
  - 分析维度：承伤与致死 SpellID、漏打断统计、起手 OT 归因、主坦前 10 秒 TPS 速率测定
         ▲
         │ 运行时数据沉淀
[ 底层：专家动作优先级列表 (APL Baseline) ]
  - 运行实体：轻量级 CreatureAI，基于 OnLevelSynced 动态属性投影与 ApplyPassiveTalents 被动补偿
  - 核心功能：提供开箱即用 70 分 baseline，执行由顶层动态微调的实战战术
```

### 顶层进化四维定义
- **维度 A（打断演进）：** 从“盲目秒断”演化为“识别 Boss 灭团级致命技能，并在读条达到 80% 时压秒打断”，最大化输出偷跑窗口。
- **维度 B（走位自适应）：** 归因器提取高危致死技能来源坐标，将其标记为不可停留禁区，随从在 Boss 施法前摇阶段提前执行侧移规避。
- **维度 C（仇恨协同）：** 依据玩家主坦建立仇恨的前 10 秒 TPS，动态延后输出随从的爆发技能与主力饰品激活时间（0~10s），根除起手 OT 猝死。
- **维度 D（阶段对齐）：** 记忆 Boss 阶段转换阈值（如 20% 狂暴/斩杀期、易伤虚弱期），在机制最关键窗口集中交出嗜血/英勇与核心爆发底牌。

---

## 2. 基类约定与核心可用 API (AdaptiveBotAI.h)
所有随从专精必须继承自统一基类 `AdaptiveBotAI`，且必须优先复用以下基类契约与接口：

### 虚函数重写规范
- `bool IsHealerBot() const override`：治疗专精返回 `true`。
- `bool IsRangedBot() const override`：远程与治疗返回 `true`，接管远程站位与移动控制。
- `float GetDamageDealtMultiplier() const override`：治疗专精压低输出，固定返回 `0.25f`。
- `uint8 GetTalentSpellMinLevel(uint32 spellId) const override`：以 `switch-case` 显式注册天赋与特权技能的最低解锁等级。
- `void Reset() override`：配置能量池类型，清空本地状态机标记，调用基类 `Reset()` 并执行 `ApplyPassiveTalents()`。
- `void OnLevelSynced(uint8 level) override`：同步等级与法力池，调用 `ApplyPassiveTalents()`。

### 核心基类可用接口
- **法术判定与执行：**
  - `uint32 GetAppropriateRank(uint32 spellId, bool isTalent = false) const;`（凡在 `GetTalentSpellMinLevel` 声明的技能必须传 `true`）
  - `bool CanCast(Unit* target, uint32 spellId, bool checkRangeAndLOS = true) const;`
  - `bool ExecuteSpell(Unit* target, uint32 spellId, bool checkRangeAndLOS = true);`
- **目标定位与仇恨：**
  - `Unit* SelectAssistTarget();`（获取协助目标）
  - `Unit* GetGroupTank();`（获取团队主坦）
  - `Player* GetMaster();`（获取随从指挥官）
  - `me->Attack(victim, false);`（远程/治疗仅锚定目标供法术施放，第二参数传 `false` 绝不开启近战追击）
- **移动与控制：**
  - `void UpdateFollowMaster(uint32 diff);`（脱战通用跟随）
  - `me->GetMotionMaster()->MoveFollow(target, dist, angle);`
  - `me->StopMoving();`（仅在决定施放非瞬发读条法术前夕按需调用）

---

## 3. 必须严格遵守的底层引擎铁律 (Avoidance Guide)
1. **全局施法守卫（Casting Guard）：** 治疗与法系 `UpdateAI()` 顶部必须执行 `if (me->HasUnitState(UNIT_STATE_CASTING)) return;`，杜绝长读条被底层的跟随移动指令掐断。
2. **MoveFollow 相对弧度机制：** `MoveFollow(target, dist, angle)` 中的 `angle` 原生代表相对目标当前朝向的弧度偏移（引擎内部自动叠加目标朝向）。**严禁写成 `target->GetOrientation() + angle`**。正后方背身位恒传 `static_cast<float>(M_PI)`，贴坦避难传背身侧向，严禁传 `0.0f`（防止冲到 Boss 脸前吃顺劈与吐息暴毙）。
3. **天赋契约参数闭环：** 3.3.5a 中所有天赋法术在 DBC 中的 `SpellLevel` 均为 0。调用 `GetAppropriateRank(id, isTalent)` 时，所有在 `GetTalentSpellMinLevel` 注册的技能第二参数必须显式为 `true`。
4. **驱散正向光环过滤：** 队友自身的王者祝福、智慧祝福在底层属性中均为 `DISPEL_MAGIC`。驱散循环中必须显式校验 `if (application->IsPositive()) continue;`，防止无限驱散自己人直至空蓝。
5. **宠物特权与血线彻底剥离：**
   - 3.3.5a 圣光道标对宠物的治疗折射量为 0。
   - 打地鼠雷达的 `lowestHpAlly`、`lowestHpPct` 与 `averageHpPct` 严禁统计宠物（`unit->ToPet() != nullptr`），防止宠物掉血引发全队恐慌。
   - 圣疗术、保护之手等战略级大招严禁对宠物施放。
6. **自律（Forbearance）平滑降级：** 判定圣疗术、保护之手时，若首选目标持有自律，必须平滑降级检测全队其余濒死成员，避免单体占位导致大招卡死。
7. **瞬发跑位施法与 StopMoving 解耦：** `me->StopMoving()` 必须精准下沉至读条法术前夕；瞬发技能（如神圣震击）允许在跑位或抱坦避难途中直接施放，保证机动性。
8. **秒拔神圣祈求零延迟：** 团队跌入重伤线（`< 50%`）时，通过 `me->RemoveAurasDueToSpell(DIVINE_PLEA)` 瞬拔 50% 减疗惩罚，且严禁在此处 `return true`，确保当帧顺下执行大加急救。
9. **战时急救门禁覆盖：** P1 常驻 Buff/护盾/道标补挂、P2 审判与回蓝流程必须挂载 `< 60%` 战时血线门禁，全队出现重伤时立即交出施法权，优先执行 P3 阶梯救命。
10. **祝福互顶死循环防御：** 对无蓝物理职业分配力量与王者祝福时，必须设立互斥守卫（只要目标持有其中任意一个即视为就绪），严禁互顶光环浪费 GCD。
11. **纯洁审判光环解耦：** 必须区分天赋被动触发器（54155）与审判命中激发的急速 Buff 本体（53657）。随从挂载被动本体，APL 判定中检查急速 Buff 剩余时间（`<= 3000ms` 补打），杜绝因自检被动天赋导致整场拒绝施放审判。

---

## 4. 治疗专精通用「打地鼠雷达」标准实现 (GroupSnapshot)
所有治疗专精统一定义与复用以下结构及过滤算法：

```cpp
struct GroupSnapshot
{
    std::vector<Unit*> allies;          // 40 码内、视线可达、存活的友方单位
    Unit* lowestHpAlly{ nullptr };      // 全队最低生命百分比成员 (严格排除宠物)
    float lowestHpPct{ 100.0f };        // 团队最低血线指标
    Unit* mainTank{ nullptr };          // 主坦锚点
    float averageHpPct{ 100.0f };       // 全队平均生命百分比 (严格排除宠物)
};

void ConsiderAlly(GroupSnapshot& snap, Unit* unit)
{
    if (!unit || !unit->IsAlive() || !unit->IsInWorld()) return;
    if (unit->GetMap() != me->GetMap() || !unit->IsFriendlyTo(me)) return;
    if (unit->GetTypeId() == TYPEID_UNIT && unit->ToCreature()->IsTotem()) return;
    if (!me->IsWithinDist(unit, HEAL_RANGE) || !me->IsWithinLOSInMap(unit)) return;
    if (std::find(snap.allies.begin(), snap.allies.end(), unit) != snap.allies.end()) return;

    snap.allies.push_back(unit);

    // 宠物保留在 allies 享受溅射与驱散，但最低血线竞争严格排除宠物
    if (!unit->ToPet())
    {
        float const hp = unit->GetHealthPct();
        if (hp < snap.lowestHpPct)
        {
            snap.lowestHpPct = hp;
            snap.lowestHpAlly = unit;
        }
    }
}

void FinalizeSnapshot(GroupSnapshot& snap)
{
    if (snap.allies.empty())
    {
        snap.lowestHpAlly = nullptr;
        snap.lowestHpPct = 100.0f;
        snap.averageHpPct = 100.0f;
        return;
    }

    // 兜底锚点过滤宠物
    if (!snap.lowestHpAlly)
    {
        for (Unit* ally : snap.allies)
        {
            if (ally && !ally->ToPet())
            {
                snap.lowestHpAlly = ally;
                snap.lowestHpPct = ally->GetHealthPct();
                break;
            }
        }
    }

    // 平均血线累加严格过滤宠物，避免宠物频死拉低均值误触全队崩溃保护
    float total = 0.0f;
    uint32 validCount = 0;
    for (Unit* ally : snap.allies)
    {
        if (ally && !ally->ToPet())
        {
            total += ally->GetHealthPct();
            ++validCount;
        }
    }
    snap.averageHpPct = (validCount > 0) ? (total / static_cast<float>(validCount)) : 100.0f;
}
```

---

## 5. 当前专精开发进展与落地状态
- **坦克集群（阶段一已全部交付，具备副本实战品质）：**
  - **防护战士 (`bot_protection_warrior`)：** 怒气自适应平滑注入、盾猛/复仇核心仇恨链、雷霆/冲击波群拉、盾牌格挡与破釜/盾墙梯级减伤、拦截破阵与断筋风筝。
  - **防护圣骑士 (`bot_protection_paladin`)：** 969 技能循环、正义防御与神圣庇护多目标嘲讽路由、神圣之盾常驻维持、炽热防御者免死补偿。
  - **鲜血死亡骑士 (`bot_blood_death_knight`)：** 符文/符能状态机模拟、冰冷触摸爆发仇恨、灵界打击动态已损回血、吸血鬼之血减伤。
  - **野性熊坦 (`bot_bear_druid`)：** 怒气消耗模拟、重殴伪队列机制、横扫群拉与裂伤仇恨构建、狂暴回复与生存本能极限保命。
- **治疗集群（阶段一首发完成）：**
  - **神圣圣骑士 (`bot_holy_paladin`)：** 经 13 轮深度推演完成单体道标折射、圣光术雕文溅射群抬、圣洁护盾维持、神圣震击跑位急救、秒拔神圣祈求减疗、自律解耦、防顺劈背身跟随与贴坦避难防抖闭环。
- **后续待开发专精：**
  - 牧师（戒律/神圣）、萨满（恢复）、德鲁伊（恢复）。
- **后续中顶层架构链路：**
  - 中层：`CombatAttributionAnalyzer`（脱战与击杀时触发的异步致死与打断归因）。
  - 顶层：`BotPersistentMemory`（SQLite 本地数据表，读写 Boss 机制参数覆盖 APL 阈值）。