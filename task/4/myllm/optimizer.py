"""
编译器调用与代码体积度量模块

负责调用 task4-agent 二进制执行优化，并测量输出 IR 的代码体积。
"""

import os
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

from .features import IRFeatures, extract_features


@dataclass
class OptResult:
    """单次优化的结果"""
    success: bool
    output_ir: str              # 输出 IR 文本
    output_size_bytes: int      # 输出 IR 文件大小（字节）
    instruction_count: int      # 指令总数
    features: Optional[IRFeatures]  # 输出 IR 的特征
    error_message: str = ""     # 错误信息
    pass_sequence: Optional[List[str]] = None  # 执行的 Pass 序列


class Optimizer:
    """优化器接口

    调用 task4-llm 二进制，传入 Pass 序列文件，执行优化并测量体积。
    """

    def __init__(self, llm_binary: str):
        """
        Args:
            llm_binary: task4-llm 二进制路径
        """
        if not os.path.isfile(llm_binary):
            raise FileNotFoundError(f"task4-llm 二进制不存在: {llm_binary}")
        self._binary = os.path.abspath(llm_binary)
        self._tmpdir = tempfile.mkdtemp(prefix="myllm_opt_")

    def run(
        self,
        input_ir_path: str,
        pass_sequence: List[str],
        output_ir_path: Optional[str] = None,
    ) -> OptResult:
        """执行优化

        Args:
            input_ir_path: 输入 IR 文件路径
            pass_sequence: Pass 序列（类名列表）
            output_ir_path: 输出 IR 文件路径（None 则自动生成临时文件）

        Returns:
            OptResult 实例
        """
        if not os.path.isfile(input_ir_path):
            return OptResult(
                success=False,
                output_ir="",
                output_size_bytes=0,
                instruction_count=0,
                features=None,
                error_message=f"输入 IR 文件不存在: {input_ir_path}",
            )

        # 写入 Pass 序列文件
        passes_file = os.path.join(self._tmpdir, "passes.txt")
        with open(passes_file, "w", encoding="utf-8") as f:
            f.write(",".join(pass_sequence))

        # 生成输出路径
        if output_ir_path is None:
            output_ir_path = os.path.join(self._tmpdir, "output.ll")

        # 执行优化
        try:
            result = subprocess.run(
                [self._binary, input_ir_path, output_ir_path, passes_file],
                capture_output=True,
                text=True,
                timeout=120,
            )
        except subprocess.TimeoutExpired:
            return OptResult(
                success=False,
                output_ir="",
                output_size_bytes=0,
                instruction_count=0,
                features=None,
                error_message="优化超时（120秒）",
                pass_sequence=pass_sequence,
            )
        except Exception as e:
            return OptResult(
                success=False,
                output_ir="",
                output_size_bytes=0,
                instruction_count=0,
                features=None,
                error_message=str(e),
                pass_sequence=pass_sequence,
            )

        if result.returncode != 0:
            return OptResult(
                success=False,
                output_ir="",
                output_size_bytes=0,
                instruction_count=0,
                features=None,
                error_message=f"优化失败 (exit={result.returncode}): {result.stderr}",
                pass_sequence=pass_sequence,
            )

        # 读取输出
        try:
            output_ir = Path(output_ir_path).read_text(encoding="utf-8")
            output_size = len(output_ir.encode("utf-8"))
            features = extract_features(output_ir)
        except Exception as e:
            return OptResult(
                success=False,
                output_ir="",
                output_size_bytes=0,
                instruction_count=0,
                features=None,
                error_message=f"读取输出失败: {e}",
                pass_sequence=pass_sequence,
            )

        return OptResult(
            success=True,
            output_ir=output_ir,
            output_size_bytes=output_size,
            instruction_count=features.num_instructions,
            features=features,
            pass_sequence=pass_sequence,
        )

    def run_classic(
        self,
        classic_binary: str,
        input_ir_path: str,
        output_ir_path: Optional[str] = None,
    ) -> OptResult:
        """运行经典固定管线（作为 baseline 对比）

        Args:
            classic_binary: task4-classic 二进制路径
            input_ir_path: 输入 IR 文件路径
            output_ir_path: 输出 IR 文件路径

        Returns:
            OptResult 实例
        """
        if not os.path.isfile(classic_binary):
            return OptResult(
                success=False,
                output_ir="",
                output_size_bytes=0,
                instruction_count=0,
                features=None,
                error_message=f"task4-classic 二进制不存在: {classic_binary}",
            )

        if output_ir_path is None:
            output_ir_path = os.path.join(self._tmpdir, "classic_output.ll")

        try:
            result = subprocess.run(
                [os.path.abspath(classic_binary), input_ir_path, output_ir_path],
                capture_output=True,
                text=True,
                timeout=120,
            )
        except Exception as e:
            return OptResult(
                success=False,
                output_ir="",
                output_size_bytes=0,
                instruction_count=0,
                features=None,
                error_message=str(e),
            )

        if result.returncode != 0:
            return OptResult(
                success=False,
                output_ir="",
                output_size_bytes=0,
                instruction_count=0,
                features=None,
                error_message=f"经典优化失败: {result.stderr}",
            )

        try:
            output_ir = Path(output_ir_path).read_text(encoding="utf-8")
            output_size = len(output_ir.encode("utf-8"))
            features = extract_features(output_ir)
        except Exception as e:
            return OptResult(
                success=False,
                output_ir="",
                output_size_bytes=0,
                instruction_count=0,
                features=None,
                error_message=f"读取输出失败: {e}",
            )

        return OptResult(
            success=True,
            output_ir=output_ir,
            output_size_bytes=output_size,
            instruction_count=features.num_instructions,
            features=features,
        )

    def measure_ir_size(self, ir_path: str) -> Tuple[int, int]:
        """度量 IR 文件的体积指标

        Returns:
            (文件字节数, 指令总数)
        """
        ir_text = Path(ir_path).read_text(encoding="utf-8")
        features = extract_features(ir_text)
        return len(ir_text.encode("utf-8")), features.num_instructions

    def cleanup(self) -> None:
        """清理临时文件"""
        import shutil
        shutil.rmtree(self._tmpdir, ignore_errors=True)
