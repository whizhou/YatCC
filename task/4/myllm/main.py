"""
myllm CLI 入口

用法:
    python -m myllm [options]
    python myllm/main.py [options]

示例:
    python -m myllm --input input.ll --output output.ll --env .env
    python -m myllm -i input.ll -o output.ll --max-iter 10
"""

import argparse
import sys
import os
from pathlib import Path


def parse_args() -> argparse.Namespace:
    """解析命令行参数"""
    parser = argparse.ArgumentParser(
        description="基于 LLM Agent 的自适应代码体积优化",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 使用 .env 文件配置
  python -m myllm -i input.ll -o output.ll --env .env

  # 直接指定参数
  python -m myllm -i input.ll -o output.ll \\
      --api-key sk-xxx --model deepseek-chat

  # 使用环境变量
  DEEPSEEK_API_KEY=sk-xxx python -m myllm -i input.ll -o output.ll
        """,
    )

    parser.add_argument(
        "-i", "--input",
        required=True,
        help="输入 LLVM IR 文件路径 (.ll)",
    )
    parser.add_argument(
        "-o", "--output",
        required=True,
        help="输出优化后 IR 文件路径 (.ll)",
    )

    # LLM 配置
    llm_group = parser.add_argument_group("LLM 配置")
    llm_group.add_argument(
        "--env",
        default="",
        help=".env 文件路径（用于加载 API 密钥等配置）",
    )
    llm_group.add_argument(
        "--api-key",
        default="",
        help="DeepSeek API 密钥（或设置 DEEPSEEK_API_KEY 环境变量）",
    )
    llm_group.add_argument(
        "--base-url",
        default="",
        help="API 基础 URL（默认: https://api.deepseek.com）",
    )
    llm_group.add_argument(
        "--model",
        default="",
        help="模型名称（默认: deepseek-chat）",
    )
    llm_group.add_argument(
        "--temperature",
        type=float,
        default=None,
        help="生成温度（默认: 0.0）",
    )
    llm_group.add_argument(
        "--max-tokens",
        type=int,
        default=None,
        help="最大生成 token 数（默认: 8192）",
    )

    # 二进制路径
    bin_group = parser.add_argument_group("二进制路径")
    bin_group.add_argument(
        "--llm-bin",
        default="",
        help="task4-llm 二进制路径（或设置 TASK4_LLM_BIN 环境变量）",
    )
    bin_group.add_argument(
        "--classic-bin",
        default="",
        help="task4-classic 二进制路径（用于 baseline 对比）",
    )

    # 优化参数
    opt_group = parser.add_argument_group("优化参数")
    opt_group.add_argument(
        "--max-iter",
        type=int,
        default=None,
        help="最大迭代轮次（默认: 5）",
    )
    opt_group.add_argument(
        "--convergence",
        type=float,
        default=None,
        help="收敛阈值百分比（默认: 0.5）",
    )

    # 输出控制
    out_group = parser.add_argument_group("输出控制")
    out_group.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="详细输出",
    )
    out_group.add_argument(
        "--report",
        default="",
        help="优化报告输出路径（JSON 格式）",
    )

    return parser.parse_args()


def main() -> int:
    """主入口"""
    args = parse_args()

    # 加载 .env
    if args.env:
        from .config import _load_dotenv
        _load_dotenv(args.env)

    # 构建配置
    from .config import Config
    config = Config.from_env()

    # 命令行参数覆盖环境变量
    if args.api_key:
        config.api_key = args.api_key
    if args.base_url:
        config.base_url = args.base_url
    if args.model:
        config.model = args.model
    if args.temperature is not None:
        config.temperature = args.temperature
    if args.max_tokens is not None:
        config.max_tokens = args.max_tokens
    if args.llm_bin:
        config.llm_binary = args.llm_bin
    if args.classic_bin:
        config.classic_binary = args.classic_bin
    if args.max_iter is not None:
        config.max_iterations = args.max_iter
    if args.convergence is not None:
        config.convergence_threshold = args.convergence

    config.input_ir = args.input
    config.output_ir = args.output

    # 验证配置
    try:
        config.validate()
    except (ValueError, FileNotFoundError) as e:
        print(f"配置错误: {e}", file=sys.stderr)
        return 1

    # 运行 Agent
    from .agent import Agent
    agent = Agent(config)

    try:
        result = agent.optimize(args.input, args.output)
    except KeyboardInterrupt:
        print("\n用户中断", file=sys.stderr)
        return 130
    except Exception as e:
        print(f"优化失败: {e}", file=sys.stderr)
        if args.verbose:
            import traceback
            traceback.print_exc()
        return 1
    finally:
        agent._optimizer.cleanup()

    # 输出报告
    if args.report:
        import json
        report = {
            "input": args.input,
            "output": args.output,
            "best_sequence": result.best_sequence,
            "baseline": {
                "size_bytes": result.baseline_size_bytes,
                "instruction_count": result.baseline_instruction_count,
            },
            "optimized": {
                "size_bytes": result.best_size_bytes,
                "instruction_count": result.best_instruction_count,
            },
            "reduction": {
                "size_pct": round(result.size_reduction_pct, 2),
                "instruction_pct": round(result.instruction_reduction_pct, 2),
            },
            "iterations": [
                {
                    "round": rec.iteration,
                    "sequence": rec.pass_sequence,
                    "success": rec.result.success,
                    "size_bytes": rec.result.output_size_bytes,
                    "instruction_count": rec.result.instruction_count,
                    "error": rec.result.error_message,
                    "reasoning": rec.llm_reasoning[:500],
                }
                for rec in result.iterations
            ],
        }
        Path(args.report).write_text(
            json.dumps(report, indent=2, ensure_ascii=False),
            encoding="utf-8",
        )
        print(f"\n报告已保存: {args.report}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
