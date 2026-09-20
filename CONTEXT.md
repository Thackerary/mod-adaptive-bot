# mod-adaptive-bot 项目核心架构与开发上下文记忆\[cite: 40]

## 1\. 核心架构模式：分层自适应系统 (Hierarchical Adaptive System)\[cite: 40]

本项目基于 AzerothCore (WoW 3.3.5a) 构建轻量化、不侵入核心源码的随从 AI，采用三层解耦闭环架构\[cite: 40]：

```text
\\\[ 顶层：持久化机制记忆库 (SQLite) ]
  - 存储索引：(Boss\\\_Entry\\\_ID, Bot\\\_GUID / Spec\\\_ID)
  - 动态下发：Boss 技能轴、高危禁区坐标、打断压秒时机、玩家坦克 TPS 仇恨延后阈值
         ▲
         │ 异步反哺 / 参数覆盖
\\\[ 中层：战斗回溯与归因分析器 (Post-Combat Attribution) ]
  - 触发时机：仅在团灭脱战 (EnterEvadeMode) 或 Boss 击杀 (JustDied) 时异步执行，零战斗运行时 CPU 开销
  - 分析维度：承伤与致死 SpellID、漏打断统计、起手 OT 归因、主坦前 10 秒 TPS 速率测定
         ▲
         │ 运行时数据沉淀
\\\[ 底层：专家动作优先级列表 (APL Baseline) + 伴随型轻量战斗护卫体系 ]
  - 状态：全职业 31 专精基线 + 轻量伴随护卫体系已全量交付并闭环封存
  - 运行实体：轻量级 CreatureAI，基于 OnLevelSynced 动态属性投影与 ApplyPassiveTalents 满阶根源补偿
  - 核心功能：提供开箱即用 70 分 baseline，执行由顶层动态微调的实战战术
```

### 顶层进化四维定义\[cite: 40]

* **维度 A（打断演进）：** 从“盲目秒断”演化为“识别 Boss 灭团级致命技能，并在读条达到 80% 时压秒打断”，最大化输出偷跑窗口\[cite: 40]。
* **维度 B（走位自适应）：** 归因器提取高危致死技能来源坐标，将其标记为不可停留禁区，随从在 Boss 施法前摇阶段提前执行侧移规避\[cite: 40]。
* **维度 C（仇恨协同）：** 依据玩家主坦建立仇恨的前 10 秒 TPS，动态延后输出随从的爆发技能与主力饰品激活时间（0\~10s），根除起手 OT 猝死\[cite: 40]。
* **维度 D（阶段对齐）：** 记忆 Boss 阶段转换阈值（如 20% 狂暴/斩杀期、易伤虚弱期），在机制最关键窗口集中交出嗜血/英勇与核心爆发底牌\[cite: 40]。

\---

## 2\. 基类约定与核心可用 API (AdaptiveBotAI.h)\[cite: 40]

所有随从专精必须继承自统一基类 `AdaptiveBotAI`，且必须优先复用以下基类契约与接口\[cite: 40]：

### 虚函数重写规范\[cite: 40]

* `bool IsHealerBot() const override`：治疗专精返回 `true`，输出/坦克专精返回 `false`\[cite: 39, 40]。
* `bool IsRangedBot() const override`：远程与治疗返回 `true`，接管远程站位；物理近战返回 `false`\[cite: 39, 40]。
* `bool IsRangedPhysicalBot() const override`：猎人重写为 `true`，与法系远程的法伤加成彻底解耦\[cite: 39, 40]。
* `float GetDamageDealtMultiplier() const override`：治疗专精压低输出返回 `0.25f`；纯输出与坦克专精固定返回 `1.0f`（严禁误继承法系随从的 2.0x\~3.3x 装备倍率通道）\[cite: 39, 40]。
* `uint8 GetTalentSpellMinLevel(uint32 spellId) const override`：以 `switch-case` 显式注册纯天赋技能的最低解锁等级\[cite: 39, 40]。
* `void Reset() override`：配置能量池类型（法力/怒气/能量/符能），清空专精自管计时器与状态机标记，调用基类 `Reset()` 并执行 `ApplyPassiveTalents()`，触发 `EnsureGuardianAlive()`\[cite: 39, 40]。
* `void OnLevelSynced(uint8 level) override`：同步等级与对应资源池上限，调用 `ApplyPassiveTalents()` 注入被动光环补偿\[cite: 39, 40]。

### 伴随护卫体系虚函数与接口契约

* `virtual bool ShouldHaveGuardian() const`：判定当前专精是否常驻伴随护卫。基类默认仅放行 `CLASS\\\_HUNTER` 与 `CLASS\\\_WARLOCK`\[cite: 39]；邪DK显式覆写返回 `true`\[cite: 38]，血DK与冰DK显式覆写返回 `false`\[cite: 36, 37]。
* `virtual GuardianVisualType GetPreferredGuardianVisualType() const`：专精级随从内核与外观池偏好分流契约。基类提供职业默认，专精类（如痛苦术/恶魔术/毁灭术/邪DK）通过重写精准下发绑定类型\[cite: 27, 28, 29, 38, 39]。
* `void EnsureGuardianAlive()`：护卫保活仲裁器。在宿主出生/重置（`Reset`）、战后结算（`OnCombatEnded`）及脱战 3 秒轮询中巡检，阵亡或丢失时以 `TEMPSUMMON\\\_MANUAL\\\_DESPAWN` 模式唤出 `NPC\\\_BOT\\\_GUARDIAN (990001)` 并当帧完成归属绑定与从属 `Reset()`\[cite: 39]。

### 核心基类可用接口\[cite: 40]

