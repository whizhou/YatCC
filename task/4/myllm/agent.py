"""
Agent 核心逻辑

实现基于 LLM 的自适应代码体积优化 Agent。
支持多轮迭代优化，每轮根据程序特征和优化历史选择 Pass 序列。

流程：
  1. 提取输入 IR 的程序特征
  2. 构建 LLM prompt（含特征、Pass 知识、优化历史）
  3. LLM 预测最优 Pass 序列
  4. 调用 task4-agent 执行优化
  5. 测量输出体积，记录结果
  6. 若未收敛且未达最大轮次，回到步骤 2（带反馈）
  7. 输出最优结果
"""

import json
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .config import Config
from .features import IRFeatures, compare_features, extract_features
from .llm_client import LLMClient, extract_json_from_response, remove_think_tags
from .optimizer import OptResult, Optimizer
from .passes import (
    PASSES,
    PassInfo,
    SizeImpact,
    format_pass_descriptions_for_llm,
    validate_sequence,
)


@dataclass
class IterationRecord:
    """单轮迭代的记录"""
    iteration: int
    pass_sequence: List[str]
    result: OptResult
    llm_reasoning: str = ""


@dataclass
class OptimizationResult:
    """最终优化结果"""
    best_sequence: List[str]
    best_ir: str
    best_size_bytes: int
    best_instruction_count: int
    baseline_size_bytes: int
    baseline_instruction_count: int
    size_reduction_pct: float
    instruction_reduction_pct: float
    iterations: List[IterationRecord]
    input_features: IRFeatures
    output_features: IRFeatures


