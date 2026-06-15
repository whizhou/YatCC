"""
优化 Pass 元数据与知识库

定义每个可用 Pass 的名称、功能描述、代码体积影响和依赖关系。
供 LLM Agent 在决策时参考。

命名约定：Pass 类名与 C++ 实现一一对应。
"""

from dataclasses import dataclass, field
from enum import Enum
from typing import Dict, List, Optional, Set


class SizeImpact(Enum):
    """Pass 对代码体积的影响方向"""
    REDUCE = "reduce"           # 直接减少代码体积
    NEUTRAL = "neutral"         # 中性（不增不减，但可能为后续铺路）
    MAY_INCREASE = "may_increase"  # 可能增大代码（如内联、展开）
    ANALYSIS = "analysis"       # 分析 Pass，不修改 IR


@dataclass
class PassInfo:
    """单个优化 Pass 的完整元数据"""
    name: str                           # C++ 类名
    description: str                    # 功能描述
    size_impact: SizeImpact             # 对代码体积的影响
    size_impact_detail: str             # 影响的具体说明
    prerequisites: List[str] = field(default_factory=list)  # 前置 Pass
    enables: List[str] = field(default_factory=list)        # 为哪些 Pass 铺路
    best_for: List[str] = field(default_factory=list)       # 最适合的程序特征
    can_repeat: bool = False            # 是否可以重复运行
    constructor_needs_out: bool = True  # 构造函数是否需要 raw_ostream 参数


# ──────────────────────────────────────────
# Pass 知识库定义
# ──────────────────────────────────────────