* **法术判定与执行：**\[cite: 40]

  * `uint32 GetAppropriateRank(uint32 spellId, bool isTalent = false) const;`（纯天赋技能传 `true`，基础技能传 `false`）\[cite: 39, 40]
  * `bool CanCast(Unit\\\* target, uint32 spellId, bool checkGcd = true, bool verbose = false) const;`\[cite: 39, 40]
  * `bool ExecuteSpell(Unit\\\* target, uint32 spellId, bool applyGcd = true, Unit\\\* facingTarget = nullptr);`\[cite: 39, 40]
* **目标定位与仇恨：**\[cite: 40]

  * `Unit\\\* SelectAssistTarget();`（获取协助目标）\[cite: 39, 40]
  * `Unit\\\* GetGroupTank();`（获取团队主坦）\[cite: 39, 40]
  * `Player\\\* GetMaster();`（获取随从指挥官）\[cite: 39, 40]
  * `me->Attack(victim, false);`（远程与治疗仅锚定目标供法术施放，第二参数传 `false` 绝不开启近战追击）\[cite: 39, 40]
  * `me->Attack(victim, true);`（物理近战必须传 `true` 开启底层自动白字挥砍与双持平砍循环）\[cite: 39, 40]
* **移动与控制：**\[cite: 40]

  * `void UpdateFollowMaster(uint32 diff);`（脱战通用跟随）\[cite: 39, 40]
  * `me->GetMotionMaster()->MoveFollow(target, dist, angle);`\[cite: 39, 40]
  * `me->StopMoving();`（仅在决定施放非瞬发读条法术前夕按需调用，瞬发与移动施法严禁调用）\[cite: 39, 40]

\---

## 3\. 必须严格遵守的底层引擎铁律 (Avoidance Guide)\[cite: 40]