class Agent:
    """基于 LLM 的自适应代码体积优化 Agent

    核心职责：
    1. 分析程序特征，选择最优 Pass 序列
    2. 执行优化并测量体积
    3. 多轮迭代，根据反馈调整策略
    4. 选择全局最优结果
    """

    def __init__(self, config: Config):
        self._config = config
        self._llm = LLMClient(
            api_key=config.api_key,
            base_url=config.base_url,
            model=config.model,
            temperature=config.temperature,
            max_tokens=config.max_tokens,
        )
        self._optimizer = Optimizer(config.llm_binary)

        # 加载 prompt 模板
        prompt_dir = os.path.join(os.path.dirname(__file__), "prompts")
        self._system_prompt_template = Path(
            os.path.join(prompt_dir, "system.txt")
        ).read_text(encoding="utf-8")
        self._user_prompt_template = Path(
            os.path.join(prompt_dir, "user.txt")
        ).read_text(encoding="utf-8")
        self._iterative_prompt_template = Path(
            os.path.join(prompt_dir, "iterative.txt")
        ).read_text(encoding="utf-8")

    def optimize(self, input_ir_path: str, output_ir_path: str) -> OptimizationResult:
        """执行完整的优化流程

        Args:
            input_ir_path: 输入 IR 文件路径
            output_ir_path: 输出最优 IR 文件路径

        Returns:
            OptimizationResult 实例
        """
        # ── 步骤 1: 提取输入 IR 特征 ──
        print("=" * 60)
        print("  Agent 代码体积优化")
        print("=" * 60)
        print()

        print("[1/5] 提取程序特征...")
        input_ir = Path(input_ir_path).read_text(encoding="utf-8")
        input_features = extract_features(input_ir)
        print(f"  函数: {input_features.num_functions}, "
              f"指令: {input_features.num_instructions}, "
              f"IR 大小: {input_features.ir_size_bytes} 字节")
        print()

        # ── 步骤 2: 运行经典管线作为 baseline ──
        print("[2/5] 运行经典管线 (baseline)...")
        baseline_result = self._run_baseline(input_ir_path)
        baseline_size = baseline_result.output_size_bytes if baseline_result.success else input_features.ir_size_bytes
        baseline_instr = baseline_result.instruction_count if baseline_result.success else input_features.num_instructions
        if baseline_result.success:
            print(f"  Baseline: {baseline_instr} 条指令, {baseline_size} 字节")
        else:
            print(f"  Baseline 失败: {baseline_result.error_message}")
            print("  使用原始 IR 大小作为 baseline")
        print()

        # ── 步骤 3: 迭代优化 ──
        print(f"[3/5] 开始迭代优化 (最多 {self._config.max_iterations} 轮)...")
        iterations: List[IterationRecord] = []
        best_size = baseline_size
        best_instructions = baseline_instr
        best_sequence: List[str] = []
        best_ir = ""

        for i in range(self._config.max_iterations):
            print(f"\n  ── 第 {i+1} 轮 ──")

            # 3a: 构建 prompt
            if i == 0:
                prompt = self._build_initial_prompt(input_ir, input_features)
            else:
                prompt = self._build_iterative_prompt(
                    input_features, iterations, best_sequence,
                    best_instructions, best_size
                )

            # 3b: 调用 LLM
            print(f"  调用 LLM ({self._config.model})...")
            llm_response = self._llm.single_chat(
                system_prompt=self._system_prompt_template,
                user_prompt=prompt,
                handlers=[remove_think_tags],
            )

            # 3c: 解析响应
            parsed = self._parse_llm_response(llm_response)
            sequence = parsed.get("sequence", [])
            reasoning = parsed.get("reasoning", parsed.get("analysis", ""))

            # 验证 Pass 名称
            sequence = validate_sequence(sequence)
            if not sequence:
                print(f"  ⚠ LLM 返回的序列无效，跳过本轮")
                iterations.append(IterationRecord(
                    iteration=i + 1,
                    pass_sequence=[],
                    result=OptResult(
                        success=False, output_ir="", output_size_bytes=0,
                        instruction_count=0, features=None,
                        error_message="LLM 返回无效序列",
                    ),
                    llm_reasoning=reasoning,
                ))
                continue

            print(f"  Pass 序列: {' → '.join(sequence)}")

            # 3d: 执行优化
            print(f"  执行优化...")
            result = self._optimizer.run(input_ir_path, sequence)

            if not result.success:
                print(f"  ⚠ 优化失败: {result.error_message}")
                iterations.append(IterationRecord(
                    iteration=i + 1,
                    pass_sequence=sequence,
                    result=result,
                    llm_reasoning=reasoning,
                ))
                continue

            print(f"  结果: {result.instruction_count} 条指令, "
                  f"{result.output_size_bytes} 字节")

            # 3e: 记录结果
            iterations.append(IterationRecord(
                iteration=i + 1,
                pass_sequence=sequence,
                result=result,
                llm_reasoning=reasoning,
            ))

            # 3f: 更新最优
            if result.output_size_bytes < best_size:
                improvement = best_size - result.output_size_bytes
                pct = improvement / best_size * 100 if best_size else 0
                print(f"  ✓ 新最优! 减少 {improvement} 字节 ({pct:.1f}%)")
                best_size = result.output_size_bytes
                best_instructions = result.instruction_count
                best_sequence = sequence
                best_ir = result.output_ir
            else:
                print(f"  ✗ 未超越当前最优 ({best_size} 字节)")

            # 3g: 检查收敛
            if len(iterations) >= 2:
                prev = iterations[-2]
                if prev.result.success and result.success:
                    size_diff = abs(prev.result.output_size_bytes - result.output_size_bytes)
                    if size_diff / max(prev.result.output_size_bytes, 1) * 100 < self._config.convergence_threshold:
                        print(f"\n  收敛：连续两轮差异 < {self._config.convergence_threshold}%")
                        break

        print()

        # ── 步骤 4: 输出最优结果 ──
        print("[4/5] 保存最优结果...")
        if best_ir:
            Path(output_ir_path).write_text(best_ir, encoding="utf-8")
            print(f"  输出文件: {output_ir_path}")
        else:
            print("  ⚠ Agent 未找到更优结果，保存 baseline 输出")
            if baseline_result.success and baseline_result.output_ir:
                Path(output_ir_path).write_text(baseline_result.output_ir, encoding="utf-8")
            else:
                Path(output_ir_path).write_text(input_ir, encoding="utf-8")
            best_ir = input_ir
            best_sequence = []
        print()

        # ── 步骤 5: 汇总 ──
        output_features = extract_features(best_ir) if best_ir else input_features
        size_reduction = (
            (baseline_size - best_size) / baseline_size * 100
            if baseline_size else 0
        )
        instr_reduction = (
            (baseline_instr - best_instructions) / baseline_instr * 100
            if baseline_instr else 0
        )

        print("[5/5] 优化汇总")
        print(f"  Baseline: {baseline_instr} 条指令, {baseline_size} 字节")
        print(f"  最优结果: {best_instructions} 条指令, {best_size} 字节")
        print(f"  指令减少: {instr_reduction:.1f}%")
        print(f"  体积减少: {size_reduction:.1f}%")
        if best_sequence:
            print(f"  最优序列: {' → '.join(best_sequence)}")
        print(f"  总迭代次数: {len(iterations)}")
        print("=" * 60)

        return OptimizationResult(
            best_sequence=best_sequence,
            best_ir=best_ir,
            best_size_bytes=best_size,
            best_instruction_count=best_instructions,
            baseline_size_bytes=baseline_size,
            baseline_instruction_count=baseline_instr,
            size_reduction_pct=size_reduction,
            instruction_reduction_pct=instr_reduction,
            iterations=iterations,
            input_features=input_features,
            output_features=output_features,
        )

    # ──────────────────────────────────────
    # 内部方法
    # ──────────────────────────────────────

    def _run_baseline(self, input_ir_path: str) -> OptResult:
        """运行经典固定管线作为 baseline"""
        classic_bin = self._config.classic_binary
        if not classic_bin or not os.path.isfile(classic_bin):
            return OptResult(
                success=False, output_ir="", output_size_bytes=0,
                instruction_count=0, features=None,
                error_message="task4-classic 二进制不存在",
            )
        return self._optimizer.run_classic(classic_bin, input_ir_path)

    def _build_initial_prompt(self, ir_text: str, features: IRFeatures) -> str:
        """构建第一轮的 LLM prompt"""
        passes_desc = format_pass_descriptions_for_llm()
        system = self._system_prompt_template.replace("{passes_description}", passes_desc)

        snippet = ir_text[:3000]
        if len(ir_text) > 3000:
            snippet += "\n... (IR 已截断)"

        user = self._user_prompt_template.format(
            features_summary=features.summary(),
            ir_snippet=snippet,
        )

        return user

    def _build_iterative_prompt(
        self,
        features: IRFeatures,
        history: List[IterationRecord],
        best_sequence: List[str],
        best_instructions: int,
        best_size: int,
    ) -> str:
        """构建迭代轮次的 LLM prompt"""
        # 构建历史摘要
        history_lines = []
        for rec in history[-self._config.history_size:]:
            status = "✓ 成功" if rec.result.success else "✗ 失败"
            size = rec.result.output_size_bytes if rec.result.success else "N/A"
            instr = rec.result.instruction_count if rec.result.success else "N/A"
            seq_str = " → ".join(rec.pass_sequence) if rec.pass_sequence else "无效序列"
            history_lines.append(
                f"  第{rec.iteration}轮: [{status}] 序列={seq_str}, "
                f"指令={instr}, 大小={size}"
            )
            if rec.result.error_message:
                history_lines.append(f"    错误: {rec.result.error_message}")

        history_text = "\n".join(history_lines)

        # 构建反馈分析
        feedback_lines = []
        successful = [r for r in history if r.result.success]
        failed = [r for r in history if not r.result.success]

        if successful:
            sizes = [r.result.output_size_bytes for r in successful]
            min_idx = sizes.index(min(sizes))
            max_idx = sizes.index(max(sizes))
            feedback_lines.append(
                f"  最小体积: 第{successful[min_idx].iteration}轮 = {min(sizes)} 字节"
            )
            feedback_lines.append(
                f"  最大体积: 第{successful[max_idx].iteration}轮 = {max(sizes)} 字节"
            )
            feedback_lines.append(
                f"  体积差异: {max(sizes) - min(sizes)} 字节"
            )

        if failed:
            feedback_lines.append(
                f"  失败轮次: {len(failed)}/{len(history)}，"
                "请避免类似的无效序列"
            )

        # 分析哪些 Pass 可能有帮助
        if successful:
            used_passes = set()
            for r in successful:
                used_passes.update(r.pass_sequence or [])
            unused = set(PASSES.keys()) - used_passes - {"StaticCallCounterPrinter"}
            if unused:
                feedback_lines.append(
                    f"  尚未尝试的 Pass: {', '.join(sorted(unused))}"
                )

        feedback_text = "\n".join(feedback_lines) if feedback_lines else "  无特殊反馈"

        return self._iterative_prompt_template.format(
            features_summary=features.summary(),
            history=history_text,
            best_sequence=" → ".join(best_sequence) if best_sequence else "无",
            best_instructions=best_instructions,
            best_size=best_size,
            feedback=feedback_text,
        )

    def _parse_llm_response(self, response: str) -> dict:
        """解析 LLM 响应中的 JSON"""
        # 应用 handler 链
        response = remove_think_tags(response)

        # 提取 JSON
        extract_json = extract_json_from_response("")
        parsed = extract_json(response)

        if isinstance(parsed, dict) and "sequence" in parsed:
            # 确保 sequence 是列表
            seq = parsed["sequence"]
            if isinstance(seq, str):
                parsed["sequence"] = [s.strip() for s in seq.split(",") if s.strip()]
            return parsed

        # 回退：尝试从纯文本中提取序列
        if isinstance(parsed, dict) and "error" in parsed:
            # 尝试逗号分隔的 Pass 名称
            for line in response.splitlines():
                line = line.strip()
                passes = [p.strip() for p in line.split(",") if p.strip()]
                valid = validate_sequence(passes)
                if len(valid) >= 2:
                    return {"sequence": valid, "reasoning": response}

        return parsed if isinstance(parsed, dict) else {"sequence": [], "reasoning": response}
