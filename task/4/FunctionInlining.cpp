#include "FunctionInlining.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

using namespace llvm;

/// 在函数调用图中检测一个函数是否参与环（递归调用）
/// 使用三色标记 DFS：
///   白色(0) = 未访问
///   灰色(1) = 正在访问（在当前 DFS 栈上）
///   黑色(2) = 已完成访问
/// 如果发现灰色节点，说明存在环
static bool
hasCycleDFS(Function* func,
            std::map<Function*, int>& color,
            const std::map<Function*, std::set<Function*>>& callGraph)
{
  color[func] = 1; // 标记为灰色（正在访问）

  auto it = callGraph.find(func);
  if (it != callGraph.end()) {
    for (Function* callee : it->second) {
      if (!callee || callee->isDeclaration())
        continue;
      int& c = color[callee];
      if (c == 1)
        return true; // 发现环：目标节点是灰色
      if (c == 0) {
        if (hasCycleDFS(callee, color, callGraph))
          return true;
      }
    }
  }

  color[func] = 2; // 标记为黑色（已完成）
  return false;
}

/// 构建函数调用图
static std::map<Function*, std::set<Function*>>
buildCallGraph(Module& mod)
{
  std::map<Function*, std::set<Function*>> cg;

  for (Function& func : mod) {
    if (func.isDeclaration())
      continue;
    for (BasicBlock& bb : func) {
      for (Instruction& inst : bb) {
        if (auto* call = dyn_cast<CallInst>(&inst)) {
          if (Function* callee = call->getCalledFunction()) {
            if (!callee->isDeclaration())
              cg[&func].insert(callee);
          }
        }
      }
    }
  }

  return cg;
}

/// 判断函数是否可以内联（不参与调用图中的环）
static bool
canInline(Function* func,
          const std::map<Function*, std::set<Function*>>& callGraph)
{
  // 不内联 main 函数和无定义的函数
  if (func->isDeclaration() || func->getName() == "main")
    return false;

  // 不内联有特殊属性的函数（但在 -O0 输入中忽略 noinline/optnone）
  // 因为我们是优化 pass，应该激进内联

  // 使用三色标记检测函数是否在调用图的环中
  std::map<Function*, int> color;
  // 初始化所有函数为白色（未访问）
  for (auto& [f, _] : callGraph)
    color[f] = 0;

  // 从 func 出发做 DFS，检测是否存在环
  return !hasCycleDFS(func, color, callGraph);
}

/// 克隆一个基本块，并使用 vmap 重映射操作数
/// 不会将克隆块插入任何函数
static BasicBlock*
cloneBasicBlockWithRemap(const BasicBlock* BB,
                         DenseMap<const Value*, Value*>& vmap,
                         const Twine& NameSuffix)
{
  BasicBlock* newBB =
    BasicBlock::Create(BB->getContext(), BB->getName() + NameSuffix);

  for (auto it = BB->begin(), ie = BB->end(); it != ie; ++it) {
    const Instruction& inst = *it;
    Instruction* newInst = inst.clone();

    // 重映射操作数
    for (unsigned i = 0; i < newInst->getNumOperands(); ++i) {
      Value* op = newInst->getOperand(i);
      auto mapIt = vmap.find(op);
      if (mapIt != vmap.end())
        newInst->setOperand(i, mapIt->second);
    }

    // 插入新指令到克隆块末尾
    newInst->insertInto(newBB, newBB->end());

    // 记录指令映射（非终结指令用于替换使用者）
    vmap[&inst] = newInst;
  }

  return newBB;
}