1. **全局施法与通道双保险守卫：** 治疗与法系 `UpdateAI()` 顶部必须执行 `if (me->HasUnitState(UNIT\\\_STATE\\\_CASTING) || me->GetCurrentSpell(CURRENT\\\_CHANNELED\\\_SPELL)) return;`，杜绝长读条或引导类法术被跟随移动指令打断\[cite: 27, 28, 29, 40]。
2. **MoveFollow 相对弧度机制：** `MoveFollow(target, dist, angle)` 中的 `angle` 原生代表相对目标当前朝向的弧度偏移（引擎自动叠加目标朝向）\[cite: 27, 28, 29, 40]。**严禁写成 `target->GetOrientation() + angle`**\[cite: 27, 28, 29, 40]。正后方背身位恒传 `static\\\_cast<float>(M\\\_PI)`，严禁传 `0.0f`\[cite: 27, 28, 29, 40]。
3. **天赋契约参数闭环：** 3.3.5a 中纯天赋法术在 DBC 中的 `SpellLevel` 均为 0\[cite: 40]。调用 `GetAppropriateRank(id, isTalent)` 时，在 `GetTalentSpellMinLevel` 注册的技能第二参数必须显式为 `true`；基础技能必须传 `false`\[cite: 39, 40]。
4. **驱散正向光环过滤与类型解耦：** 校验 `if (application->IsPositive()) continue;` 防止驱散友方增益；驱散通道解耦独立判定；萨满仅解诅咒/疾病/中毒，德鲁伊仅解诅咒与中毒\[cite: 40]。
5. **宠物特权与血线彻底剥离：** 打地鼠雷达严禁统计宠物（`unit->ToPet() != nullptr`）；救命大招与核心 HoT 严禁对宠物施放\[cite: 40]。
6. **战时急救门禁覆盖（防治疗倒挂）：** 辅助增益、全团预铺盾、长读条群抬或插图腾前，全队出现重伤（`< 60%\\\~70%`）时施法权必须无条件让渡给单体急救\[cite: 40]。
7. **瞬发跑位施法与 StopMoving 解耦：** `me->StopMoving()` 必须精准下沉至确认施放非瞬发读条法术前夕；瞬发法术放行移动中施放\[cite: 27, 28, 29, 40]。
8. **Off-GCD 技能防空转与当帧顺下：** 对不占 GCD 的瞬发法术施放成功后**严禁 `return true`**，必须允许当帧决策流顺下执行，使增益当帧立即被后续技能消费\[cite: 27, 28, 39, 40]。
9. **光环防顶守卫：** 弹跳类或唯一类光环（如愈合祷言），必须全团扫描光环存在性，避免重复施放顶掉层数或浪费 GCD\[cite: 40]。
10. **叠层 Buff 与充能 Buff 检测解耦：** 叠加型光环（好运、漩涡武器）用 `GetStackAmount()` 检测；充能扣减型光环（地盾、闪电盾）通过 `aura->GetCharges() > threshold` 校验剩余次数\[cite: 40]。
11. **元素护盾排己守卫：** 萨满同目标仅存一种元素盾；主坦锚点退化为自身时强制排除 `tank == me`，自身维持水盾/电盾，地盾仅对他人主坦维持\[cite: 40]。
12. **瞬发战略大招防吞保护：** 自然迅捷等爆发瞬发激活后若原目标脱离濒死，必须降级给全队最低血线成员立即打出瞬发大加，杜绝光环遗留浪费\[cite: 40]。
13. **形态锁定与施法限制破除：** 脱战补爪子/坚韧/心火或驱散时瞬拔形态（无 GCD），补完切回；战时绝对禁止拔形态\[cite: 40]。
14. **主坦双 HoT 优先级防饥饿调度：** 奶德回春术缺失时前置优先补齐；仅在三花极度濒危（`< 2000ms`）才抢先续订，放宽补刷窗口至 4000ms，杜绝回春饿死\[cite: 40]。
15. **愈合直接治疗兜底：** 80 级前尚未习得滋养，在 `< 60%` 重伤紧急分支中必须支持 `TryRegrowth(target, true)` 强制直疗模式，旁路 HoT 防顶限制\[cite: 40]。
16. **物理近战背后找背与防同心圆死锁：** 目标看主坦时严格站位正后方 1.5 码规避顺劈与招架加速；怪物看随从本人时（`victim->GetVictim() == me`）禁止绕后，切回 `MoveChase(victim, 1.5f)` 直线贴身硬刚\[cite: 37, 38, 40]。
17. **远程近战盲区撤离迟滞区间：** 猎人/法系远程必须建立迟滞区间：`< 8.0f` 触发撤退（`isRetreating = true`），且必须退至 `>= 15.0f`（或 16.0f）安全线后才允许清空走位发生器立定施法，杜绝原地高频抽搐走停\[cite: 27, 28, 29, 40]。
18. **猎人远程自动射击通道维持：** 8\~35 码内必须显式维持 `CURRENT\\\_AUTOREPEAT\\\_SPELL` 通道的 `AUTO\\\_SHOT`（`checkGcd = false`），保证白字输出与蝰蛇回蓝\[cite: 40]。
19. **随从武器毒药与命中模拟：** 依赖武器毒药的潜行者专精，必须在白字平砍与技能命中后以节流方式（1000ms）向目标触发注入 `DEADLY\\\_POISON`（维持 5 层）与 `INSTANT\\\_POISON`\[cite: 40]。
20. **近战终结技攒能挂起与产星通道锁死：** 连击点达到 4\~5 星门槛但能量不足支付终结技（< 35 能量）时，必须原地挂起等待能量跳动；达到 4\~5 星后直接锁死产星通道，严格禁止降级施放低级技能偷跑能量\[cite: 40]。
21. **自保免疫/假死/隐形脱困保护期与超时逃逸：** 寒冰屏障设立 1500ms 最小保护期、50% 安全血线与 2.5s 超时兜底；假死设立 1500ms 硬性超时逃逸；隐形术强制跑满 3000ms 淡入期确保仇恨 100% 清零\[cite: 40]。
22. **长引导与前置光环防吞保护：** 气定神闲开启门禁必须显式排除飞弹速射（`!HasAura(AURA\\\_MISSILE\\\_BARRAGE)`），防止爆发光环被零耗蓝引导飞弹吞噬\[cite: 40]。
23. **触发型被动天赋根源注入：** 随从依赖暴击触发的瞬发光环（战争艺术、血涌、血之气息、猝死），必须在 `ApplyPassiveTalents` 中手工注入对应的**被动天赋根源 ID**，杜绝特效永久哑火\[cite: 40]。
24. **圣盾术清仇恨主动解除与自律梯级：** 遵循“拯救之手（常规 OT 且 >= 20%） $\\to$ 圣疗术（< 15% 且无自律） $\\to$ 圣盾术（< 20% 且濒死/OT 且无自律）”；无敌开启满 2000ms 且仇恨安全时主动点掉光环消除 -50% 伤害惩罚\[cite: 40]。
25. **斩杀期怒气抽空饥饿反转：** 狂暴战斩杀会抽空怒气，循环必须执行“嗜血 $\\to$ 旋风斩 $\\to$ 血涌猛击 $\\to$ 斩杀填充”，斩杀仅作核心打击 CD 期间的泄怒补充\[cite: 40]。
26. **平砍队列控怒门禁与双持惩罚移除：** 狂暴战 50 怒气门禁、武器战 65 怒气门禁，配置 1200ms 队列节流；斩杀期封锁英勇打击防抽空怒气\[cite: 40]。
27. **激怒急救同帧顺下：** 狂暴回复前置激怒通过狂暴之怒/血性狂暴补齐后，必须在当帧顺下施放狂暴回复，杜绝怒气被打击抽空导致急救被拒\[cite: 40]。
28. **引导类斩杀通道等级自适应与断档主动破锁：** 引导吸取灵魂/精神鞭笞途中核心增伤（鬼影缠身、触）断档且 CD 就绪时，主动调用 `me->InterruptSpell` 掐断通道重补增伤；门禁须做低级未习得自适应\[cite: 29, 40]。
29. **异常资源消耗通道底层直放：** 生命分流（Life Tap）等以生命值（`POWER\\\_HEALTH`）为消耗底座的法术，必须彻底绕开 `CanCast`，直接调用 `me->CastSpell(me, spellId, true) == SPELL\\\_CAST\\\_OK` 触发式直放，由专精自管安全血线（> 50%）与 GCD\[cite: 27, 28, 29, 40]。
30. **刷新类被动与首发施加解耦：** 永恒痛苦等刷新机制无法凭空施加 DoT，起手或转火必须强制先验 `GetOwnDotRemaining == 0` 主动挂上基础 DoT\[cite: 29, 40]。
31. **读条减免天赋与移动施法解耦：** 类似强化腐蚀术等使法术瞬发的天赋，注入根源后必须放行移动中施放，仅对非瞬发读条才在 `CanCast` 通过后刹停\[cite: 29, 40]。
32. **日月蚀双相记忆状态机：** 引入 `PHASE\\\_SEEKING\\\_LUNAR` 与 `PHASE\\\_SEEKING\\\_SOLAR` 记忆相位：月蚀期间打星火并锁入找日蚀相，结束后在空窗期坚决继续读星火打入日蚀；日蚀期间打愤怒并锁入找月蚀相，结束后空窗期继续读愤怒打入月蚀\[cite: 40]。
33. **多阶被动天赋满级 Rank 注入契约：** 必须显式核对并注入 **Rank 3/5 满阶 Spell ID**，严禁注入 DBC 默认 Rank 1 根源造成触发率与数值缩水（如仔细瞄准 Rank 3 `34484`、硫磺与烈火 Rank 5 `47252`、灾祸 Rank 5 `17792` 等）\[cite: 28, 40]。
34. **瞬发光环型法术 GCD 闭环与非通道判定：** 星辰坠落为独立瞬发光环而非引导通道，严禁计入通道守卫；激活、星落、横扫攻击等占 GCD 技能施放后当帧交还控制权\[cite: 40]。
35. **锥形方向性法术朝向校正：** 台风、龙息术、冰锥术等自身原点正面锥形法术，施放前必须显式调用 `me->SetFacingToObject(victim)` 锁定朝向\[cite: 40]。
36. **远程填充技能与全局射程收敛：** 暗牧精神鞭笞（24 码）等短射程填充技，专精的最大交战距离与理想站位必须严格向下收敛（24.0f / 20.0f），防止在 24\~30 码出现既不读条也不前压的发呆死锁\[cite: 40]。
37. **被动天赋根源 ID 与触发光环严格解耦：** 必须注入触发源天赋根源，严禁误注入 15s 临时增益光环（会自然脱落）或目标易伤 Debuff（会误挂在随从头上）\[cite: 27, 28, 40]。
38. **近战找背运动学与读条法术同帧守卫：** 近战读条（猛击 0.5s、碎裂投掷 1.5s）施放后当帧 `return`，找背状态机顶部置位读条守卫，杜绝读条被同帧下发的 `MoveFollow` 秒断\[cite: 40]。
39. **大招旋风通道与活体转火跟随：** 利刃风暴期间锁定打击但放行 `MoveChase` 贴身跟随；目标中途死亡当帧调用 `SelectAssistTarget()` 切换活体目标\[cite: 40]。
40. **敌对目标增益法术目标类型契约：** 预谋等在 DBC 中硬性定义为敌对目标的技能，施法目标必须传 `victim`，传友方 `me` 必被底层拒放\[cite: 40]。
41. **重置类技能（Preparation / Cold Snap）门禁与清零严格对齐：** 统计口径必须与施放后的清零列表完全一致；且若对应核心大招光环仍在身上（如冰冷血脉爆发中），严禁开启重置大招造成自吞\[cite: 40]。
42. **非玩家单位连击点事件注入补偿：** 随从无法触发 `EffectAddComboPoints`，盗贼的尊严等机制必须在专精中以光环判定配合 1000ms 内部节流手工调用 `AddComboPoints(victim, 1)`\[cite: 40]。
43. **3.3.5a DK 输出姿态契约：** 双持冰与邪恶 DK PvE 恒定维持【鲜血灵气】（+15% 伤害与 4% 吸血），严禁开启高仇恨冰霜灵气\[cite: 37, 38, 40]。
44. **Creature 随从符能产出模拟补偿：** 消耗符文打击技命中后显式调用 `me->ModifyPower(POWER\\\_RUNIC\\\_POWER, ...)` 补偿符能（大打击 +20，常规 +10，符文武器 +25），解决无玩家符文系统的符能断供\[cite: 37, 38, 40]。
45. **石像鬼敌对目标与符能蓄水池仲裁：** 召唤石像鬼为敌对 30 码法术，目标传 `victim`；CD 临近（$\\le 10\\text{s}$）或就绪时封锁常规凋零缠绕进行 60 符能蓄水，符能 $\\ge 800$ 时才放行防溢能\[cite: 38, 40]。
46. **NPC 触发型增益状态机补偿：** 鲜血打击命中手工调用 `AddAura(AURA\\\_DESOLATION, me)` 补齐荒芜 20s +5% 增伤，严禁在 `ApplyPassiveTalents` 中当做永久被动注入\[cite: 38, 40]。
47. **低等级前置法术依赖让路门禁：** 双持冰 DK 未习得 60 级凛风冲击时，白霜触发不可让路，必须校验 `HasTalent(HOWLING\\\_BLAST)`，未习得坚决打冰触补病\[cite: 37, 40]。
48. **杀戮盛宴位移托管与同帧走位互斥：** 杀戮盛宴施放成功当帧直接 `return` 独占整帧控制权，严禁同帧顺下执行找背走位指令打乱瞬移\[cite: 40]。
49. **漩涡武器满层核弹多目标降级兜底：** 满 5 层漩涡武器多目标优先闪电链；若处于 CD 或等级未达标，必须平滑回退单体闪电箭兜底，杜绝发呆\[cite: 40]。
50. **火焰新星图腾落点追踪与几何爆破校验：** 插图腾时快照记录坐标 `lastFireTotemX/Y/Z`；施放火焰新星前校验目标处于图腾 10 码爆破半径内，杜绝盲打空爆\[cite: 40]。
51. **随从被动回能机制平砍节流补偿：** 战斗潜能与漩涡武器叠层在白字平砍后通过近战贴身判定结合 1000ms 内部节流手工进行概率补偿\[cite: 40]。
52. **压秒打断对长引导通道双检支持：** 打断技能检测敌方施法时，必须同时兼顾 `CURRENT\\\_GENERIC\\\_SPELL` 与 `CURRENT\\\_CHANNELED\\\_SPELL`（ChannelInterruptFlags != 0）\[cite: 40]。
53. **REACT\_PASSIVE 与 UpdateVictim 冲突彻底破除（随从平砍铁律）：** 伴随型随从设定为 `REACT\\\_PASSIVE` 以剥离自主警戒索敌权\[cite: 25]；引擎底层 `UpdateVictim()` 在被动状态下恒返回 `false`\[cite: 25]。随从平砍必须绕开 `UpdateVictim()` 守卫，直接调用 `DoMeleeAttackIfReady()` 驱动挥砍，杜绝贴身发呆零伤害\[cite: 25]。
54. **远程施法运动发生器清空防断条（小鬼读条铁律）：** 随从在下发 `MoveChase` 接近目标并进入法术射程（$\\le 30.0\\text{f}$）立定施法时，仅调 `StopMoving()` 无法清除底层的追击生成器，后续寻路微移会掐断读条\[cite: 25]。必须先执行 `if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE\\\_MOTION\\\_TYPE) me->GetMotionMaster()->Clear();` 彻底清空生成器，再刹车打出法术\[cite: 25]。
55. **实体生成初始化时序与三级宿主回溯（Creator 兜底铁律）：** `me->SummonCreature()` 内部触发随从构造与首次 `Reset()` 时，宿主尚未执行 `SetOwnerGUID`\[cite: 25, 39]。随从端必须实现 `GetMaster()` 经“魅惑者 $\\to$ 拥有者 $\\to$ 创建者（CreatorGUID）”三级回溯，确保出生瞬间正确识别宿主专精\[cite: 25]；宿主在赋予所有权后必须显式调用一次 `summon->AI()->Reset()` 重新刷新属性与法力池\[cite: 39]。
56. **表现层外观双重设值与体型归一化铁律：** 随从外观随机化必须双重设值 `me->SetDisplayId` 与 `me->SetNativeDisplayId`，防止受控/变形光环结束后回退为模板白板\[cite: 25]；体型调整仅改动 `me->SetObjectScale`，严禁改动服务端的 `combat\\\_reach` 与 `bounding\\\_radius`，保证找背与攻击几何恒定\[cite: 25]。
57. **法系随从零耗蓝锁能与数值倍率隔离铁律：** 小鬼等法系随从每帧读条前执行 `me->SetPower(POWER\\\_MANA, max)` 彻底根除 OOM\[cite: 25]；随从无角色装备法强，小鬼注入系统伤害增效光环 `23568`（80 级 +120%）\[cite: 25]，且近战随从（座狼/食尸鬼/地狱犬/恶魔卫士）必须显式排他剥离该光环，防止平砍数值溢出\[cite: 25]。
58. **随从雷达隔离与宿主生命周期销毁守卫：** 治疗雷达 `SelectLowestHealthAlly` 必须显式剔除 `BotGuardianAI` 杜绝治疗倒挂\[cite: 39]；随从 `UpdateAI` 头部严格校验宿主状态，一旦宿主阵亡、离开世界或跨地图（`GetMap() != owner->GetMap()`），随从同帧调用 `DespawnOrUnsummon()` 彻底销毁，杜绝孤儿木桩与跨图崩溃断言\[cite: 25]。

