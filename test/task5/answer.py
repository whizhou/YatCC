"""对给定的测例表调用 clang 生成 RV64 参考 LLVM IR 和汇编代码，
将输出保存到同名输出目录下的 answer.ll 与 answer.s 文件中。
然后再调用 riscv64-linux-gnu-g++ 链接 answer.s 为 answer.exe，
再运行 answer.exe，将输出保存到 answer.out 和 answer.err 文件中。
"""

import sys
import os.path as osp
import argparse
import subprocess as subps

sys.path.append(osp.abspath(__file__ + "/../.."))
from common import (
    CasesHelper,
    print_parsed_args,
    exit_if_cases_cached,
    cache_cases,
)

if __name__ == "__main__":
    parser = argparse.ArgumentParser("实验五答案生成", description=__doc__)
    parser.add_argument("srcdir", help="测例源码目录")
    parser.add_argument("bindir", help="输出目录")
    parser.add_argument("cases_file", help="测例表路径")
    parser.add_argument("gcc", help="gcc程序路径")
    parser.add_argument("clang", help="clang程序路径")
    parser.add_argument("qemu_path", help="qemu程序路径")
    parser.add_argument("rtlib", help="运行时库源码路径")
    parser.add_argument("rtlib_a", help="运行时库文件路径")
    args = parser.parse_args()
    print_parsed_args(parser, args)

    cache = exit_if_cases_cached(args.bindir, args.cases_file)

    print("加载测例表...", end="", flush=True)
    cases_helper = CasesHelper.load_file(
        args.srcdir,
        args.bindir,
        args.cases_file,
    )
    print("完成")

    clang_common_args = [
        "--target=riscv64-unknown-linux-gnu",
        "-march=rv64gc",
        "-mabi=lp64d",
        "-isystem",
        osp.join(args.rtlib, "include"),
    ]

    for case in cases_helper.cases:
        opt_level = "-O0" if case.name == "functional-3/063_nested_calls2.sysu.c" else "-O2"
        src_path = osp.join(args.srcdir, case.name)
        ll_path = cases_helper.of_case_bindir("answer.ll", case, True)
        print(ll_path, end=" ... ", flush=True)
        try:
            retn = subps.run(
                [
                    args.clang,
                    *clang_common_args,
                    opt_level,
                    "-S",
                    "-emit-llvm",
                    "-o",
                    ll_path,
                    src_path,
                ],
                timeout=30,
            ).returncode
        except subps.TimeoutExpired:
            print("TIMEOUT")
            continue
        if retn:
            print("FAIL", retn)
            exit(1)
        print("OK")

        asm_path = cases_helper.of_case_bindir("answer.s", case, True)
        print(asm_path, end=" ... ", flush=True)
        try:
            retn = subps.run(
                [
                    args.clang,
                    *clang_common_args,
                    opt_level,
                    "-S",
                    "-fno-addrsig",
                    "-o",
                    asm_path,
                    src_path,
                ],
                timeout=30,
            ).returncode
        except subps.TimeoutExpired:
            print("TIMEOUT")
            continue
        if retn:
            print("FAIL", retn)
            exit(1)
        print("OK")

        # 再将汇编代码编译为二进制程序
        exe_path = cases_helper.of_case_bindir("answer.exe", case, True)
        print(exe_path, end=" ... ", flush=True)
        try:
            with open(
                cases_helper.of_case_bindir("answer.compile", case), "w", encoding="utf-8"
            ) as f:
                retn = subps.run(
                    [
                        args.gcc,
                        "--static",
                        "-o",
                        exe_path,
                        asm_path,
                        args.rtlib_a,
                    ],
                    stdout=f,
                    stderr=f,
                    timeout=30,
                ).returncode
        except subps.TimeoutExpired:
            print("TIMEOUT")
            continue
        if retn:
            print("FAIL", retn)
            exit(2)
        print("OK")

        # 运行二进制程序，得到程序输出
        out_path = cases_helper.of_case_bindir("answer.out", case, True)
        err_path = cases_helper.of_case_bindir("answer.err", case, True)
        print(out_path, end=" ... ", flush=True)
        print("OK")
        print(err_path, end=" ... ", flush=True)
        with open(out_path, "w", encoding="utf-8") as f, open(
            err_path, "w", encoding="utf-8"
        ) as ferr:
            try:
                retn = subps.run(
                    [args.qemu_path, exe_path],
                    stdout=f,
                    stderr=ferr,
                    stdin=cases_helper.open_case_input(case)[1],
                    timeout=20,
                ).returncode
                ferr.write(f"Return Code: {retn}\n")
            except subps.TimeoutExpired:
                print("TIMEOUT")
            else:
                print("OK")

    cache_cases(args.bindir, cache)
