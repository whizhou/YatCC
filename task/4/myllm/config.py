"""
配置管理模块

管理 LLM API 密钥、路径、优化参数等配置项。
支持从 .env 文件、环境变量或构造函数参数加载。
"""

import os
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Config:
    """Agent 优化系统配置"""

    # ── LLM 配置 ──
    api_key: str = "sk-8096edb78ae04c34a9cb043f4795e45c"
    base_url: str = "https://api.deepseek.com"
    model: str = "deepseek-v4-flash"
    temperature: float = 0.0
    max_tokens: int = 8192

    # ── 路径配置 ──
    # task4-llm 二进制（动态 Pass 执行器）
    llm_binary: str = ""
    # task4-classic 二进制（经典固定管线，用于 baseline）
    classic_binary: str = ""
    # 输入 IR 文件路径
    input_ir: str = ""
    # 输出 IR 文件路径
    output_ir: str = ""

    # ── 优化参数 ──
    max_iterations: int = 5
    # 体积改进阈值（百分比），低于此值视为收敛
    convergence_threshold: float = 0.5
    # 是否允许多轮迭代
    enable_iteration: bool = True
    # 历史记录大小（用于 Agent 上下文）
    history_size: int = 3

    @classmethod
    def from_env(cls, env_file: str = "") -> "Config":
        """从 .env 文件和环境变量加载配置"""
        if env_file and os.path.exists(env_file):
            _load_dotenv(env_file)

        return cls(
            api_key=os.environ.get("DEEPSEEK_API_KEY", ""),
            base_url=os.environ.get("DEEPSEEK_BASE_URL", "https://api.deepseek.com"),
            model=os.environ.get("DEEPSEEK_MODEL", "deepseek-chat"),
            temperature=float(os.environ.get("AGENT_TEMPERATURE", "0.0")),
            max_tokens=int(os.environ.get("AGENT_MAX_TOKENS", "8192")),
            llm_binary=os.environ.get("TASK4_LLM_BIN", ""),
            classic_binary=os.environ.get("TASK4_CLASSIC_BIN", ""),
            input_ir=os.environ.get("INPUT_IR", ""),
            output_ir=os.environ.get("OUTPUT_IR", ""),
            max_iterations=int(os.environ.get("AGENT_MAX_ITERATIONS", "5")),
            convergence_threshold=float(
                os.environ.get("AGENT_CONVERGENCE_THRESHOLD", "0.5")
            ),
        )

    def validate(self) -> None:
        """验证必要配置项"""
        if not self.api_key:
            raise ValueError(
                "缺少 API 密钥，请设置 DEEPSEEK_API_KEY 环境变量或在配置中指定"
            )
        if not self.llm_binary or not os.path.isfile(self.llm_binary):
            raise FileNotFoundError(
                f"task4-llm 二进制不存在: {self.llm_binary}\n"
                "请先编译: cmake --build build --target task4-llm"
            )
        if not self.input_ir or not os.path.isfile(self.input_ir):
            raise FileNotFoundError(f"输入 IR 文件不存在: {self.input_ir}")


def _load_dotenv(path: str) -> None:
    """简易 .env 文件解析器（避免外部依赖）"""
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            key, _, value = line.partition("=")
            key = key.strip()
            value = value.strip().strip("\"'")
            if key and key not in os.environ:
                os.environ[key] = value
