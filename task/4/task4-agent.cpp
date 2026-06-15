/**
 * task4-llm: 基于 LLM 的动态 Pass 序列执行器
 *
 * 用法:
 *   task4-llm <input.ll> <output.ll>              — LLM 管线（调 Python Agent）
 *   task4-llm <input.ll> <output.ll> <passes.txt> — 指定 Pass 序列
 *
 * passes.txt 格式: 逗号分隔的 Pass 类名列表
 */

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>

#include "AlgebraicIdentity.hpp"
#include "AllocaHoisting.hpp"
#include "CommonSubexpressionElimination.hpp"
#include "ConstantFolding.hpp"
#include "ConstantPropagation.hpp"
#include "DeadCodeElimination.hpp"
#include "DeadStoreElimination.hpp"
#include "FunctionInlining.hpp"
#include "InstructionCombining.hpp"
#include "LICM.hpp"
#include "LoopUnroll.hpp"
#include "Mem2Reg.hpp"
#include "StaticCallCounter.hpp"
#include "StaticCallCounterPrinter.hpp"
#include "StrengthReduction.hpp"

#ifndef TASK4_DIR
#define TASK4_DIR "."
#endif

// ──────────────────────────────────────────
// 工具函数
// ──────────────────────────────────────────

/// 从文件读取 Pass 序列（逗号分隔）
static std::vector<std::string>
readPassSequence(const std::string& filePath)
{
  std::ifstream file(filePath);
  if (!file.is_open()) {
    std::cerr << "Error: 无法打开 Pass 序列文件: " << filePath << '\n';
    return {};
  }

  std::string content;
  std::getline(file, content);
  file.close();

  std::vector<std::string> passes;
  std::istringstream ss(content);
  std::string token;
  while (std::getline(ss, token, ',')) {
    size_t start = token.find_first_not_of(" \t\n\r");
    size_t end = token.find_last_not_of(" \t\n\r");
    if (start != std::string::npos && end != std::string::npos) {
      passes.push_back(token.substr(start, end - start + 1));
    }
  }
  return passes;
}

/// 调用 Python Agent 获取 LLM 推荐的 Pass 序列
/// 返回临时文件路径（内含逗号分隔的 Pass 名称），失败返回空串
static std::string
queryLLMAgent(const std::string& inputPath)
{
  // popen 调 run_agent.py，它输出临时文件路径到 stdout
  std::string cmd = std::string("python3 ") + TASK4_DIR + "/run_agent.py " +
                    inputPath + " 2>/dev/null";

  std::array<char, 512> buf{};
  std::string result;
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    std::cerr << "Error: 无法启动 LLM Agent\n";
    return "";
  }
  while (fgets(buf.data(), buf.size(), pipe) != nullptr) {
    result += buf.data();
  }
  int rc = pclose(pipe);
  if (rc != 0) {
    std::cerr << "Error: LLM Agent 失败 (exit=" << rc << ")\n";
    return "";
  }

  // 去除尾部换行
  while (!result.empty() &&
         (result.back() == '\n' || result.back() == '\r'))
    result.pop_back();

  if (result.empty()) {
    std::cerr << "Error: LLM Agent 未返回 Pass 序列\n";
    return "";
  }
  return result;
}

// ──────────────────────────────────────────
// Pass 映射
// ──────────────────────────────────────────

static std::unordered_map<
  std::string,
  std::function<void(llvm::ModulePassManager&)>>
