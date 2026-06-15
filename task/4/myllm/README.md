# myllm - 基于 LLM Agent 的自适应代码体积优化

## 概述

传统编译器的代码体积优化（如 `-Oz`）依赖固定的优化序列，对不同特征的程序适配性有限。
`myllm` 引入大语言模型作为优化策略大脑，根据程序特征动态生成最优的优化 Pass 组合。

### 核心特性

- **程序特征定制化**: 自动提取 LLVM IR 的关键特征（函数数量、指令分布、循环结构、内存操作比例等），定制专属优化策略
- **LLM 驱动的 Pass 选择**: 利用 DeepSeek 等大模型分析程序特征，预测最优的 Pass 组合和执行顺序
- **奖励平滑机制**: Agent 理解 Pass 间的"铺路"关系（如先常量传播再触发死代码消除），避免短视的优化决策
- **多轮迭代优化**: 根据优化结果反馈调整策略，逐步逼近最优解
- **Baseline 对比**: 自动与经典固定管线对比，量化优化增益

## 架构

```
┌─────────────────────────────────────────────────────────┐
│                    Python Agent (myllm/)                  │
│                                                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐ │
│  │ features │  │  passes  │  │   llm    │  │ optimizer│ │
│  │ IR 特征  │  │ Pass 知识│  │ DeepSeek │  │ 编译器   │ │
│  │ 提取     │  │ 库       │  │ 客户端   │  │ 调用     │ │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘ │
│       │              │              │              │       │
│       └──────────────┴──────┬───────┴──────────────┘       │
│                             │                              │
│                      ┌──────┴──────┐                       │
│                      │   agent.py  │                       │
│                      │  核心决策   │                       │
│                      └──────┬──────┘                       │
│                             │                              │
└─────────────────────────────┼──────────────────────────────┘
                              │ 调用
                              ▼
                   ┌─────────────────────┐
                   │    task4-llm (C++)  │
                   │  动态 Pass 执行器   │
                   │  读取 passes.txt    │
                   └─────────────────────┘
```

## 文件结构

```
myllm/
├── __init__.py          # 包初始化
├── __main__.py          # python -m myllm 入口
├── config.py            # 配置管理（API 密钥、路径、参数）
├── features.py          # LLVM IR 程序特征提取
├── passes.py            # 优化 Pass 元数据与知识库
├── llm_client.py        # OpenAI 兼容 LLM 客户端
├── optimizer.py         # 编译器调用与体积度量
├── agent.py             # Agent 核心决策逻辑
├── main.py              # CLI 入口
├── prompts/             # LLM 提示词模板
│   ├── system.txt       # 系统提示词（Pass 知识 + 优化原则）
│   ├── user.txt         # 首轮用户提示词
│   └── iterative.txt    # 迭代轮次用户提示词
└── docs/
    └── passes.md        # Pass 详细文档（供参考）
```

## 使用方法

### 前置条件

1. 编译 task4-llm 二进制：
   ```bash
   cd /path/to/YatCC
   cmake -B build -G Ninja -DLLVM_INSTALL_DIR=$YatCC_LLVM_DIR
   cmake --build build --target task4-llm
   ```

2. 安装 Python 依赖：
   ```bash
   pip install openai
   ```

3. 配置环境变量（复制 `.env.example` 为 `.env`）：
   ```bash
   cp .env.example .env
   # 编辑 .env 填入 API 密钥
   ```

### 运行优化

```bash
# 基本用法
python -m myllm -i input.ll -o output.ll --env .env

# 指定参数
python -m myllm -i input.ll -o output.ll \
    --api-key sk-xxx \
    --model deepseek-chat \
    --llm-bin ./build/task/4/task4-llm \
    --classic-bin ./build/task/4/task4-classic \
    --max-iter 10 \
    --report report.json

# 使用环境变量
export DEEPSEEK_API_KEY=your_key_here
export TASK4_LLM_BIN=./build/task/4/task4-llm
python -m myllm -i input.ll -o output.ll
```

### 参数说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-i, --input` | 输入 IR 文件 | 必填 |
| `-o, --output` | 输出 IR 文件 | 必填 |
| `--env` | .env 文件路径 | - |
| `--api-key` | API 密钥 | 环境变量 |
| `--base-url` | API URL | https://api.deepseek.com |
| `--model` | 模型名称 | deepseek-chat |
| `--llm-bin` | task4-llm 路径 | 环境变量 |
| `--classic-bin` | task4-classic 路径 | 环境变量 |
| `--max-iter` | 最大迭代轮次 | 5 |
| `--convergence` | 收敛阈值(%) | 0.5 |
| `--report` | 报告输出路径 | - |

## 优化流程

1. **特征提取**: 分析输入 IR 的函数数量、指令分布、循环结构、内存操作比例等
2. **Baseline 建立**: 运行经典固定管线作为对比基准
3. **LLM 决策**: 将程序特征和 Pass 知识发送给 LLM，获取推荐的 Pass 序列
4. **执行优化**: 调用 task4-llm 执行 LLM 推荐的 Pass 序列
5. **效果评估**: 测量输出 IR 的代码体积，与历史最优对比
6. **迭代改进**: 将结果反馈给 LLM，调整策略（重复 3-5）
7. **输出最优**: 选择体积最小的结果作为最终输出

## task4-llm C++ 二进制

`task4-agent.cpp` 编译为 `task4-llm` 目标，是一个独立的 C++ 程序。

### 用法
```bash
task4-llm <input.ll> <output.ll> <passes.txt>
```

### passes.txt 格式
逗号分隔的 Pass 类名：
```
ConstantPropagation,AlgebraicIdentity,FunctionInlining,AllocaHoisting,Mem2Reg,DeadStoreElimination,DeadCodeElimination
```

### 可用 Pass 列表
- ConstantPropagation, ConstantFolding, AlgebraicIdentity
- FunctionInlining, AllocaHoisting, Mem2Reg
- DeadStoreElimination, DeadCodeElimination
- LICM, InstructionCombining, StrengthReduction
- LoopUnroll, CommonSubexpressionElimination
- StaticCallCounterPrinter