PASSES: Dict[str, PassInfo] = {
    "ConstantPropagation": PassInfo(
        name="ConstantPropagation",
        description=(
            "常量传播与常量折叠合并 Pass。循环执行两个阶段直到收敛：\n"
            "1) 全局常量传播：将无 store 写入的全局变量的 load 替换为初始值\n"
            "2) 常量折叠：对两个常量操作数的二元运算(Add/Sub/Mul/UDiv/SDiv/"
            "And/Or/Xor/Shl/LShr/AShr/SRem/URem)和ICmp直接计算结果"
        ),
        size_impact=SizeImpact.REDUCE,
        size_impact_detail=(
            "消除全局变量的 load 指令；折叠算术运算为常量；"
            "为 DCE 创造更多死代码消除机会"
        ),
        prerequisites=[],
        enables=["AlgebraicIdentity", "DeadCodeElimination"],
        best_for=["大量全局常量", "常量运算链", "简化后暴露死代码"],
        can_repeat=True,
    ),

    "ConstantFolding": PassInfo(
        name="ConstantFolding",
        description=(
            "纯常量折叠 Pass。仅对两个操作数均为 ConstantInt 的二元运算"
            "直接计算结果并替换。支持 Add/Sub/Mul/UDiv/SDiv。"
            "注意：ConstantPropagation 已包含此功能，通常不需要单独运行。"
        ),
        size_impact=SizeImpact.REDUCE,
        size_impact_detail="直接消除算术指令，但功能被 ConstantPropagation 覆盖",
        enables=["DeadCodeElimination"],
        best_for=["大量常量运算"],
    ),

    "AlgebraicIdentity": PassInfo(
        name="AlgebraicIdentity",
        description=(
            "代数恒等式优化 Pass，通过数学规则消除无意义运算。支持 8 大类优化：\n"
            "1) 算术恒等式: add(x,0)→x, sub(x,0)→x, mul(x,1)→x, sdiv(x,1)→x\n"
            "2) 零化规则: mul(x,0)→0, and(x,0)→0\n"
            "3) 幂等规则: and(x,x)→x, or(x,x)→x, sub(x,x)→0, xor(x,x)→0\n"
            "4) 负数规则: mul(x,-1)→neg(x), sdiv(x,-1)→neg(x)\n"
            "5) 幂运算规则: mul(x,2^n)→shl(x,n), sdiv(x,2^n)→带偏置ashr\n"
            "6) 取模规则: urem(x,1)→0, urem(x,2^n)→and(x,2^n-1)\n"
            "7) 常量链合并: add(add(x,C1),C2)→add(x,C1+C2)\n"
            "8) 常量折叠: 两个常量操作数直接计算"
        ),
        size_impact=SizeImpact.REDUCE,
        size_impact_detail=(
            "每条规则都直接减少指令数。特别是乘除变移位(mul→shl)和"
            "取模变位与(urem→and)可大幅简化运算。"
            "sdiv(x,2^n)使用带偏置算术右移处理负数舍入。"
        ),
        prerequisites=["ConstantPropagation"],
        enables=["DeadCodeElimination"],
        best_for=["大量算术运算", "乘除法", "取模运算", "常量运算链"],
        can_repeat=True,
    ),

    "FunctionInlining": PassInfo(
        name="FunctionInlining",
        description=(
            "函数内联 Pass。将非递归函数内联到调用处。\n"
            "算法：\n"
            "1) 构建函数调用图，DFS 检测环（递归）\n"
            "2) 不在环中、非 main、非声明的函数可内联\n"
            "3) 内联过程：参数映射→克隆基本块→更新分支→分割调用块→"
            "创建合并块→处理返回值(PHI合并多返回点)→设置控制流\n"
            "4) 反复内联直到收敛，删除无调用者的函数"
        ),
        size_impact=SizeImpact.MAY_INCREASE,
        size_impact_detail=(
            "内联增大代码（复制函数体），但消除调用指令(call/ret)。"
            "关键收益：为后续 CSE/常量传播/DCE 提供更大优化窗口；"
            "被内联的函数可能被完全消除（dead function elimination）。"
            "对于小型被频繁调用的函数，总体积可能减少。"
        ),
        prerequisites=["ConstantPropagation"],
        enables=["AllocaHoisting", "Mem2Reg", "CSE"],
        best_for=["大量小型函数调用", "函数体简单", "调用频繁"],
    ),

    "AllocaHoisting": PassInfo(
        name="AllocaHoisting",
        description=(
            "alloca 提升 Pass。将非入口基本块中的 alloca 指令移动到函数入口块开头。\n"
            "解决 FunctionInlining 后 alloca 位于循环体内导致栈溢出的问题。"
        ),
        size_impact=SizeImpact.NEUTRAL,
        size_impact_detail="不增减指令数，但为 Mem2Reg 提供正确基础",
        prerequisites=["FunctionInlining"],
        enables=["Mem2Reg"],
        best_for=["内联后 alloca 在循环体内"],
    ),

    "Mem2Reg": PassInfo(
        name="Mem2Reg",
        description=(
            "内存到寄存器提升 Pass（SSA 构造）。将 alloca/store/load 模式"
            "转换为 SSA 寄存器和 PHI 节点。\n"
            "算法：\n"
            "1) 检查 alloca 是否可提升（仅 load/store 访问）\n"
            "2) 快速路径：单 store 用支配树替换；单块用线性扫描\n"
            "3) 通用路径：计算迭代支配边界(IDF)→插入PHI→RenamePass沿支配树重命名\n"
            "4) 简化全同输入的 PHI 节点"
        ),
        size_impact=SizeImpact.REDUCE,
        size_impact_detail=(
            "消除 alloca/store/load 三件套，替换为直接寄存器使用。"
            "大量消除内存访问指令，同时暴露死代码和死存储。"
        ),
        prerequisites=["AllocaHoisting"],
        enables=["DeadStoreElimination", "DeadCodeElimination", "LICM", "ConstantPropagation"],
        best_for=["内存密集型程序", "大量 alloca/store/load"],
    ),

    "DeadStoreElimination": PassInfo(
        name="DeadStoreElimination",
        description=(
            "死存储消除 Pass，两阶段：\n"
            "1) Alloca 死存储：若 alloca 从未被 load，删除所有对其的 store\n"
            "2) 基本块内 DSE：反向遍历，跟踪被后续 store 覆盖的内存位置，"
            "若 store 的目标已被覆盖则为死存储"
        ),
        size_impact=SizeImpact.REDUCE,
        size_impact_detail="直接消除死 store 指令。Mem2Reg 后大量 store 变为死存储。",
        prerequisites=["Mem2Reg"],
        enables=["DeadCodeElimination"],
        best_for=["Mem2Reg 后清理", "冗余存储"],
        can_repeat=True,
    ),

    "DeadCodeElimination": PassInfo(
        name="DeadCodeElimination",
        description=(
            "死代码消除 Pass（Worklist 算法）。删除无使用者且无副作用的指令。\n"
            "副作用检查：终止指令/fence/volatile有副作用；"
            "store 仅当目标全局无 load 时无副作用；"
            "call 检查 doesNotAccessMemory 或 onlyReadsMemory 属性。\n"
            "删除后递归检查操作数是否变为死代码。"
        ),
        size_impact=SizeImpact.REDUCE,
        size_impact_detail="消除所有无用指令。核心清理 Pass，在其他优化创造死代码后运行。",
        prerequisites=["Mem2Reg", "LICM"],
        enables=[],
        best_for=["清理阶段", "优化后死代码"],
        can_repeat=True,
    ),

    "LICM": PassInfo(
        name="LICM",
        description=(
            "循环不变量代码外提 Pass。将循环中不随迭代变化的指令提升到 preheader。\n"
            "算法：\n"
            "1) 后序遍历循环树（内层优先）\n"
            "2) 不变性检查：所有操作数在循环外定义；"
            "load 的底层对象在循环内无 store 修改；call 必须纯函数\n"
            "3) 安全性：可投机执行或支配所有循环出口\n"
            "4) 迭代直到收敛"
        ),
        size_impact=SizeImpact.NEUTRAL,
        size_impact_detail=(
            "指令从循环体移到 preheader，不增减总指令数。"
            "间接正面：为循环内 DCE 创造机会；"
            "被提升的指令可能被后续常量传播折叠。"
        ),
        prerequisites=["Mem2Reg"],
        enables=["ConstantPropagation", "DeadCodeElimination"],
        best_for=["复杂循环结构", "循环内有不变计算", "循环内有纯函数调用"],
        can_repeat=True,
    ),

    "InstructionCombining": PassInfo(
        name="InstructionCombining",
        description=(
            "指令合并 Pass。利用算术代数规则简化指令：\n"
            "1) 恒等消除: x+0→x, x*1→x, x&-1→x, x|0→x, x^0→x\n"
            "2) 零化: x*0→0, x&0→0\n"
            "3) 常量链合并: add(add(x,C1),C2)→add(x,C1+C2)\n"
            "4) 自身消除: x-x→0, x^x→0\n"
            "Worklist 驱动，新指令加入 worklist 继续优化（传递闭包）"
        ),
        size_impact=SizeImpact.REDUCE,
        size_impact_detail="直接减少指令数。常量链合并可大幅减少连续运算。",
        prerequisites=["AlgebraicIdentity"],
        enables=["StrengthReduction", "DeadCodeElimination"],
        best_for=["大量算术运算", "常量运算链"],
    ),

    "StrengthReduction": PassInfo(
        name="StrengthReduction",
        description=(
            "强度削弱 Pass。将高计算复杂度指令转化为低复杂度指令：\n"
            "1) mul x, 2^n → shl x, n\n"
            "2) mul x, C → shift+add/sub 组合（找最长连续1-bit区间用减法优化）\n"
            "3) udiv x, 2^n → lshr x, n\n"
            "4) sdiv x, 2^n → 带偏置的 ashr\n"
            "5) urem x, 2^n → and x, (2^n-1)\n"
            "6) srem x, 2^n → 取绝对值+and+条件取负"
        ),
        size_impact=SizeImpact.MAY_INCREASE,
        size_impact_detail=(
            "单条 mul/div 替换为多条 shift/add，但每条更简单。"
            "间接正面：为 InstructionCombining 创造合并机会。"
        ),
        prerequisites=["InstructionCombining"],
        enables=["ConstantPropagation", "DeadCodeElimination"],
        best_for=["大量乘除法运算", "常量乘数"],
    ),

    "LoopUnroll": PassInfo(
        name="LoopUnroll",
        description=(
            "循环展开 Pass。将常量迭代次数的小循环完全展开。\n"
            "通过 SCEV (ScalarEvolution) 分析获取常量回边次数，"
            "完全展开阈值为 MAX_FULL_UNROLL_THRESHOLD=16。\n"
            "克隆循环体 N 次，更新 PHI 入边，重连控制流。"
        ),
        size_impact=SizeImpact.MAY_INCREASE,
        size_impact_detail=(
            "直接增大代码（N 倍循环体）。"
            "间接正面：展开后 CSE/常量传播可消除大量冗余。"
            "仅在迭代次数≤16时展开。"
        ),
        prerequisites=["LICM", "ConstantPropagation"],
        enables=["CommonSubexpressionElimination"],
        best_for=["小循环（迭代次数≤16）", "循环体简单", "循环后有 CSE 机会"],
    ),

    "CommonSubexpressionElimination": PassInfo(
        name="CommonSubexpressionElimination",
        description=(
            "公共子表达式消除 Pass（滑动窗口 CSE）。\n"
            "在基本块内维护滑动窗口（默认256条指令），"
            "对纯计算指令（30+种操作码）计算哈希，"
            "若窗口内找到 isIdenticalTo 匹配的指令则替换。"
        ),
        size_impact=SizeImpact.REDUCE,
        size_impact_detail="消除重复计算指令。LoopUnroll/FunctionInlining 后产生大量重复指令。",
        prerequisites=["LoopUnroll"],
        enables=["DeadCodeElimination"],
        best_for=["循环展开后", "函数内联后", "重复计算"],
    ),

    "StaticCallCounterPrinter": PassInfo(
        name="StaticCallCounterPrinter",
        description="静态调用计数打印器。统计每个函数被直接调用的次数。分析/报告 Pass，不修改 IR。",
        size_impact=SizeImpact.ANALYSIS,
        size_impact_detail="不修改 IR，仅输出分析结果",
        prerequisites=[],
        enables=[],
        best_for=["调试分析"],
    ),
}