\---

## 4\. 治疗专精通用「打地鼠雷达」标准实现 (GroupSnapshot)\[cite: 40]

```cpp
struct GroupSnapshot
{
    std::vector<Unit\\\*> allies;          // 40 码内、视线可达、存活的友方单位
    Unit\\\* lowestHpAlly{ nullptr };      // 全队最低生命百分比成员 (严格排除宠物与战斗护卫)
    float lowestHpPct{ 100.0f };        // 团队最低血线指标
    Unit\\\* mainTank{ nullptr };          // 主坦锚点
    float averageHpPct{ 100.0f };       // 全队平均生命百分比 (严格排除宠物与战斗护卫)
};

void ConsiderAlly(GroupSnapshot\\\& snap, Unit\\\* unit)
{
    if (!unit || !unit->IsAlive() || !unit->IsInWorld()) return;
    if (unit->GetMap() != me->GetMap() || !unit->IsFriendlyTo(me)) return;
    
    // 排除图腾与伴随型战斗护卫（Combat Guardian）：护卫血量为投影值且享 90% AoE 免伤，
    // 严禁进入治疗打地鼠雷达，杜绝治疗机器人把核心 GCD 浪费在护卫身上导致主坦断疗。
    if (unit->GetTypeId() == TYPEID\\\_UNIT)
    {
        Creature\\\* creature = unit->ToCreature();
        if (creature->IsTotem() || creature->GetScriptName() == "BotGuardianAI")
            return;
    }

    if (!me->IsWithinDist(unit, HEAL\\\_RANGE) || !me->IsWithinLOSInMap(unit)) return;
    if (std::find(snap.allies.begin(), snap.allies.end(), unit) != snap.allies.end()) return;

    snap.allies.push\\\_back(unit);

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
```

