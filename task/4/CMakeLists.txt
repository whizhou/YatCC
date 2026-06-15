# ── task4-classic: 传统固定管线 ──
file(GLOB _classic_src
     main.cpp
     AlgebraicIdentity.cpp AllocaHoisting.cpp
     CommonSubexpressionElimination.cpp ConstantFolding.cpp
     ConstantPropagation.cpp DeadCodeElimination.cpp
     DeadStoreElimination.cpp FunctionInlining.cpp
     InstructionCombining.cpp LICM.cpp LoopUnroll.cpp
     Mem2Reg.cpp StaticCallCounter.cpp StaticCallCounterPrinter.cpp
     StrengthReduction.cpp
     *.hpp *.c *.h)

add_executable(task4-classic ${_classic_src})

target_include_directories(task4-classic PRIVATE . ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(task4-classic PRIVATE ${LLVM_INCLUDE_DIRS})

target_link_libraries(task4-classic LLVM)

# ── task4-llm: LLM 驱动的动态 Pass 序列执行器 ──
#    2 参数: task4-llm <input> <output>         → 调 Python Agent 获取 LLM 序列
#    3 参数: task4-llm <input> <output> <passes> → 按指定序列执行
file(GLOB _pass_src
     AlgebraicIdentity.cpp AllocaHoisting.cpp
     CommonSubexpressionElimination.cpp ConstantFolding.cpp
     ConstantPropagation.cpp DeadCodeElimination.cpp
     DeadStoreElimination.cpp FunctionInlining.cpp
     InstructionCombining.cpp LICM.cpp LoopUnroll.cpp
     Mem2Reg.cpp StaticCallCounter.cpp StaticCallCounterPrinter.cpp
     StrengthReduction.cpp)

add_executable(task4-llm task4-agent.cpp ${_pass_src})

target_include_directories(task4-llm PRIVATE . ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(task4-llm PRIVATE ${LLVM_INCLUDE_DIRS})

target_link_libraries(task4-llm LLVM)

target_compile_definitions(task4-llm
                           PRIVATE TASK4_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# ── 汇总 ──
add_custom_target(task4 DEPENDS task4-classic task4-llm)
