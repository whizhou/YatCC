"""
LLVM IR 程序特征提取模块

从 LLVM IR 文本中提取程序的关键特征，用于：
  1. 帮助 LLM 理解程序结构，做出更精准的优化决策
  2. 追踪优化前后的特征变化，评估优化效果
"""

import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional


@dataclass
class IRFeatures:
    """LLVM IR 程序特征集合"""

    # ── 基础统计 ──
    num_functions: int = 0
    num_basic_blocks: int = 0
    num_instructions: int = 0
    num_phi_nodes: int = 0
    num_arguments: int = 0

    # ── 指令类别分布 ──
    num_loads: int = 0
    num_stores: int = 0
    num_alloca: int = 0
    num_branches: int = 0
    num_calls: int = 0
    num_returns: int = 0
    num_binary_ops: int = 0
    num_comparisons: int = 0
    num_casts: int = 0
    num_gep: int = 0
    num_select: int = 0

    # ── 二元运算细分 ──
    num_add: int = 0
    num_sub: int = 0
    num_mul: int = 0
    num_div: int = 0  # udiv + sdiv
    num_rem: int = 0  # urem + srem
    num_shift: int = 0  # shl + lshr + ashr
    num_bitwise: int = 0  # and + or + xor

    # ── 结构特征 ──
    max_loop_depth: int = 0
    num_loops: int = 0
    avg_block_size: float = 0.0
    max_block_size: int = 0
    num_global_variables: int = 0
    num_constant_globals: int = 0  # 无 store 的全局变量（可传播）

    # ── 调用图特征 ──
    num_unique_callees: int = 0
    max_call_depth: int = 0
    num_recursive_functions: int = 0

    # ── 内存特征 ──
    memory_op_ratio: float = 0.0  # (load + store + alloca) / total_instructions
    alloca_ratio: float = 0.0  # alloca / total_instructions

    # ── 代码体积指标 ──
    ir_size_bytes: int = 0  # IR 文本大小

    # ── 函数级信息 ──
    function_names: List[str] = field(default_factory=list)
    function_sizes: Dict[str, int] = field(default_factory=dict)

    def summary(self) -> str:
        """生成结构化特征摘要，供 LLM 阅读"""
        lines = [
            "=== 程序特征摘要 ===",
            "",
            "【基础统计】",
            f"  函数数量: {self.num_functions}",
            f"  基本块数量: {self.num_basic_blocks}",
            f"  指令总数: {self.num_instructions}",
            f"  PHI 节点: {self.num_phi_nodes}",
            f"  IR 大小: {self.ir_size_bytes} 字节",
            "",
            "【指令分布】",
            f"  内存操作: load={self.num_loads}, store={self.num_stores}, alloca={self.num_alloca}",
            f"  控制流: branch={self.num_branches}, call={self.num_calls}, ret={self.num_returns}",
            f"  算术运算: add={self.num_add}, sub={self.num_sub}, mul={self.num_mul}, "
            f"div={self.num_div}, rem={self.num_rem}",
            f"  位运算: shift={self.num_shift}, bitwise={self.num_bitwise}",
            f"  比较: {self.num_comparisons}, 转换: {self.num_casts}, GEP: {self.num_gep}",
            "",
            "【结构特征】",
            f"  循环数量: {self.num_loops}",
            f"  最大循环嵌套深度: {self.max_loop_depth}",
            f"  平均基本块大小: {self.avg_block_size:.1f} 条指令",
            f"  最大基本块大小: {self.max_block_size} 条指令",
            f"  全局变量: {self.num_global_variables} (常量: {self.num_constant_globals})",
            "",
            "【调用图】",
            f"  唯一被调函数: {self.num_unique_callees}",
            f"  递归函数: {self.num_recursive_functions}",
            "",
            "【内存特征】",
            f"  内存操作占比: {self.memory_op_ratio:.1%}",
            f"  alloca 占比: {self.alloca_ratio:.1%}",
            "",
            "【函数列表】",
        ]
        for name, size in sorted(
            self.function_sizes.items(), key=lambda x: -x[1]
        )[:20]:
            lines.append(f"  {name}: {size} 条指令")

        if len(self.function_sizes) > 20:
            lines.append(f"  ... (共 {len(self.function_sizes)} 个函数)")

        return "\n".join(lines)