\---

## 5\. 当前底层 APL 开发进展总结 (31/31 专精全量闭环)\[cite: 40]

底层（专家动作优先级列表 APL Baseline）已达成 **31 专精全量闭环交付**（通过编译及深度逻辑审核，实现零侵入、高实战性能）\[cite: 40]：

### 坦克集群 (4 席 - 全部封存)\[cite: 40]

* **防护战 (`bot\\\_protection\\\_warrior`)：** 怒气平滑注入、盾猛/复仇核心仇恨链、雷霆/冲击波群拉、盾挡/破釜/盾墙梯级减伤、断筋拦截控场\[cite: 40]。
* **防护骑 (`bot\\\_protection\\\_paladin`)：** 969 技能循环、正义防御/神圣庇护多目标嘲讽路由、神圣之盾常驻维持、炽热防御者免死补偿\[cite: 40]。
* **血DK (`bot\\\_blood\\\_death\\\_knight`)：** 符文符能状态机模拟、冰触爆发仇恨、灵界打击动态已损回血、吸血鬼之血减伤，覆写关闭随从席位\[cite: 36, 40]。
* **熊德 (`bot\\\_bear\\\_druid`)：** 怒气消耗模拟、重殴伪队列机制、横扫/裂伤仇恨构建、狂暴回复与生存本能保命\[cite: 40]。

### 治疗集群 (5 席 - 全部封存)\[cite: 40]

