# LLVM 优化 Pass 详细文档

本文档描述 Task 4 中实现的所有 LLVM IR 优化 Pass，供 LLM Agent 在决策时参考。

---

## 目录

1. [ConstantPropagation](#1-constantpropagation)
2. [ConstantFolding](#2-constantfolding)
3. [AlgebraicIdentity](#3-algebraicidentity)
4. [FunctionInlining](#4-functioninlining)
5. [AllocaHoisting](#5-allocahoisting)
6. [Mem2Reg](#6-mem2reg)
7. [DeadStoreElimination](#7-deadstoreelimination)
8. [DeadCodeElimination](#8-deadcodeelimination)
9. [LICM](#9-licm)
10. [InstructionCombining](#10-instructioncombining)
11. [StrengthReduction](#11-strengthreduction)
12. [LoopUnroll](#12-loopunroll)
13. [CommonSubexpressionElimination](#13-commonsubexpressionelimination)
14. [StaticCallCounterPrinter](#14-staticcallcounterprinter)
15. [Pass 依赖与交互关系](#15-pass-依赖与交互关系)
16. [代码体积优化策略指南](#16-代码体积优化策略指南)

---

## 1. ConstantPropagation

**类型**: Module Transform Pass  
**构造函数**: `ConstantPropagation(llvm::raw_ostream& out)`

### 功能
将全局常量变量的 `load` 替换为常量值，并循环执行常量折叠直到收敛。合并了常量传播和常量折叠两个阶段。

### 算法细节
1. **全局变量传播**: 遍历所有全局变量，若无 `store` 写入（通过 `hasStoreToGlobal` 检查），将所有 `load` 替换为初始值。
   - 支持简单全局变量：`@g = global i32 42` + `load @g` → `42`
   - 支持常量数组：通过 GEP 链解析 `getelementptr` + `load` 模式
2. **常量折叠**: 对两个操作数均为 `ConstantInt` 的二元运算直接计算结果。
   - 支持 13 种操作码：`Add/Sub/Mul/UDiv/SDiv/And/Or/Xor/Shl/LShr/AShr/SRem/URem`
   - 支持 `ICmp` 比较指令的折叠
3. **收敛循环**: `while(propagated || folded)` 重复执行直到无新变换

### IR 变换示例
```
@g = global i32 42, align 4
; 变换前
%x = load i32, ptr @g
; 变换后
; (load 被替换为常量 42)
```

### 对代码体积的影响
- **正面**: 消除全局变量的 load 指令；折叠后可能暴露死代码
- **间接**: 为 DCE、DSE 创造更多消除机会

### 依赖关系
- **前置**: 无（但运行在 Mem2Reg 之后效果更好）
- **后续**: DCE 可消除因常量替换产生的死代码；AlgebraicIdentity 可进一步简化

---

## 2. ConstantFolding

**类型**: Module Transform Pass  
**构造函数**: `ConstantFolding(llvm::raw_ostream& out)`

### 功能
仅执行常量折叠：将两个操作数均为常量的二元运算替换为常量结果。

### 算法细节
遍历所有 `BinaryOperator`，若左右操作数均为 `ConstantInt`，直接计算结果并替换。支持 `Add/Sub/Mul/UDiv/SDiv`。

### IR 变换示例
```
; 变换前
%r = add i32 3, 5
; 变换后
; (%r 被替换为常量 8)
```

### 对代码体积的影响
- **正面**: 直接消除算术指令
- **注意**: ConstantPropagation 已包含此功能，通常不需要单独运行

---

## 3. AlgebraicIdentity

**类型**: Module Transform Pass  
**构造函数**: `AlgebraicIdentity(llvm::raw_ostream& out)`

### 功能
通过数学代数规则消除无意义的运算，是最全面的代数简化 Pass。

### 支持的 8 大类优化

| 类别 | 规则 | 变换 |
|------|------|------|
| 算术恒等式 | `x+0→x, x-0→x, x*1→x, x/1→x` | 消除无用运算 |
| 零化规则 | `x*0→0, x&0→0` | 产生常量 |
| 幂等规则 | `x&x→x, x\|x→x, x-x→0, x^x→0` | 消除冗余 |
| 负数规则 | `x*(-1)→-x, sdiv(x,-1)→-x` | 简化运算 |
| 幂运算规则 | `x*2^n→shl(x,n), sdiv(x,2^n)→ashr(x+偏置,n)` | 乘除变移位 |
| 取模规则 | `urem(x,1)→0, urem(x,2^n)→and(x,2^n-1)` | 取模变位与 |
| 常量链合并 | `add(add(x,C1),C2)→add(x,C1+C2)` | 合并连续常量 |
| 常量折叠 | 两个常量操作数直接计算 | 消除运算 |

### 对代码体积的影响
- **显著正面**: 每条规则都直接减少指令数
- **关键**: `sdiv(x,2^n)` 使用带偏置的算术右移（处理负数舍入），比原始除法指令更短

### 依赖关系
- **前置**: ConstantPropagation（传播常量后更易触发规则）
- **后续**: DCE 消除因简化产生的死代码

---

## 4. FunctionInlining

**类型**: Module Transform Pass  
**构造函数**: `FunctionInlining(llvm::raw_ostream& out)`

### 功能
将非递归函数内联到调用处，消除函数调用开销，扩大后续优化的作用范围。

### 算法细节
1. `buildCallGraph`: 构建函数调用图
2. `hasCycleDFS`: 三色标记 DFS 检测调用图中的环（递归）
3. `canInline`: 不在环中、非 main、非声明的函数可内联
4. `inlineCallSite`: 8 步内联过程
   - 参数映射 → 克隆基本块 → 更新分支目标 → 分割调用块
   - 创建合并块 → 处理返回值（PHI 合并多返回点）
   - 插入克隆块 → 设置控制流
5. 剥离 `noinline` 和 `optnone` 属性
6. 反复内联直到收敛；内联后删除无调用者的函数

### IR 变换示例
```
; 变换前
define i32 @foo(i32 %x) { %r = add i32 %x, 1  ret i32 %r }
define i32 @bar() { %v = call i32 @foo(i32 42)  ret i32 %v }

; 变换后
define i32 @bar() {
  %r = add i32 42, 1
  ret i32 %r
}
; @foo 被删除（如果无其他调用者）
```

### 对代码体积的影响
- **双向**: 内联增大代码（复制函数体），但消除调用指令
- **间接正面**: 为后续 CSE、常量传播、DCE 提供更大优化窗口
- **关键**: 被内联的函数可能被完全消除（dead function elimination）

### 依赖关系
- **前置**: ConstantPropagation（内联前传播常量可使参数为常量）
- **后续**: AllocaHoisting（内联可能将 alloca 移入循环体）、Mem2Reg

---

## 5. AllocaHoisting

**类型**: Module Transform Pass  
**构造函数**: `AllocaHoisting(llvm::raw_ostream& out)`

### 功能
将非入口基本块中的 `alloca` 指令移动到函数入口块的开头。

### 问题背景
`FunctionInlining` 将含有数组 alloca 的函数内联到循环体后，alloca 位于循环内的非入口块中，每次迭代都会执行，导致栈空间不断增长（栈溢出 SIGSEGV）。

### 算法细节
1. 收集非入口块中的所有 alloca
2. 移动到入口块第一个非 PHI 指令之前
3. 保证每个 alloca 只执行一次（防止栈溢出）

### 对代码体积的影响
- **中性**: 不直接增减指令数
- **必要性**: 为 Mem2Reg 和后续优化提供正确基础

### 依赖关系
- **前置**: FunctionInlining（内联后需要修复 alloca 位置）
- **后续**: Mem2Reg（确保 alloca 在入口块便于分析）

---

## 6. Mem2Reg

**类型**: Module Transform Pass  
**构造函数**: `Mem2Reg()`（无参数）

### 功能
将 `alloca`/`load`/`store` 模式转换为 SSA 寄存器（PHI 节点），消除内存访问开销。这是最核心的优化之一。

### 算法细节
1. `isAllocaPromotable`: 检查 alloca 是否只被 load/store 使用
2. **快速路径 - 单 store**: 若 alloca 只有一个 store，利用支配树直接替换所有被支配的 load
3. **快速路径 - 单块**: 若 alloca 只在一个基本块内使用，线性扫描替换
4. **通用路径**: 使用 `ForwardIDFCalculator` 计算迭代支配边界，在边界处插入 PHI 节点，然后通过 `RenamePass` 沿支配树遍历重命名
5. `simplifyPHINode`: 简化全同输入的 PHI 节点

### IR 变换示例
```
; 变换前
%x = alloca i32
store i32 42, ptr %x
%v = load i32, ptr %x
%r = add i32 %v, 1

; 变换后
; (alloca 和 store/load 被消除)
%r = add i32 42, 1
```

### 对代码体积的影响
- **显著正面**: 消除 alloca/store/load 三件套，替换为直接寄存器使用
- **间接**: 暴露大量死代码（store 被消除后 alloca 也死亡）

### 依赖关系
- **前置**: AllocaHoisting（alloca 需在入口块）
- **后续**: DSE（消除因 SSA 化暴露的死 store）、DCE（消除死 alloca）、ConstantPropagation

---

## 7. DeadStoreElimination

**类型**: Module Transform Pass  
**构造函数**: `DeadStoreElimination(llvm::raw_ostream& out)`

### 功能
删除被后续 store 覆盖或从未被读取的死存储。

### 算法细节（两阶段）
1. **Alloca 死存储消除**: 递归检查 alloca 是否从未被 load（通过 GEP/bitcast 间接追踪），删除所有对无读取 alloca 的 store
2. **基本块内 DSE**: 反向遍历基本块，维护 `killed` 集合（已被后续 store 覆盖的指针），若 store 的目标已被覆盖则为死存储
   - `canonicalPointer`: 剥离指针类型转换用于别名比较

### IR 变换示例
```
; 变换前
store i32 1, ptr %p
store i32 2, ptr %p    ; 覆盖了前一个 store

; 变换后
store i32 2, ptr %p
```

### 对代码体积的影响
- **正面**: 直接消除死 store 指令
- **配合 Mem2Reg**: Mem2Reg 后很多 store 变为死存储

### 依赖关系
- **前置**: Mem2Reg（暴露死 store）
- **后续**: DCE 消除因 store 删除而死的地址计算

---

## 8. DeadCodeElimination

**类型**: Module Transform Pass  
**构造函数**: `DeadCodeElimination(llvm::raw_ostream& out)`

### 功能
删除无使用者且无副作用的指令。

### 算法细节（Worklist 算法）
1. 种子 worklist：所有 `use_empty() && !hasSideEffects` 的指令
2. 副作用检查：
   - 终止指令、fence、volatile 操作 → 有副作用
   - store → 仅当目标全局变量从未被 load 时无副作用
   - call → 检查 `doesNotAccessMemory()` 或 `onlyReadsMemory()` 属性
3. 删除指令后，检查其操作数是否变为死代码，加入 worklist

### IR 变换示例
```
; 变换前
%dead = add i32 %a, %b   ; 无使用者
%used = add i32 %c, %d

; 变换后
%used = add i32 %c, %d
```

### 对代码体积的影响
- **正面**: 消除所有无用指令
- **核心清理 Pass**: 在其他优化创造死代码后运行

### 依赖关系
- **前置**: Mem2Reg、LICM、StrengthReduction 等创造死代码的 Pass
- **后续**: 无（通常是管线末端）

---

## 9. LICM

**类型**: Module Transform Pass  
**构造函数**: `LICM(llvm::raw_ostream& out)`

### 功能
循环不变量代码外提（Loop-Invariant Code Motion），将循环中不随迭代变化的指令提升到 preheader。

### 算法细节
1. **循环遍历**: 后序遍历循环树（内层优先）
2. **不变性检查** (`isLoopInvariant`):
   - 排除 PHI/alloca/terminator/store
   - 所有操作数在循环外定义或已提升
   - load: 底层对象在循环内无 store 修改
   - call: 函数必须纯（`doesNotAccessMemory` 或 `onlyReadsMemory`）
3. **安全性检查** (`safeToHoist`): 可投机执行 或 支配所有循环出口
4. **提升**: 移动到 preheader 末尾
5. **迭代**: 重复直到收敛

### IR 变换示例
```
; 变换前 (循环体内)
loop:
  %v = load i32, ptr @global   ; @global 在循环内无修改
  %r = add i32 %v, %i

; 变换后
preheader:
  %v = load i32, ptr @global
loop:
  %r = add i32 %v, %i
```

### 对代码体积的影响
- **双向**: 循环内的指令移到 preheader，不增减指令数
- **间接正面**: 为循环内 DCE 创造机会；被提升的指令可能被后续常量传播折叠

### 依赖关系
- **前置**: Mem2Reg（SSA 化后更易判断不变性）
- **后续**: ConstantPropagation（提升后可能变为常量）、DCE

---

## 10. InstructionCombining

**类型**: Module Transform Pass  
**构造函数**: `InstructionCombining(llvm::raw_ostream& out)`

### 功能
利用算术代数规则简化指令，合并常量运算链。

### 支持的优化
| 模式 | 规则 |
|------|------|
| 恒等消除 | `x+0→x, x*1→x, x/1→x, x&-1→x, x\|0→x, x^0→x` |
| 零化 | `x*0→0, x&0→0` |
| 常量链合并 | `add(add(x,C1),C2)→add(x,C1+C2)`（Sub, Mul 类似） |
| 自身消除 | `x-x→0, x^x→0` |

### 算法细节
- Worklist 驱动：新指令加入 worklist 继续优化（传递闭包）
- `cleanupDeadInstructions`: 清理因合并产生的死代码

### 对代码体积的影响
- **正面**: 直接减少指令数
- **关键**: 常量链合并可大幅减少连续运算

### 依赖关系
- **前置**: AlgebraicIdentity、StrengthReduction（产生的新指令可被合并）
- **后续**: DCE

---

## 11. StrengthReduction

**类型**: Module Transform Pass  
**构造函数**: `StrengthReduction(llvm::raw_ostream& out)`

### 功能
将高计算复杂度的指令转化为低复杂度的指令。

### 支持的变换
| 原始指令 | 变换结果 |
|----------|----------|
| `mul x, 2^n` | `shl x, n` |
| `mul x, C` | `shift + add/sub` 组合 |
| `udiv x, 2^n` | `lshr x, n` |
| `sdiv x, 2^n` | 带偏置的 `ashr` |
| `urem x, 2^n` | `and x, (2^n-1)` |
| `srem x, 2^n` | 取绝对值 + and + 条件取负 |

### `strengthReduceMul` 详解
将乘以常数 C 分解为 shift+add/sub 组合。找最长连续 1-bit 区间用减法优化（如 `x*7 = (x<<3) - x`），否则逐 bit 分解。

### 对代码体积的影响
- **双向**: 单条 mul/div 替换为多条 shift/add，但每条更简单
- **间接正面**: 为后续 InstructionCombining 创造合并机会

### 依赖关系
- **前置**: InstructionCombining
- **后续**: ConstantPropagation、DCE

---

## 12. LoopUnroll

**类型**: Module Transform Pass  
**构造函数**: `LoopUnroll(llvm::raw_ostream& out)`

### 功能
将常量迭代次数的小循环完全展开，减少循环开销。

### 算法细节
1. `getConstantTripCount`: 通过 SCEV（ScalarEvolution）分析获取常量回边次数
2. `fullyUnrollLoop`: 完全展开
   - 克隆循环体 N 次
   - 更新 PHI 节点入边
   - 重连控制流（latch → 下一个 header）
   - 更新 exit block 的 PHI
   - 更新支配树
3. 阈值: `MAX_FULL_UNROLL_THRESHOLD = 16`（迭代次数 ≤ 16 才展开）
4. 后序遍历循环（内层优先）

### IR 变换示例
```
; 变换前
loop:
  %i = phi i32 [0, %entry], [%i.next, %loop]
  ; body
  %i.next = add i32 %i, 1
  %cond = icmp slt i32 %i.next, 4
  br i1 %cond, label %loop, label %exit

; 变换后 (4 次展开)
; body_0
; body_1
; body_2
; body_3
; (无循环)
```

### 对代码体积的影响
- **负面**: 直接增大代码（N 倍循环体）
- **间接正面**: 展开后 CSE、常量传播可消除大量冗余
- **权衡**: 仅在迭代次数少时使用

### 依赖关系
- **前置**: LICM（先外提不变量减少展开体积）、ConstantPropagation
- **后续**: CSE（消除展开产生的重复指令）

---

## 13. CommonSubexpressionElimination

**类型**: Module Transform Pass  
**构造函数**: `CommonSubexpressionElimination(llvm::raw_ostream& out, unsigned windowSize = 256)`

### 功能
在基本块内，窗口范围内搜索相同指令并替换为已有结果。

### 算法细节
1. 滑动窗口（默认 256 条指令），正向遍历
2. `hashInstruction`: 基于操作码 + 操作数指针计算哈希
3. `isCSECandidate`: 排除终止/副作用/PHI/volatile，只处理纯计算指令（30+ 种操作码）
4. `isIdenticalTo`: 精确比较两条指令
5. 窗口保证候选指令支配当前指令（正向遍历）
6. `cleanupDeadInstructions`: 递归清理死代码

### IR 变换示例
```
; 变换前
%a = add i32 %x, %y
; ... (窗口内)
%b = add i32 %x, %y

; 变换后
%a = add i32 %x, %y
; (%b 的所有使用者被替换为 %a)
```

### 对代码体积的影响
- **正面**: 消除重复计算指令
- **关键场景**: LoopUnroll 展开后产生大量重复指令

### 依赖关系
- **前置**: LoopUnroll（展开产生重复）、FunctionInlining（内联产生重复）
- **后续**: DCE 消除死代码

---

## 14. StaticCallCounterPrinter

**类型**: Module Transform Pass  
**构造函数**: `StaticCallCounterPrinter(llvm::raw_ostream& out)`

### 功能
统计每个函数被直接调用的次数并打印。这是一个分析/报告 Pass，不修改 IR。

### 对代码体积的影响
- **无**: 不修改 IR，仅输出分析结果

---

## 15. Pass 依赖与交互关系

### 推荐管线阶段

```
阶段1: 基础优化
  ConstantPropagation → AlgebraicIdentity
  (传播常量 + 简化代数，暴露更多优化机会)

阶段2: SSA 构造
  FunctionInlining → AllocaHoisting → Mem2Reg
  (内联 → 修复 alloca → 构造 SSA)

阶段3: 清理
  DeadStoreElimination → DeadCodeElimination
  (消除 SSA 化暴露的死代码)

阶段4: 循环优化
  LICM → ConstantPropagation → AlgebraicIdentity
  (外提不变量 → 传播新常量 → 简化)

阶段5: 指令级优化
  InstructionCombining → StrengthReduction → ConstantPropagation
  (合并 → 强度削弱 → 传播)

阶段6: 循环展开 + CSE
  LICM → LoopUnroll → CommonSubexpressionElimination → ConstantPropagation
  (再次外提 → 展开 → 消除重复 → 传播)

阶段7: 最终清理
  AlgebraicIdentity → LICM → DeadStoreElimination → DeadCodeElimination
  (最后的简化和清理)
```

### Pass 间的"铺路"关系（奖励平滑）

| 先行 Pass | 后续 Pass | 铺路机制 |
|-----------|-----------|----------|
| ConstantPropagation | Mem2Reg | 常量传播使 store 的值为常量，Mem2Reg 可直接替换 |
| FunctionInlining | CSE | 内联后同一函数体出现多次，CSE 可消除重复 |
| LoopUnroll | CSE | 展开后循环体重复，CSE 消除公共子表达式 |
| StrengthReduction | InstructionCombining | 强度削弱产生 shift+add，InstCombine 可合并 |
| AlgebraicIdentity | DCE | 代数简化暴露死代码（如 x*0→0 使乘法死亡） |
| LICM | ConstantPropagation | 外提到 preheader 后可能变为常量 |
| Mem2Reg | DSE | SSA 化后大量 store 变为死存储 |
| ConstantPropagation | AlgebraicIdentity | 常量传播后更多代数规则可触发 |

---

## 16. 代码体积优化策略指南

### 核心原则

1. **消除冗余**: DCE、DSE、CSE 直接删除无用/重复指令
2. **简化运算**: AlgebraicIdentity、InstructionCombining 将复杂运算替换为简单运算
3. **消除内存访问**: Mem2Reg 将 alloca/store/load 替换为寄存器
4. **跨上下文优化**: FunctionInlining 消除调用开销，LICM 消除循环冗余
5. **铺路思维**: 某些 Pass 本身不减少代码，但为后续 Pass 创造机会

### 针对不同程序特征的策略

| 程序特征 | 推荐策略 | 关键 Pass |
|----------|----------|-----------|
| 大量全局常量 | 先传播常量再清理 | ConstantPropagation → DCE |
| 深层函数调用 | 内联 + CSE | FunctionInlining → CSE |
| 复杂循环结构 | LICM + 展开 + CSE | LICM → LoopUnroll → CSE |
| 大量算术运算 | 代数简化 + 强度削弱 | AlgebraicIdentity → StrengthReduction |
| 内存密集型 | SSA 化 + 死存储消除 | Mem2Reg → DSE → DCE |
| 小型函数众多 | 内联 + 死函数消除 | FunctionInlining → DCE |