def extract_features(ir_text: str) -> IRFeatures:
    """从 LLVM IR 文本中提取程序特征

    Args:
        ir_text: LLVM IR 文本内容

    Returns:
        IRFeatures 实例
    """
    feat = IRFeatures()
    feat.ir_size_bytes = len(ir_text.encode("utf-8"))

    lines = ir_text.splitlines()

    # ── 全局变量分析 ──
    global_store_targets: set = set()
    for line in lines:
        stripped = line.strip()
        # 全局变量定义
        if re.match(r"^@\w+\s*=", stripped):
            feat.num_global_variables += 1

    # ── 函数级分析 ──
    current_func = None
    current_func_name = None
    current_block_size = 0
    in_function = False
    block_sizes: List[int] = []
    func_body_lines: List[str] = []

    for line in lines:
        stripped = line.strip()

        # 函数定义开始
        func_match = re.match(
            r"^define\s+.*@(\w+)\s*\(", stripped
        )
        if func_match:
            in_function = True
            current_func_name = func_match.group(1)
            feat.num_functions += 1
            feat.function_names.append(current_func_name)
            current_block_size = 0
            func_body_lines = []
            continue

        # 函数结束
        if in_function and stripped == "}":
            if current_func_name and current_block_size > 0:
                block_sizes.append(current_block_size)
            if current_func_name:
                total = sum(
                    1
                    for l in func_body_lines
                    if l.strip()
                    and not l.strip().startswith(";")
                    and not re.match(r"^\w+:", l.strip())
                    and not l.strip().startswith("define")
                    and l.strip() != "}"
                )
                feat.function_sizes[current_func_name] = total
            in_function = False
            current_func_name = None
            continue

        if not in_function:
            continue

        func_body_lines.append(line)

        # 基本块标签
        if re.match(r"^[\w.]+:", stripped) or re.match(r"^[\w.]+:\s*;", stripped):
            if current_block_size > 0:
                block_sizes.append(current_block_size)
            current_block_size = 0
            feat.num_basic_blocks += 1
            continue

        # 跳过空行和注释
        if not stripped or stripped.startswith(";"):
            continue

        # 指令分析
        feat.num_instructions += 1
        current_block_size += 1

        # PHI 节点
        if re.match(r"%\w+\s*=\s*phi\s", stripped):
            feat.num_phi_nodes += 1

        # 内存操作
        if " load " in stripped or stripped.startswith("load "):
            feat.num_loads += 1
        if " store " in stripped or stripped.startswith("store "):
            feat.num_stores += 1
            # 跟踪 store 目标
            store_match = re.search(r"store\s+\w+.*?,\s*\w+\s+(@\w+)", stripped)
            if store_match:
                global_store_targets.add(store_match.group(1))
        if " alloca " in stripped:
            feat.num_alloca += 1

        # 控制流
        if stripped.startswith("br ") or stripped.startswith("switch "):
            feat.num_branches += 1
        if " call " in stripped:
            feat.num_calls += 1
            callee_match = re.search(r"call\s+\w+\s+@(\w+)", stripped)
            if callee_match:
                pass  # 统计在下面
        if stripped.startswith("ret "):
            feat.num_returns += 1

        # 二元运算
        if re.search(r"=\s*add\s", stripped):
            feat.num_add += 1
            feat.num_binary_ops += 1
        elif re.search(r"=\s*sub\s", stripped):
            feat.num_sub += 1
            feat.num_binary_ops += 1
        elif re.search(r"=\s*mul\s", stripped):
            feat.num_mul += 1
            feat.num_binary_ops += 1
        elif re.search(r"=\s*[us]div\s", stripped):
            feat.num_div += 1
            feat.num_binary_ops += 1
        elif re.search(r"=\s*[us]rem\s", stripped):
            feat.num_rem += 1
            feat.num_binary_ops += 1
        elif re.search(r"=\s*(shl|lshr|ashr)\s", stripped):
            feat.num_shift += 1
            feat.num_binary_ops += 1
        elif re.search(r"=\s*(and|or|xor)\s", stripped):
            feat.num_bitwise += 1
            feat.num_binary_ops += 1

        # 比较
        if re.search(r"=\s*icmp\s", stripped):
            feat.num_comparisons += 1

        # 类型转换
        if re.search(
            r"=\s*(trunc|zext|sext|fptoui|fptosi|uitofp|sitofp|ptrtoint|inttoptr|bitcast|addrspacecast)\s",
            stripped,
        ):
            feat.num_casts += 1

        # GEP
        if re.search(r"=\s*getelementptr\s", stripped):
            feat.num_gep += 1

        # Select
        if re.search(r"=\s*select\s", stripped):
            feat.num_select += 1

    # 最后一个基本块
    if current_block_size > 0:
        block_sizes.append(current_block_size)

    # ── 计算汇总指标 ──
    if block_sizes:
        feat.avg_block_size = sum(block_sizes) / len(block_sizes)
        feat.max_block_size = max(block_sizes)

    total_instr = max(feat.num_instructions, 1)
    feat.memory_op_ratio = (
        feat.num_loads + feat.num_stores + feat.num_alloca
    ) / total_instr
    feat.alloca_ratio = feat.num_alloca / total_instr

    # ── 调用图特征 ──
    callees: set = set()
    for line in lines:
        call_match = re.search(r"call\s+\w+\s+@(\w+)", line)
        if call_match:
            callees.add(call_match.group(1))
    feat.num_unique_callees = len(callees)

    # ── 循环检测（简化：通过 backedge 检测） ──
    # 更精确的检测需要支配树分析，这里用启发式
    loop_indicators = 0
    for line in lines:
        # br i1 %cond, label %loop_header, label %exit
        # 简单启发：phi 节点 + 向后跳转通常意味着循环
        if re.search(r"=\s*phi\s", line):
            loop_indicators += 1
    feat.num_loops = max(0, loop_indicators // 2)  # 粗略估计

    # 循环深度估计（通过缩进层级等启发式）
    feat.max_loop_depth = min(feat.num_loops, 3)  # 粗略上界

    return feat


def compare_features(before: IRFeatures, after: IRFeatures) -> str:
    """对比优化前后的特征变化

    Returns:
        人类可读的变化摘要
    """
    lines = ["=== 优化效果对比 ===", ""]

    def delta(name: str, b: int, a: int) -> str:
        diff = a - b
        pct = (diff / b * 100) if b else 0
        sign = "+" if diff > 0 else ""
        return f"  {name}: {b} → {a} ({sign}{diff}, {sign}{pct:.1f}%)"

    lines.append(delta("指令总数", before.num_instructions, after.num_instructions))
    lines.append(delta("基本块", before.num_basic_blocks, after.num_basic_blocks))
    lines.append(delta("load", before.num_loads, after.num_loads))
    lines.append(delta("store", before.num_stores, after.num_stores))
    lines.append(delta("alloca", before.num_alloca, after.num_alloca))
    lines.append(delta("call", before.num_calls, after.num_calls))
    lines.append(delta("PHI 节点", before.num_phi_nodes, after.num_phi_nodes))
    lines.append(delta("IR 大小(bytes)", before.ir_size_bytes, after.ir_size_bytes))

    return "\n".join(lines)
