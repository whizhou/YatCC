# 获取实时库路径
file(REAL_PATH ../rtlib _rtlib_dir)
# 获取task0和task3的输出目录路径
file(REAL_PATH ../task5 _task5_out BASE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
file(REAL_PATH ../cases _task5_in BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
file(REAL_PATH ../task3 _task3_out BASE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set(GCC_EXE /usr/bin/riscv64-linux-gnu-g++)
if(EXISTS /usr/bin/clang-18)
  set(CLANG_EXE /usr/bin/clang-18)
else()
  set(CLANG_EXE ${CLANG_EXECUTABLE})
endif()
set(TASK5_RUNTIME ${CMAKE_CURRENT_SOURCE_DIR}/libsysy.a)
set(QENU /usr/bin/qemu-riscv64-static)

# 生成测例表(Generate test case table)
if(NOT TASK5_CASES_TXT)
  # 如果未指定测例列表文件，则根据测试排除规则筛选测例
  set(_task5_cases "")
  foreach(_case ${TEST_CASES})
    if(NOT _case MATCHES ${TASK5_EXCLUDE_REGEX})
      list(APPEND _task5_cases ${_case})
    endif()
  endforeach()
  # 将筛选后的测例列表写入cases.txt文件
  set(TASK5_CASES_TXT ${CMAKE_CURRENT_BINARY_DIR}/cases.txt)
  string(REPLACE ";" "\n" _lines "${_task5_cases}")
  file(WRITE ${TASK5_CASES_TXT} "${_lines}")
else()
  # 如果已指定测例列表文件，则使用Python脚本处理测例
  exec_program(
    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/..
    ARGS common.py ${TEST_CASES_DIR} ${CMAKE_CURRENT_BINARY_DIR}
         ${TASK5_CASES_TXT}
    OUTPUT_VARIABLE _task5_cases)
endif()

# 生成全部答案(Generate all answers)
add_custom_target(
  task5-answer
  ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/answer.py
  ${TEST_CASES_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${TASK5_CASES_TXT}
  ${GCC_EXE}
  ${CLANG_EXE}
  ${QENU}
  ${_rtlib_dir}
  ${TASK5_RUNTIME}
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  USES_TERMINAL
  SOURCES answer.py)

# 设置依赖关系：task5的答案依赖于运行时库
add_dependencies(task5-answer test-rtlib)

message(STATUS "TEST_RTLIB_SO = ${TEST_RTLIB_SO}")
# 对实验进行评分(Score the experiment)
add_custom_target(
  task5-score
  COMMAND
    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/score.py ${TEST_CASES_DIR}
    ${CMAKE_CURRENT_BINARY_DIR} ${TASK5_CASES_TXT} ${CTEST_COMMAND}
    ${GCC_EXE} ${QENU} ${TASK5_RUNTIME}
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  USES_TERMINAL
  SOURCES score.py)

if(TASK5_REVIVE)
  add_dependencies(task5-score task5-classic task5-answer test-rtlib)
else()
  add_dependencies(task5-score task5-classic task4 task3-answer task5-answer test-rtlib)
endif()

# 一键测评 task5-classic（别名，与 task5-score 等价）
add_custom_target(task5-classic-score DEPENDS task5-score)


foreach(_case ${_task5_cases})
  set(_output_dir ${CMAKE_CURRENT_BINARY_DIR}/${_case})
  file(MAKE_DIRECTORY ${_output_dir})
  if(TASK5_REVIVE)
    if(_case STREQUAL "functional-2/028_arr_defn3.sysu.c")
      set(_task5_ir_opt -O2)
    else()
      set(_task5_ir_opt -O0)
    endif()
    add_test(
      NAME task5/${_case}
      COMMAND
        bash -c
        "${CLANG_EXE} --target=riscv64-unknown-linux-gnu \
          -march=rv64gc -mabi=lp64d \
          -S -emit-llvm ${_task5_ir_opt} \
          -isystem '${_rtlib_dir}/include' \
          -o '${_output_dir}/answer.ll' \
          '${_task5_in}/${_case}' && \
         ${CMAKE_BINARY_DIR}/task/5/task5-classic \
          '${_output_dir}/answer.ll' '${_output_dir}/output.s'")
  else()
    # 不复活：先跑 task4（同学实现）产出 IR，再用 task5-llvm 降低到汇编。
    if(_case MATCHES ${TASK5_LLM_REGEX})
      add_test(
        NAME task5/${_case}
        COMMAND
          bash -c
          "${CMAKE_BINARY_DIR}/task/4/task4-llm \
            '${_task3_out}/${_case}/answer.ll' '${_output_dir}/output.ll' && \
           ${CMAKE_BINARY_DIR}/task/5/task5-classic \
            '${_output_dir}/output.ll' '${_output_dir}/output.s'")
    else()
      add_test(
        NAME task5/${_case}
        COMMAND
          bash -c
          "${CMAKE_BINARY_DIR}/task/4/task4-classic \
            '${_task3_out}/${_case}/answer.ll' '${_output_dir}/output.ll' && \
           ${CMAKE_BINARY_DIR}/task/5/task5-classic \
            '${_output_dir}/output.ll' '${_output_dir}/output.s'")
    endif()
  endif()
  # 为每个测例添加评分测试
  add_test(
    NAME test5/${_case}
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/score.py
      ${TEST_CASES_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${TASK5_CASES_TXT}
      ${CTEST_COMMAND} ${GCC_EXE} ${QENU} ${TASK5_RUNTIME} --single
      ${_case})
endforeach()

message(AUTHOR_WARNING "在实验五默认复活")
