#!/usr/bin/env python3
"""
被 task4-llm C++ 二进制通过 popen 调用。
接收 IR 路径，调用 LLM Agent 获取 Pass 序列，写入临时文件，输出文件路径。

用法: run_agent.py <input.ll>
输出: /tmp/myllm_passes_XXXXXX.txt 的路径
"""

import json
import os
import sys
import tempfile

def main():
    if len(sys.argv) != 2:
        sys.exit(1)

    input_ir = sys.argv[1]
    if not os.path.isfile(input_ir):
        sys.exit(1)

    # 确保 myllm 包可导入（从 task/4/ 目录）
    task4_dir = os.path.dirname(os.path.abspath(__file__))
    if task4_dir not in sys.path:
        sys.path.insert(0, task4_dir)

    # 加载配置
    env_file = os.path.join(task4_dir, ".env")
    from myllm.config import Config, _load_dotenv
    if os.path.exists(env_file):
        _load_dotenv(env_file)
    config = Config.from_env(env_file if os.path.exists(env_file) else "")

    # 读取 IR
    from pathlib import Path
    from myllm.features import extract_features
    from myllm.llm_client import LLMClient, remove_think_tags
    from myllm.passes import format_pass_descriptions_for_llm, validate_sequence

    ir_text = Path(input_ir).read_text(encoding="utf-8")
    features = extract_features(ir_text)

    # 构建 prompt
    prompt_dir = os.path.join(task4_dir, "myllm", "prompts")
    system_tpl = Path(os.path.join(prompt_dir, "system.txt")).read_text(encoding="utf-8")
    user_tpl = Path(os.path.join(prompt_dir, "user.txt")).read_text(encoding="utf-8")

    passes_desc = format_pass_descriptions_for_llm()
    system = system_tpl.replace("{passes_description}", passes_desc)

    snippet = ir_text[:3000]
    if len(ir_text) > 3000:
        snippet += "\n... (IR 已截断)"
    user = user_tpl.format(features_summary=features.summary(), ir_snippet=snippet)

    # 调用 LLM
    llm = LLMClient(
        api_key=config.api_key,
        base_url=config.base_url,
        model=config.model,
        temperature=config.temperature,
        max_tokens=config.max_tokens,
    )
    response = llm.single_chat(system, user, handlers=[remove_think_tags])

    # 解析 JSON 响应
    from myllm.llm_client import extract_json_from_response
    parsed = extract_json_from_response("")(response)
    sequence = parsed.get("sequence", [])
    if isinstance(sequence, str):
        sequence = [s.strip() for s in sequence.split(",") if s.strip()]
    sequence = validate_sequence(sequence)

    if not sequence:
        sys.exit(1)

    # 写入临时文件
    fd, tmp_path = tempfile.mkstemp(suffix=".txt", prefix="myllm_passes_")
    with os.fdopen(fd, "w") as f:
        f.write(",".join(sequence))

    # 输出路径供 C++ 读取
    print(tmp_path)

if __name__ == "__main__":
    main()
