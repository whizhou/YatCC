# 任务 5：后端代码生成

本次任务要求你完成从 LLVM IR 到目标机器汇编的翻译过程。当前实验默认目标平台为 RV64（`riscv64-unknown-linux-gnu`）。

## 输入

- 启用复活

  clang 以 `--target=riscv64-unknown-linux-gnu -march=rv64gc -mabi=lp64d -S -emit-llvm` 生成的 LLVM IR（`.ll` 文件）。

- 禁用复活

  任务 4 的输出，即优化后的 LLVM IR（`.ll` 文件）。

## 输出

文本格式的 RV64 汇编（`.s` 文件），要求其语义与输入 LLVM IR 一致，并能够与实验提供的运行时库一起链接运行。

## 评测

评测时会先生成标准答案，再将你的 `output.s` 与 `libsysy.a` 一起链接为 RV64 可执行文件，并通过 `qemu-riscv64-static` 运行。程序的标准输出和返回值必须与标准答案一致。

评测后，每个测例目录下会生成以下文件：

- `score.txt`

  评测的得分详情。

- `answer.ll`

  评测使用的 LLVM IR 输入。启用复活时由 clang 生成；禁用复活时由前置任务链路生成。

- `output.ll`

  仅在禁用复活时出现，即任务 4 输出的 LLVM IR。

- `answer.s`

  标准答案汇编。

- `output.s`

  你的答案（如果 task5 程序正常运行）。

- `answer.exe` 和 `output.exe`

  分别由 `riscv64-linux-gnu-g++ --static` 链接得到的可执行文件。

- `answer.compile` 和 `output.compile`

  链接 `answer.s` 与 `output.s` 过程中的输出。

- `answer.out` 和 `answer.err`

  运行 `answer.exe` 时的标准输出和标准错误输出，其中 `answer.err` 末尾包含程序返回值。

- `output.out` 和 `output.err`

  运行 `output.exe` 时的标准输出和标准错误输出，其中 `output.err` 末尾包含程序返回值。

## 基础代码

基础代码给出了一个简化的 RV64 后端框架，整体流程如下：

`LLVM IR -> MIR（虚拟寄存器） -> 线性扫描寄存器分配 -> RV64 汇编`

主要文件包括：

- `main.cpp`

  读取并校验输入 LLVM IR，设置 target triple，然后调用后端主流程。

- `EmitMIR.h` 和 `EmitMIR.cpp`

  将 LLVM IR 降低为简化的中间表示 `VInst`。这一层仍然使用虚拟寄存器，便于将指令生成与寄存器分配分开实现。

- `AllocReg.h` 和 `AllocReg.cpp`

  使用线性扫描算法为虚拟寄存器分配物理寄存器；分配失败的值会落到栈上的 spill slot 中。

- `EmitAsm.h` 和 `EmitAsm.cpp`

  根据寄存器分配结果输出最终 RV64 汇编，并处理函数序言/结语、访存、调用和分支等细节。

基础代码已经搭好了完整流程，并能覆盖一部分基础测例。你可以在此基础上继续补齐更多 LLVM IR 指令和边界情况。

## 一些提示

1. 先保证正确性，再考虑代码质量

   本任务只要求输出汇编的语义正确，不要求与 clang 的指令序列完全一致。先让程序能正确运行，再考虑代码是否更紧凑。

2. 先做带虚拟寄存器的指令生成

   建议先把 LLVM IR 翻译为带虚拟寄存器的 MIR，再单独做寄存器分配。这样结构更清晰，也更符合本实验当前的代码组织方式。

3. 优先处理调用约定和栈帧

   需要特别注意 `a0` ~ `a7`、`ra`、`sp`、`s0` 等寄存器的职责，以及局部变量、spill slot、保存寄存器在栈上的布局。

4. 注意 RV64 上的数据宽度

   指针和栈槽通常按 8 字节处理；而 `i32` 的 load/store、比较与返回值传递仍然要保持原语义，不能简单全部当作 64 位整数处理。

5. 控制流通常需要按基本块处理

   条件分支、循环、短路求值和 `phi` 节点都需要结合基本块边来翻译。一个常见做法是在前驱边上插入额外标签块来完成 `phi` 搬运。

6. 不必一开始追求完整后端

   当前基础代码是面向教学的简化实现。你可以先覆盖课程给出的基础测例，再逐步补齐更多 IR 指令和特殊情况。
