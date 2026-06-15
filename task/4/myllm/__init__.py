"""
myllm - 基于 LLM Agent 的自适应代码体积优化系统

该模块利用大语言模型分析 LLVM IR 程序特征，动态生成最优的优化 Pass 组合，
实现比固定管线更优的代码体积优化效果。

核心组件:
    - features:   LLVM IR 程序特征提取
    - passes:     优化 Pass 元数据与知识库
    - llm_client: OpenAI 兼容的 LLM 客户端
    - optimizer:  编译器调用与体积度量
    - agent:      Agent 优化决策核心
"""

__version__ = "0.1.0"