/// 内联一个函数调用
/// 返回 true 表示成功内联
static bool
inlineCallSite(CallInst* callInst)
{
  // 安全检查：确保 callInst 仍然有效
  if (!callInst || !callInst->getParent())
    return false;

  Function* callee = callInst->getCalledFunction();
  if (!callee || callee->isDeclaration())
    return false;

  Function* caller = callInst->getFunction();

  // ================================================================
  // 第一步：构建值映射（参数 -> 实际参数）
  // ================================================================
  DenseMap<const Value*, Value*> vmap;

  // 映射函数参数到调用的实际参数
  auto ci = callInst->arg_begin();
  for (auto ai = callee->arg_begin(), ae = callee->arg_end(); ai != ae;
       ++ai, ++ci) {
    vmap[&*ai] = *ci;
  }

  // ================================================================
  // 第二步：克隆被调用函数的基本块
  // ================================================================
  // 先克隆所有基本块（此时指令操作数中的 BasicBlock 引用还未更新）
  std::vector<BasicBlock*> clonedBlocks;
  for (BasicBlock& calleeBB : *callee) {
    BasicBlock* clonedBB = cloneBasicBlockWithRemap(&calleeBB, vmap, "");
    clonedBlocks.push_back(clonedBB);
    // 记录基本块映射（用于后续更新分支目标）
    vmap[&calleeBB] = clonedBB;
  }

  // 第二遍：更新分支指令中的 BasicBlock 引用
  for (BasicBlock* clonedBB : clonedBlocks) {
    for (Instruction& inst : *clonedBB) {
      if (auto* br = dyn_cast<BranchInst>(&inst)) {
        for (unsigned i = 0; i < br->getNumOperands(); ++i) {
          if (auto* succBB = dyn_cast<BasicBlock>(br->getOperand(i))) {
            auto it = vmap.find(succBB);
            if (it != vmap.end())
              br->setOperand(i, it->second);
          }
        }
      }
    }
  }

  // ================================================================
  // 第三步：在调用处分割基本块
  // ================================================================
  BasicBlock* callBlock = callInst->getParent();

  // 在 call 处分割：callBlock 保留 call 之前的指令，
  // afterCallBB 包含 call 及 call 之后的指令
  BasicBlock* afterCallBB =
    callBlock->splitBasicBlock(callInst, "inline.cont");

  // splitBasicBlock 在 callBlock 末尾插入了 br afterCallBB，删除它
  // 稍后将 callBlock 的 terminator 替换为 br clonedEntry
  callBlock->getTerminator()->eraseFromParent();

  // ================================================================
  // 第四步：创建合并块
  // ================================================================
  BasicBlock* mergeBB =
    BasicBlock::Create(caller->getContext(), "inline.merge", caller,
                       afterCallBB);

  // ================================================================
  // 第五步：处理返回指令
  // ================================================================
  std::vector<ReturnInst*> returns;
  for (BasicBlock* clonedBB : clonedBlocks) {
    if (auto* ret = dyn_cast<ReturnInst>(clonedBB->getTerminator()))
      returns.push_back(ret);
  }

  bool hasReturnValue = callee->getReturnType()->isVoidTy() ? false : true;
  PHINode* phi = nullptr;

  if (!returns.empty()) {
    // 有返回指令
    if (hasReturnValue && returns.size() > 1) {
      // 多个返回点且有返回值 -> 需要 PHI 节点
      phi = PHINode::Create(callee->getReturnType(), returns.size(),
                            "inline.ret", mergeBB);
    }

    for (ReturnInst* ret : returns) {
      BasicBlock* retBlock = ret->getParent();
      if (hasReturnValue) {
        Value* retVal = ret->getReturnValue();
        if (phi)
          phi->addIncoming(retVal, retBlock);
        else
          // 单个返回点，记录返回值
          callInst->replaceAllUsesWith(retVal);
      }
      // 将 return 替换为跳转到 mergeBB
      ret->eraseFromParent();
      BranchInst::Create(mergeBB, retBlock);
    }
  }

  // ================================================================
  // 第六步：将克隆的基本块插入 caller 中
  // ================================================================
  // 在 callBlock 之后插入所有克隆块
  BasicBlock* insertPos = callBlock;
  for (BasicBlock* clonedBB : clonedBlocks) {
    clonedBB->insertInto(caller, insertPos->getNextNode());
    insertPos = clonedBB;
  }

  // ================================================================
  // 第七步：设置控制流
  // ================================================================
  // callBlock 跳转到内联函数的入口
  auto entryIt = vmap.find(&callee->getEntryBlock());
  BasicBlock* clonedEntry = cast<BasicBlock>(entryIt->second);
  BranchInst::Create(clonedEntry, callBlock);

  // mergeBB 跳转到 call 之后的代码
  if (returns.empty()) {
    // 无返回指令（例如 callee 有无限循环）
    new UnreachableInst(caller->getContext(), mergeBB);
  } else {
    BranchInst::Create(afterCallBB, mergeBB);
  }

  // ================================================================
  // 第八步：替换 call 指令的使用并删除
  // ================================================================
  if (phi) {
    callInst->replaceAllUsesWith(phi);
  } else if (!hasReturnValue) {
    // void 返回，无操作
  } else if (returns.empty()) {
    // 无返回指令但函数非 void -> 用 undef 替换
    callInst->replaceAllUsesWith(UndefValue::get(callee->getReturnType()));
  }

  callInst->eraseFromParent();
  return true;
}

/// 删除无调用者的函数（不包括 main 和声明）
static int
removeDeadFunctions(Module& mod)
{
  int removed = 0;

  // 收集所有仍有调用者的函数
  std::set<Function*> calledFunctions;
  for (Function& func : mod) {
    if (func.isDeclaration())
      continue;
    for (BasicBlock& bb : func) {
      for (Instruction& inst : bb) {
        if (auto* call = dyn_cast<CallInst>(&inst)) {
          if (Function* callee = call->getCalledFunction())
            calledFunctions.insert(callee);
        }
      }
    }
  }

  // 删除无调用者的函数
  std::vector<Function*> toRemove;
  for (Function& func : mod) {
    if (func.isDeclaration())
      continue;
    if (func.getName() == "main")
      continue;
    if (calledFunctions.count(&func) == 0)
      toRemove.push_back(&func);
  }

  for (Function* func : toRemove) {
    func->eraseFromParent();
    ++removed;
  }

  return removed;
}

PreservedAnalyses
FunctionInlining::run(Module& mod, ModuleAnalysisManager& mam)
{
  // 剥离 noinline 和 optnone 属性（clang -O0 生成的 IR 带有这些属性，
  // 会阻止后续优化 pass 正常工作）
  for (Function& func : mod) {
    if (func.isDeclaration())
      continue;
    func.removeFnAttr(Attribute::NoInline);
    func.removeFnAttr(Attribute::OptimizeNone);
  }

  int inlined = 0;
  int removed = 0;

  // 反复内联，直到没有新的内联发生
  while (true) {
    // 构建函数调用图
    auto callGraph = buildCallGraph(mod);

    // 收集所有可内联的调用点
    std::vector<CallInst*> candidates;
    for (Function& func : mod) {
      if (func.isDeclaration())
        continue;
      for (BasicBlock& bb : func) {
        for (Instruction& inst : bb) {
          if (auto* call = dyn_cast<CallInst>(&inst)) {
            if (Function* callee = call->getCalledFunction()) {
              if (canInline(callee, callGraph))
                candidates.push_back(call);
            }
          }
        }
      }
    }

    if (candidates.empty())
      break;

    // 内联所有候选调用
    for (CallInst* call : candidates) {
      if (inlineCallSite(call))
        ++inlined;
    }

    // 内联后删除无调用的函数
    removed += removeDeadFunctions(mod);
  }

  mOut << "FunctionInlining running...\n"
       << "Inlined " << inlined << " call sites\n"
       << "Removed " << removed << " dead functions\n";

  if (inlined == 0)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