* **神圣骑 (`bot\\\_holy\\\_paladin`)：** 单体道标折射、圣光雕文溅射群抬、圣洁护盾维持、神震跑位急救、秒拔祈求减疗、自律解耦自保梯级\[cite: 40]。
* **戒律牧 (`bot\\\_discipline\\\_priest`)：** 灵魂护体（0CD盾）、狂喜回蓝、争分夺秒、主坦独立维持盾、盾与苦修首跳急救、痛苦压制\[cite: 40]。
* **神圣牧 (`bot\\\_holy\\\_priest`)：** 守护之魂40%受疗免死、雕文6目标环智能群抬、圣光涌动免费瞬发快疗、好运2层大加、联结治疗双向回血\[cite: 40]。
* **恢复萨 (`bot\\\_restoration\\\_shaman`)：** 地盾充能排己维护、水盾常驻、迅捷防吞瞬发大加、激流/潮汐奔涌加速波、4目标治疗链跳跃、解耦三系驱散\[cite: 40]。
* **恢复德 (`bot\\\_restoration\\\_druid`)：** 树形态维持瞬拔解控、回春前置防饥饿、三花滚动濒危让位、迅捷治愈不吞HoT、野性成长群抬、愈合直疗兜底\[cite: 40]。

### 输出集群 (22 席 - 全部封存)\[cite: 40]

* **近战物理/混合 (10 席)：**\[cite: 40]

  * **武器战 (`bot\\\_arms\\\_warrior`)：** 撕裂维持血之气息压制、猝死免费斩杀、致死创伤易伤、碎裂投掷立定破盾、利刃风暴跟随转火\[cite: 40]。
  * **狂暴战 (`bot\\\_fury\\\_warrior`)：** 狂暴姿态、嗜血/旋风斩核心FCFS防斩杀饿死、血涌瞬发猛击、控怒英勇/顺劈队列、激怒急救同帧顺下\[cite: 40]。
  * **惩戒骑 (`bot\\\_retribution\\\_paladin`)：** 阵营圣印自适应（腐蚀/复仇）、自律解耦自保梯级（拯救/圣疗/无敌秒脱）、十字军/神风/驱邪FCFS、防同心圆找背\[cite: 40]。
  * **双持冰DK (`bot\\\_frost\\\_death\\\_knight`)：** 鲜血灵气锁死、萨萨里安双持双段打击、符能模拟补偿、白霜免费吹风与断病死锁破除、杀戮机器暴击冰打，覆写关闭随从席位\[cite: 37, 40]。
  * **邪DK (`bot\\\_unholy\\\_death\\\_knight`)：** 鲜血灵气/骨盾常驻、双疾病传染无损续期、黑色热疫易伤、鲜血打击荒芜增伤、天鬼敌对目标与符能蓄水，覆写常驻天灾军团食尸鬼护卫\[cite: 38, 40]。
  * **刺杀贼 (`bot\\\_assassination\\\_rogue`)：** 潜行锁平砍绞喉起手触发灭绝回能、5层致命+速效毒药模拟、毒伤刷新切割、4\~5星终结技攒能挂起\[cite: 40]。
  * **战斗贼 (`bot\\\_combat\\\_rogue`)：** 切割绝对优先级、杀戮盛宴位移独立独占整帧、冲动/乱舞双爆发防溢能、4\~5星锁死产星挂起、嫁祸诀窍\[cite: 40]。
  * **敏锐贼 (`bot\\\_subtlety\\\_rogue`)：** 8s影舞伏击控能挂起防偷跑出血、预谋+暗步背身秒接刺骨、暗影杀手能耗对齐、尊严每秒被动回星、伺机待发双通道重置\[cite: 40]。
  * **增强萨 (`bot\\\_enhancement\\\_shaman`)：** 漩涡武器5层瞬发闪电（多目标闪电链/单体闪电箭平滑降级）、平砍贴身叠层模拟、风暴打击易伤、火图腾落点追踪与新星几何爆破\[cite: 40]。
  * **猫德 (`bot\\\_feral\\\_cat`)：** 背后找背撕碎、斜掠/割裂流血维持、野性咆哮增伤、清晰预兆触发直放撕碎、终结技攒能挂起\[cite: 40]。
* **远程物理 (3 席)：**\[cite: 40]

  * **射击猎 (`bot\\\_marksmanship\\\_hunter`)：** 8/15码盲区迟滞模型、自动射击通道维持、蝰蛇回蓝状态机、奇美拉刷新毒蛇钉刺、准备就绪防吞急速、假死1.5s超时逃逸，常驻座狼机制护卫\[cite: 25, 39, 40]。
  * **生存猎 (`bot\\\_survival\\\_hunter`)：** 荷枪实弹防吞跳剪切与触发式直放、满阶爆炸射击、黑箭增伤、狙击训练站桩维持、陷阱发射器与毒蛇钉刺全分阶查验，常驻座狼机制护卫\[cite: 25, 39, 40]。
  * **兽王猎 (`bot\\\_beast\\\_mastery\\\_hunter`)：** 野兽之心红人爆发解耦本体、长寿动态42s胁迫昏迷打断实体直放、仔细瞄准满阶攻强转化、稳固雕文增伤联动，常驻座狼机制护卫\[cite: 25, 39, 40]。