buildPassMap(llvm::raw_ostream& out)
{
  std::unordered_map<std::string,
                     std::function<void(llvm::ModulePassManager&)>> map;

  map["ConstantPropagation"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(ConstantPropagation(out));
  };
  map["ConstantFolding"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(ConstantFolding(out));
  };
  map["AlgebraicIdentity"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(AlgebraicIdentity(out));
  };
  map["FunctionInlining"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(FunctionInlining(out));
  };
  map["AllocaHoisting"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(AllocaHoisting(out));
  };
  map["Mem2Reg"] = [](llvm::ModulePassManager& mpm) {
    mpm.addPass(Mem2Reg());
  };
  map["DeadStoreElimination"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(DeadStoreElimination(out));
  };
  map["DeadCodeElimination"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(DeadCodeElimination(out));
  };
  map["LICM"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(LICM(out));
  };
  map["InstructionCombining"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(InstructionCombining(out));
  };
  map["StrengthReduction"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(StrengthReduction(out));
  };
  map["LoopUnroll"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(LoopUnroll(out));
  };
  map["CommonSubexpressionElimination"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(CommonSubexpressionElimination(out));
  };
  map["StaticCallCounterPrinter"] = [&out](llvm::ModulePassManager& mpm) {
    mpm.addPass(StaticCallCounterPrinter(out));
  };

  return map;
}

// ──────────────────────────────────────────
// main
// ──────────────────────────────────────────

int
main(int argc, char** argv)
{
  if (argc < 3 || argc > 4) {
    std::cout << "Usage: " << argv[0]
              << " <input.ll> <output.ll> [<passes.txt>]\n";
    return -1;
  }

  std::string inputPath = argv[1];
  std::string outputPath = argv[2];
  // std::string passesPath = (argc == 4) ? argv[3] : "";
  std::string passesPath = "";  // 强制使用 LLM Agent 模式

  // ── 获取 Pass 序列 ──
  std::vector<std::string> passSequence;

  if (!passesPath.empty()) {
    // 模式 B: 从文件读取
    passSequence = readPassSequence(passesPath);
    if (passSequence.empty()) {
      std::cerr << "Error: Pass 序列为空\n";
      return -4;
    }
  } else {
    // 模式 A: 调用 LLM Agent
    std::cerr << "[task4-llm] 查询 LLM Agent...\n";
    std::string tmpFile = queryLLMAgent(inputPath);
    if (tmpFile.empty()) {
      std::cerr << "Error: 无法获取 LLM Pass 序列\n";
      return -6;
    }
    passSequence = readPassSequence(tmpFile);
    std::remove(tmpFile.c_str()); // 清理临时文件
    if (passSequence.empty()) {
      std::cerr << "Error: LLM 返回的 Pass 序列为空\n";
      return -4;
    }
    std::cerr << "[task4-llm] LLM 序列: ";
    for (size_t i = 0; i < passSequence.size(); ++i) {
      std::cerr << passSequence[i]
                << (i + 1 < passSequence.size() ? "," : "\n");
    }
  }

  // ── 解析输入 IR ──
  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  auto mod = llvm::parseIRFile(inputPath, err, ctx);
  if (!mod) {
    std::cerr << "Error: 无法解析输入文件: " << inputPath << '\n';
    err.print(argv[0], llvm::errs());
    return -2;
  }

  std::error_code ec;
  llvm::raw_fd_ostream outFile(outputPath, ec);
  if (ec) {
    std::cerr << "Error: 无法打开输出文件: " << outputPath << '\n';
    return -3;
  }

  // ── 初始化分析管理器 ──
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::ModulePassManager mpm;

  llvm::PassBuilder pb;
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  mam.registerPass([]() { return StaticCallCounter(); });

  // ── 构建并执行 Pass 管线 ──
  auto passMap = buildPassMap(llvm::errs());

  for (const auto& passName : passSequence) {
    auto it = passMap.find(passName);
    if (it == passMap.end()) {
      std::cerr << "Error: 未知的 Pass: " << passName << '\n';
      std::cerr << "可用 Pass: ";
      for (const auto& [name, _] : passMap)
        std::cerr << name << " ";
      std::cerr << '\n';
      return -5;
    }
    it->second(mpm);
  }

  mpm.run(*mod, mam);

  // ── 输出 ──
  mod->print(outFile, nullptr, false, true);

  if (llvm::verifyModule(*mod, &llvm::outs())) {
    std::cerr << "Warning: 模块验证失败\n";
    return 3;
  }

  return 0;
}