# ──────────────────────────────────────────
# 辅助函数
# ──────────────────────────────────────────

def get_all_pass_names() -> List[str]:
    """获取所有可用 Pass 的类名列表"""
    return list(PASSES.keys())


def get_optimization_passes() -> List[str]:
    """获取所有优化 Pass（排除分析 Pass）"""
    return [
        name for name, info in PASSES.items()
        if info.size_impact != SizeImpact.ANALYSIS
    ]


def get_pass_info(name: str) -> Optional[PassInfo]:
    """获取指定 Pass 的元数据"""
    return PASSES.get(name)


def format_pass_descriptions_for_llm() -> str:
    """生成供 LLM 阅读的 Pass 描述文本"""
    lines = []
    for name, info in PASSES.items():
        if info.size_impact == SizeImpact.ANALYSIS:
            continue
        lines.append(f"### {name}")
        lines.append(f"功能: {info.description}")
        lines.append(f"体积影响: [{info.size_impact.value}] {info.size_impact_detail}")
        if info.prerequisites:
            lines.append(f"前置依赖: {', '.join(info.prerequisites)}")
        if info.enables:
            lines.append(f"可为以下 Pass 铺路: {', '.join(info.enables)}")
        if info.best_for:
            lines.append(f"适用场景: {', '.join(info.best_for)}")
        lines.append(f"可重复运行: {'是' if info.can_repeat else '否'}")
        lines.append("")

    return "\n".join(lines)


def validate_sequence(sequence: List[str]) -> List[str]:
    """验证并过滤 Pass 序列，返回有效的 Pass 名称列表"""
    valid = []
    for name in sequence:
        name = name.strip()
        if name in PASSES:
            valid.append(name)
        else:
            # 尝试模糊匹配
            matched = False
            for pass_name in PASSES:
                if name.lower() == pass_name.lower():
                    valid.append(pass_name)
                    matched = True
                    break
            if not matched:
                # 尝试部分匹配
                for pass_name in PASSES:
                    if name.lower() in pass_name.lower():
                        valid.append(pass_name)
                        matched = True
                        break
    return valid