* **法系远程 (9 席)：**\[cite: 40]

  * **元素萨 (`bot\\\_elemental\\\_shaman`)：** 烈焰震击DoT维持、熔岩爆发必暴与瞬发熔岩奔腾消费、闪电箭填充、天怒图腾全团法强法暴增益、雷霆风暴回蓝\[cite: 40]。
  * **鸟德 (`bot\\\_balance\\\_druid`)：** 日月蚀双相记忆状态机（SEEKING\_SOLAR/LUNAR）、满阶日月蚀/自然之赐/大地月亮注入、星辰坠落非通道GCD闭环、台风朝向校正\[cite: 40]。
  * **暗牧 (`bot\\\_shadow\\\_priest`)：** 24码射程收敛防发呆死锁、触/疫病/痛三DoT滚动、精神鞭笞100%自动续痛、触断档自适应破锁重起、脱战瞬拔形态补增益、消散/暗影魔续航\[cite: 40]。
  * **奥法 (`bot\\\_arcane\\\_mage`)：** 4层奥冲蓝耗风控双相仲裁（Conserve控蓝/Burn泄蓝）、飞弹速射瞬发消费、气定神闲防吞飞弹、冰箱1.5s/隐形3s超时脱困、欺凌弱小减速维持\[cite: 40]。
  * **火法 (`bot\\\_fire\\\_mage`)：** 活动炸弹末跳防剪切（<=0才补）、法术连击瞬发炎爆triggered直放与手动剥除、燃烧Off-GCD当帧顺下、0\~35码火球术无盲区读条、龙息术朝向校正\[cite: 40]。
  * **冰法 (`bot\\\_frost\\\_mage`)：** 深度冻结triggered直放穿透Boss免疫、寒冰指原地扣层保留原生时长、思维冻结瞬发霜火之箭直放、急速冷却冰脉防开局秒吞、全域冻结判定\[cite: 40]。
  * **痛苦术 (`bot\\\_affliction\\\_warlock`)：** 四重DoT滚动预读（鬼影/无常/腐蚀/痛苦诅咒）、吸取灵魂长引导断档自适应破锁、永恒痛苦首发必挂、生命分流triggered直放绕开内存越界，覆写常驻地狱猎犬护卫\[cite: 29, 39, 40]。
  * **恶魔术 (`bot\\\_demonology\\\_warlock`)：** 恶魔变形近身献祭光环与雕文36s手动延长、熔火之心烧尽手动剥层、灭杀35%极速魂火倾泻、生命分流雕文被动ID分离与直放、末日灾祸等级自适应与防顶，覆写常驻恶魔卫士护卫\[cite: 27, 39, 40]。
  * **毁灭术 (`bot\\\_destruction\\\_warlock`)：** 献祭核心前置绝对第一优先级、燃烧瞬发核爆、爆燃3层充能读条手动剥除、混乱之箭穿透爆击与雕文动态CD、硫磺与烈火/灾祸满阶常量修正、暗影灼烧跑位补刀、64级烧尽平滑回退暗影箭，覆写常驻小鬼远程护卫\[cite: 28, 39, 40]。

\---

## 6\. 轻量化伴随型战斗护卫体系 (Combat Guardian System - 全量闭环交付)

### 核心设计哲学：外观千人千面，机制内核永远毕业

针对 3.3.5a 中带宠职业（猎人、术士、死亡骑士），彻底废弃重量级原生 `Player Pet` 架构（杜绝宠物存盘、天赋树、快乐度及技能栏的维护负担），采用轻量化挂载实体（统一 Creature Entry `990001`，挂载脚本 `BotGuardianAI`）\[cite: 25]。表现层以外观池随机摇号呈现多样性，机制内核严格锚定该职业/专精的副本毕业形态\[cite: 25]。

### 专精与随从机制矩阵

|宿主职业 / 专精|随从常驻|表现层外观池 (Skinning)|机制内核 (Graduation Core)|战斗行为模式|
|-|-|-|-|-|
|**猎人**（全专精通用）\[cite: 39]|**是**\[cite: 39]|狼/恐狼/猫科/熊/猪/迅猛龙/龟/蜘蛛等随机 Roll 点\[cite: 25]|**座狼**：全阶梯【狂怒之嚎】（80 级 +320 AP，40s 循环）+ 90% AoE【回避】\[cite: 25]|近战平砍（`DoMeleeAttackIfReady` 直调）\[cite: 25]|
|**痛苦术** (`Affliction`)\[cite: 29]|**是**\[cite: 39]|地狱猎犬多色随机 Roll 点\[cite: 25]|**地狱猎犬**：全阶梯全队【恶魔智力】（智力/精神）+ 90% AoE【回避】\[cite: 25]|近战平砍（`DoMeleeAttackIfReady` 直调）\[cite: 25]|
|**恶魔术** (`Demonology`)\[cite: 27]|**是**\[cite: 39]|恶魔卫士多模型随机 Roll 点\[cite: 25]|**恶魔卫士**：纯物理近战压制 + 90% AoE【回避】\[cite: 25]|近战平砍（`DoMeleeAttackIfReady` 直调）\[cite: 25]|
|**毁灭术** (`Destruction`)\[cite: 28]|**是**\[cite: 39]|小鬼多色模型（统一 0.5f 缩放）\[cite: 25]|**小鬼**：全阶梯全队【血之契印】（耐力）+ 系统法伤增效光环 `23568`（80 级 +120%）\[cite: 25]|**30 码远程火焰箭**：每帧锁满蓝 `POWER\\\_MANA` 根除 OOM；射程内立定清空追击发生器防断条\[cite: 25]|
|**邪DK** (`Unholy`)\[cite: 38]|**是**\[cite: 38]|经典/高精食尸鬼/天灾潜伏者/地穴恶魔随机 Roll 点\[cite: 25]|**天灾食尸鬼**：纯物理近战 + 90% AoE【回避】\[cite: 25]|近战平砍（`DoMeleeAttackIfReady` 直调）\[cite: 25]|
|**血DK / 冰DK**\[cite: 36, 37]|**否**\[cite: 36, 37]|无|无|不召唤\[cite: 36, 37]|
|**其余各职业专精**\[cite: 39]|**否**\[cite: 39]|无|无|不召唤\[cite: 39]|

### 关键技术实现要点

