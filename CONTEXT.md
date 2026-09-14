```markdown
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

* **维度 A（打断演进）：** 从“盲目秒断”演化为“识别 Boss 灭团级致命技能，并在读条达到 80% 时压秒打断”，最大化输出偷跑窗口。
* **维度 B（走位自适应）：** 归因器提取高危致死技能来源坐标，将其标记为不可停留禁区，随从在 Boss 施法前摇阶段提前执行侧移规避。
* **维度 C（仇恨协同）：** 依据玩家主坦建立仇恨的前 10 秒 TPS，动态延后输出随从的爆发技能与主力饰品激活时间（0~10s），根除起手 OT 猝死。
* **维度 D（阶段对齐）：** 记忆 Boss 阶段转换阈值（如 20% 狂暴/斩杀期、易伤虚弱期），在机制最关键窗口集中交出嗜血/英勇与核心爆发底牌。

---

## 2. 基类约定与核心可用 API (AdaptiveBotAI.h)

所有随从专精必须继承自统一基类 `AdaptiveBotAI`，且必须优先复用以下基类契约与接口：

### 虚函数重写规范

* `bool IsHealerBot() const override`：治疗专精返回 `true`。


* `bool IsRangedBot() const override`：远程与治疗返回 `true`，接管远程站位与移动控制。


* `float GetDamageDealtMultiplier() const override`：治疗专精压低输出，固定返回 `0.25f`。


* `uint8 GetTalentSpellMinLevel(uint32 spellId) const override`：以 `switch-case` 显式注册天赋与特权技能的最低解锁等级。


* `void Reset() override`：配置能量池类型，清空本地状态机标记，调用基类 `Reset()` 并执行 `ApplyPassiveTalents()`。


* `void OnLevelSynced(uint8 level) override`：同步等级与法力池，调用 `ApplyPassiveTalents()`。



### 核心基类可用接口

* **法术判定与执行：**
* `uint32 GetAppropriateRank(uint32 spellId, bool isTalent = false) const;`（凡在 `GetTalentSpellMinLevel` 声明的技能必须传 `true`）
* `bool CanCast(Unit* target, uint32 spellId, bool checkRangeAndLOS = true) const;`

* `bool ExecuteSpell(Unit* target, uint32 spellId, bool checkRangeAndLOS = true);`



* **目标定位与仇恨：**
* `Unit* SelectAssistTarget();`（获取协助目标）


* `Unit* GetGroupTank();`（获取团队主坦）


* `Player* GetMaster();`（获取随从指挥官）


* `me->Attack(victim, false);`（远程/治疗仅锚定目标供法术施放，第二参数传 `false` 绝不开启近战追击）




* **移动与控制：**
* `void UpdateFollowMaster(uint32 diff);`（脱战通用跟随）


* `me->GetMotionMaster()->MoveFollow(target, dist, angle);`

* `me->StopMoving();`（仅在决定施放非瞬发读条法术前夕按需调用）





---

## 3. 必须严格遵守的底层引擎铁律 (Avoidance Guide)

1. **全局施法与通道双保险守卫（Casting & Channel Guard）：** 治疗与法系 `UpdateAI()` 顶部必须执行 `if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL)) return;`，杜绝长读条或引导类法术（如苦修、希望圣歌）被跟随移动指令打断。


2. **MoveFollow 相对弧度机制：** `MoveFollow(target, dist, angle)` 中的 `angle` 原生代表相对目标当前朝向的弧度偏移（引擎内部自动叠加目标朝向）。**严禁写成 `target->GetOrientation() + angle**`。正后方背身位恒传 `static_cast<float>(M_PI)`，贴坦避难传背身侧向，严禁传 `0.0f`（防止冲到 Boss 脸前吃顺劈与吐息暴毙）。


3. **天赋契约参数闭环：** 3.3.5a 中所有天赋法术在 DBC 中的 `SpellLevel` 均为 0。调用 `GetAppropriateRank(id, isTalent)` 时，所有在 `GetTalentSpellMinLevel` 注册的技能第二参数必须显式为 `true`；基础法术（如暗影魔、希望圣歌、基础图腾、滋养、生命之绽）传 `false`。


4. **驱散正向光环过滤与类型精准解耦：**
* 队友自身的增益在底层属性中多为可驱散类型，驱散时必须校验 `if (application->IsPositive()) continue;`，防止无限驱散自己人直至空蓝。


* 驱散通道必须解耦独立判定，严禁使用级联 `if (!dispel)` 导致高优先级驱散就绪后彻底短路其他驱散类型。


* 各职业驱散范围必须硬性收敛：萨满仅解诅咒/疾病/中毒（杜绝魔法）；德鲁伊仅解诅咒与中毒（`DISPEL_CURSE | DISPEL_POISON`，杜绝魔法与疾病）。




5. **宠物特权与血线彻底剥离：**
* 打地鼠雷达的 `lowestHpAlly`、`lowestHpPct` 与 `averageHpPct` 严禁统计宠物（`unit->ToPet() != nullptr`），防止宠物掉血引发全队恐慌。


* 守护之魂、痛苦压制、圣疗术、真言术：盾、大地之盾、迅捷治愈、生命之绽等核心救命大招与主力 HoT 严禁对宠物施放。




6. **战时急救门禁覆盖（防治疗倒挂）：**
* 辅助增益、全团预铺盾、长读条群抬或图腾矩阵展开前，必须检查团队最低血线，全队出现重伤（`< 60%~70%`）时施法权必须无条件让渡给单体急救。




7. **瞬发跑位施法与 StopMoving 解耦：** `me->StopMoving()` 必须精准下沉至确认施放读条法术前夕；瞬发技能（真言术：盾、愈合祷言、神圣震击、激流、回春术、迅捷治愈、野性成长）允许在跑位或抱坦避难途中直接施放，保障机动性。


8. **Off-GCD 技能防空转与当帧顺下：** 对不占公共冷却的瞬发法术（如心灵专注、潮汐之力、自然迅捷），施放成功后**严禁 `return true**`，必须允许当帧决策流顺下执行，使增益光环能当帧立即被后续技能吃下。


9. **光环防顶守卫：** 弹跳类或唯一类光环（愈合祷言），必须全团扫描光环存在性，避免重复施放白白顶掉层数或浪费 GCD。
10. **叠层 Buff 与充能 Buff 检测解耦（Stacks vs Charges）：**
* 叠加型光环（如好运、潮汐奔涌、生命之绽三花）必须使用 `Aura* a = target->GetAura(id); a->GetStackAmount() >= N` 检测层数。


* 充能扣减型光环（如大地之盾初始 9 次充能）在底层不使用 `StackAmount`，必须通过 `aura->GetCharges() > 2` 校验剩余次数，杜绝无脑刷新空蓝。




11. **元素护盾排己守卫（Dual Shield Conflict）：** 萨满同目标只能存在一种元素护盾。主坦锚点退化时（如单人无坦），`MaintainEarthShield` 必须显式排除 `tank == me`，随从自身恒定维持水之护盾回蓝，大地之盾仅对他人主坦维持，杜绝双盾互顶死循环。


12. **瞬发战略大招防吞保护：** 自然迅捷等爆发瞬发增益，激活后若濒死目标脱离阈值，必须降级给全队最低血线成员立即当帧打出瞬发大加（治疗波/治疗之触），绝不允许光环遗留至后续低级填充技（次级治疗波、回春）造成浪费。


13. **形态锁定与施法限制破除（Shapeshift Exemption & Instant Shifting）：**
* 3.3.5a 生命之树形态底层硬性禁止施放平衡系技能（野性印记、驱除诅咒）以及非瞬发大加【治疗之触】。


* 即使 `CheckShapeshiftExemption` 放行了 `CanCast`，原生底层 `Spell::CheckCast` 仍会拦截返回 `SPELL_FAILED_NOT_SHAPESHIFT`。


* 随从在施放救急瞬发治疗之触、脱战补爪子或驱除诅咒时，必须瞬拔树形态（`me->RemoveAurasDueToSpell(TREE_OF_LIFE)`，无 GCD）；救急/维护结束后由 `MaintainTreeOfLife` 自动变回。




14. **主坦双 HoT 优先级防饥饿调度（EDF Scheduling）：**
* 奶德维护主坦 HoT 时，回春术（迅捷治愈跳板）缺失时必须前置优先补齐。


* 仅在三花（生命之绽）极度濒危（`GetDuration() < 2000ms`）时才允许抢先续订三花，避免三花无限刷新导致回春永久饿死。


* 放宽三花补刷窗口至 `LIFEBLOOM_REFRESH_WINDOW = 4000ms`，与 3000ms 巡检周期无缝接续不掉层。




15. **愈合直接治疗兜底（forceDirectHeal）：**
* 80 级前随从尚未习得【滋养】（80 级技能），高压期若目标已挂有愈合 HoT，常规防顶检测会导致愈合直接被拒。


* 在 `< 60%` 重伤紧急分支中必须支持 `TryRegrowth(target, true)` 强制直疗模式，旁路 HoT 防顶限制，吃下愈合的直接治疗量保坦。





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

### 坦克集群（阶段一全部交付，具备副本实战品质）

* **防护战士 (`bot_protection_warrior`)：** 怒气自适应平滑注入、盾猛/复仇核心仇恨链、雷霆/冲击波群拉、盾牌格挡与破釜/盾墙梯级减伤、拦截破阵与断筋风筝。
* **防护圣骑士 (`bot_protection_paladin`)：** 969 技能循环、正义防御与神圣庇护多目标嘲讽路由、神圣之盾常驻维持、炽热防御者免死补偿。
* **鲜血死亡骑士 (`bot_blood_death_knight`)：** 符文/符能状态机模拟、冰冷触摸爆发仇恨、灵界打击动态已损回血、吸血鬼之血减伤。
* **野性熊坦 (`bot_bear_druid`)：** 怒气消耗模拟、重殴伪队列机制、横扫群拉与裂伤仇恨构建、狂暴回复与生存本能极限保命。

### 治疗集群（阶段一全五专精闭环交付并封存）

* **神圣圣骑士 (`bot_holy_paladin`)：** 单体道标折射、圣光术雕文溅射群抬、圣洁护盾维持、神圣震击跑位急救、秒拔神圣祈求减疗、自律解耦、防顺劈背身跟随与贴坦避难防抖。
* **戒律牧师 (`bot_discipline_priest`)：** 灵魂护体（0 CD 盾）、狂喜回蓝、争分夺秒（25% 急速）、主坦独立维持盾、单体急救盾与苦修首跳无缝衔接、绝望祷言自保、痛苦压制防猝死、魔法/疾病解耦驱散。
* **神圣牧师 (`bot_holy_priest`)：** 守护之魂 40% 受疗与免死急救、雕文 6 目标治疗之环智能群抬、圣光涌动 Proc 瞬发免费快疗、好运 2 层联动 1.8 秒大加、联结治疗双向回血叠层、希望圣歌长通道单体安全门禁、冥想精神回蓝补偿。
* **恢复萨满 (`bot_restoration_shaman`)：** 大地之盾充能与排己维护、水之护盾常驻回蓝、自然迅捷防误吞原子瞬发大加、激流与潮汐奔涌（Tidal Waves）层数联动加速治疗波、雕文 4 目标治疗链智能跳跃（55% 濒死安全门禁）、四大图腾矩阵平滑展开（2000ms 单插、25 码位移重置、80% 战时血线门禁）、解耦三系驱散（绝不误驱魔法）。


* **恢复德鲁伊 (`bot_restoration_druid`)：**
* **形态管理：** 生命之树形态维持；施放自然迅捷治疗之触、脱战野性印记、驱除诅咒时无 GCD 瞬拔树形态绕过底层引擎限制。


* **主坦 HoT 滚动：** 回春术前置防饥饿、生命之绽三花滚动（4000ms 窗口续花）、2000ms 濒危让位抢帧保护，维持主坦双 HoT 齐备。


* **急救与爆发：** 树皮术瞬发自保减伤；自然迅捷 + 瞬发治疗之触防吞原子大加；迅捷治愈雕文（不吞 HoT 瞬发爆发）；野性成长智能 6 目标群抬（55% 单体安全门禁）。


* **填充与续航：** 80 级前或高压期愈合强制直接治疗兜底（`forceDirectHeal`）；滋养吃 HoT 增效；激活战时门禁续航（法力 < 40% 且最低血线 >= 60%）；注入【强烈】50% 施法精神回蓝。


* **驱散与站位：** 严格限定解诅咒与中毒（杜绝魔法/疾病）；18 码坦克正后方背身跟随（`BEHIND_ANGLE = M_PI`）与 2 码贴坦避难脱困状态机。





---

## 6. 下一阶段：输出专精集群（阶段二）开发规划

治疗集群已全部封存，开发重心转移至输出专精集群。

### 核心设计原则与依赖

1. **维度 C（仇恨协同）：**
* 输出随从起手严禁秒交主力爆发与饰品。
* 依赖归因分析器对玩家主坦前 10 秒 TPS 的测定，动态施加 0~10 秒的起手爆发延迟与平稳起手垫刀机制，彻底根除起手 OT 猝死。


2. **移动与射程约束：**
* 远程专精（猎/法/术/鸟/暗/元）继承 `IsRangedBot() -> true`，复用背身跟随与防近战贴脸机制。
* 物理近战（贼/狂暴战/惩戒骑/猫/邪DK/增强萨）需开发近战专用背后找背状态机（规避招架与顺劈）。


3. **首期输出专精候选：**
* 物理远程核心：**猎人（射击/生存）**（假死清仇恨、误导给主坦、守护光环、宠物行为树配合）。
* 法系输出核心：**法师（奥术/冰霜/火焰）**（奥术弹幕/奥冲叠层泄蓝循环、隐形术清仇恨、法力宝石、唤醒续航、活动炸弹多目标）。



```

```