1. **生命周期保活闭环：**

   * **出生即随行：** 宿主机器人 `Reset()` 阶段调用 `EnsureGuardianAlive()`，检测到需要护卫且缺失时立即以 `TEMPSUMMON\\\_MANUAL\\\_DESPAWN` 模式唤出，并同帧绑定 `SetOwnerGUID`、`Faction`、`PhaseMask` 后触发随从端 `Reset()`\[cite: 25, 39]。
   * **战后秒补与轮询：** 随从在团本中意外阵亡后，宿主在 `OnCombatEnded()` 脱战瞬间及 `UpdateTimers` 脱战 3 秒轮询中自动补召，无缝重聚\[cite: 39]。
   * **宿主销毁级联：** 随从 `UpdateAI` 校验宿主状态，宿主阵亡、离线、离开世界或跨地图时，随从同帧调用 `me->DespawnOrUnsummon()`，绝不残留孤儿实体\[cite: 25]。
2. **移动与跟随几何控制：**

   * **超距瞬移拉回：** 随从与宿主距离超过 50 码时（骑马飞奔、掉落、传送门），立即执行 `NearTeleportTo` 瞬移至宿主身旁并重新挂载伴随站位，前置于技能结算，保证增益释放 100% 覆盖宿主\[cite: 25]。
   * **行军零引怪：** 覆写 `MoveInLineOfSight` 直接返回，彻底拔除原生野怪视线警戒，行军路过红名绝不自主开怪\[cite: 25]。
3. **战斗状态机与转火自愈：**

   * **转火严格同步：** 随从不设自主仇恨列表，唯一攻击目标严格锚定宿主当前目标（`owner->GetVictim()`）\[cite: 25]。宿主转火瞬间随从同帧切目标；多怪连战期间小怪倒地随从仅停手（`AttackStop`）而不清空进战状态（不清 `CombatStop`），保证连战平滑突进\[cite: 25]。
   * **受控位移自愈：** 近战随从目标未变但追击链因 Boss 击飞/眩晕/恐惧打断时，自动检测非 `CHASE\\\_MOTION\\\_TYPE` 并补发 `MoveChase`，彻底根除原地发呆\[cite: 25]。
4. **小鬼远程纯法系控制轨：**

   * **远程目标锁定：** 执行 `me->Attack(masterTarget, false)` 仅建立法术目标锚定，绝不开启近战挥砍\[cite: 25]。
   * **追击发生器清空：** 贴近至 30 码内刹车前，必须显式调用 `me->GetMotionMaster()->Clear()` 清空 `CHASE\\\_MOTION\\\_TYPE`，避免追击生成器在读条中下发微移打断火焰箭\[cite: 25]。
   * **零耗蓝与等级查表：** 每帧施法前将 `POWER\\\_MANA` 置满；火焰箭采用专用查表函数 `GetImpFireboltSpellId(level)` 映射 1\~80 级各阶 DBC ID\[cite: 25]。

\---

## 7\. 下一阶段：中层归因分析器与顶层记忆库联调\[cite: 40]

底层 APL Baseline 31 专精与轻量战斗护卫体系全面封存后，开发核心重心正式切换至**中层（Post-Combat Attribution）与顶层（SQLite Memory）体系构建**\[cite: 40]：

```text
                                  ┌───────────────────────────────┐
                                  │   SQLite 机制记忆数据库						  │
                                  │  (adaptive\\\_boss\\\_memory.db)					  │
                                  └──────────────┬────────────────┘
                                                 				  ▲
                                   持久化沉淀			   │   动态下发覆盖
                                                 				  ▼
┌───────────────────────────────┐ 		    归因分析流      ┌───────────────────────────────┐
│ 战斗回溯与归因分析器							├─────────────►│ 专家动作优先级列表 (APL)					        │
│ (Post-Combat Attribution)     				   │			                   │ (31 专精 Baseline + 护卫体系) 			      │
└───────────────────────────────┘              			   └───────────────────────────────┘
```

### 一、 中层：战斗回溯与归因分析器开发规划\[cite: 40]

1. **触发挂钩与零运行时开销契约：**\[cite: 40]

   * 仅在战斗终结瞬间挂钩触发：团灭脱战（`CreatureAI::EnterEvadeMode`）或 Boss 死亡（`CreatureAI::JustDied`）\[cite: 39, 40]。
   * 严禁在战斗 `UpdateAI` 循环中进行重度统计，数据采样仅依靠轻量环形缓冲区（Ring Buffer）沉淀最后 30 秒事件\[cite: 40]。
2. **三核心归因引擎：**\[cite: 40]

   * **致死伤害归因（Lethality Attribution）：** 分析团灭前承伤峰值与致死 `SpellID`，提取伤害源坐标生成危险区域标记（维度 B 走位自适应前置）\[cite: 40]。
   * **漏断与灭团技归因（Interrupt Audit）：** 统计 Boss 致命施法期间随从的打断技能就绪状态，标记灭团技能并计算压秒容差（维度 A 80% 压秒打断前置）\[cite: 40]。
   * **仇恨速率测定（Threat Ramp Metering）：** 采样玩家坦克开局前 10 秒 TPS 曲线，测定随从爆发推迟量 $\\Delta t$（0\~10s），回写至输出专精爆发门禁（维度 C 仇恨协同前置）\[cite: 40]。

### 二、 顶层：持久化机制记忆库 (SQLite Schema)\[cite: 40]

* **核心数据表规划：**\[cite: 40]

  * `boss\\\_threat\\\_profiles`：记录主坦针对特定 Boss 的 TPS 速率与推荐爆发延迟\[cite: 40]；
  * `boss\\\_lethal\\\_spells`：记录 Boss 灭团级技能 ID、施法时间、安全规避半径\[cite: 40]；
  * `boss\\\_interrupt\\\_priorities`：记录高优先级打断法术列表与最优压秒阈值（默认 0.80）\[cite: 40]。

