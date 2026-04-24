file(REAL_PATH ../rtlib _rtlib_dir)
file(REAL_PATH ../task0 _task0_out BASE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
file(REAL_PATH ../task2 _task2_out BASE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# 生成测例表
if(NOT TASK3_CASES_TXT)
  set(_task3_cases "")
  foreach(_case ${TEST_CASES})
    if(NOT _case MATCHES ${TASK3_EXCLUDE_REGEX})
      list(APPEND _task3_cases ${_case})
    endif()
  endforeach()
  set(TASK3_CASES_TXT ${CMAKE_CURRENT_BINARY_DIR}/cases.txt)
  string(REPLACE ";" "\n" _lines "${_task3_cases}")
  file(WRITE ${TASK3_CASES_TXT} "${_lines}")
else()
  exec_program(
    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/..
    ARGS common.py ${TEST_CASES_DIR} ${CMAKE_CURRENT_BINARY_DIR}
         ${TASK3_CASES_TXT}
    OUTPUT_VARIABLE _task3_cases)
endif()

# 生成全部答案
add_custom_target(
  task3-answer
  ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/answer.py
  ${TEST_CASES_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${TEST_CASES_TXT}
  ${CLANG_EXECUTABLE}
  ${_rtlib_dir}
  ${TEST_RTLIB_SO}
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  USES_TERMINAL
  SOURCES answer.py)

add_dependencies(task3-answer task0-answer test-rtlib)

# 对实验进行评分
add_custom_target(
  task3-score
  COMMAND
    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/score.py ${TEST_CASES_DIR}
    ${CMAKE_CURRENT_BINARY_DIR} ${TASK3_CASES_TXT} ${CTEST_COMMAND}
    ${CLANG_PLUS_EXECUTABLE} ${TEST_RTLIB_SO}
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  USES_TERMINAL
  SOURCES score.py)

add_dependencies(task3-score task3 task3-answer test-rtlib)

# 为每个测例创建一个测试
if(TASK3_REVIVE)
  # 如果启用复活，则将前一个实验的标准答案作为输入
  add_dependencies(task3-score task2-answer)

  foreach(_case ${_task3_cases})
    set(_output_dir ${CMAKE_CURRENT_BINARY_DIR}/${_case})
    file(MAKE_DIRECTORY ${_output_dir})
    add_test(NAME task3/${_case}
             COMMAND task3 ${_task2_out}/${_case}/answer.json
                     ${_output_dir}/output.ll)
    add_test(
      NAME test3/${_case}
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/score.py
        ${TEST_CASES_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${TASK3_CASES_TXT}
        ${CTEST_COMMAND} ${CLANG_PLUS_EXECUTABLE} ${TEST_RTLIB_SO} --single
        ${_case})
  endforeach()

  message(AUTHOR_WARNING "在实验三复活：请在构建 task2-answer 后再使用 task3 的测试项目。")

else()
  # 否则以实验零的标准答案作为输入
  add_dependencies(task3-score task0-answer)

  foreach(_case ${_task3_cases})
    set(_output_dir ${CMAKE_CURRENT_BINARY_DIR}/${_case})
    file(MAKE_DIRECTORY ${_output_dir})
    add_test(NAME task3/${_case} COMMAND task3 ${_task0_out}/${_case}
                                         ${_output_dir}/output.ll)
    add_test(
      NAME test3/${_case}
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/score.py
        ${TEST_CASES_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${TASK3_CASES_TXT}
        ${CTEST_COMMAND} ${CLANG_PLUS_EXECUTABLE} ${TEST_RTLIB_SO} --single
        ${_case})
  endforeach()

  message(AUTHOR_WARNING "实验三复活已禁用，请在构建 task0-answer 后再使用 task3 的测试项目。")

endif()

# ############################################################################ #
# 提供 diy 功能，对单独的cases目录进行重新编译与评分，跳过缓存机制。
# ############################################################################ #

# 生成 diy 测例答案
add_custom_target(
  task3-diy-answer
  ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/answer.py
  ${DIY_TEST_CASES_DIR} # 测例目录
  ${CMAKE_CURRENT_BINARY_DIR}/diy-cases # 输出目录
  ${DIY_TEST_CASES_TXT} # 测例表
  ${CLANG_EXECUTABLE}
  ${_rtlib_dir}
  ${TEST_RTLIB_SO}
  --no-cache # 禁用缓存机制
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  USES_TERMINAL
  SOURCES answer.py)

add_dependencies(task3-diy-answer task0-diy-answer test-rtlib)

# 对 diy 实验进行评分
add_custom_target(
  task3-diy-score
  COMMAND
    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/score.py
    ${DIY_TEST_CASES_DIR} ${CMAKE_CURRENT_BINARY_DIR}/diy-cases
    ${DIY_TEST_CASES_TXT} ${CTEST_COMMAND} ${CLANG_PLUS_EXECUTABLE}
    ${TEST_RTLIB_SO} --diy-mod
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  USES_TERMINAL
  SOURCES score.py)

add_dependencies(task3-diy-score task3 task3-diy-answer test-rtlib)

# 为每个 diy 测例创建一个测试和评分
if(TASK3_REVIVE)
  # 如果启用复活，则将前一个实验的标准答案作为输入
  add_dependencies(task3-diy-score task2-diy-answer)

  foreach(_diy_case ${DIY_TEST_CASES})
    set(_diy_output_dir ${CMAKE_CURRENT_BINARY_DIR}/diy-cases/${_diy_case})
    file(MAKE_DIRECTORY ${_diy_output_dir})
    add_test(NAME task3-diy/${_diy_case}
             COMMAND task3 ${_task2_out}/diy-cases/${_diy_case}/answer.json
                     ${_diy_output_dir}/output.ll)
    add_test(
      NAME test3-diy/${_diy_case}
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/score.py
        ${DIY_TEST_CASES_DIR} ${CMAKE_CURRENT_BINARY_DIR}/diy-cases
        ${DIY_TEST_CASES_TXT} ${CTEST_COMMAND} ${CLANG_PLUS_EXECUTABLE}
        ${TEST_RTLIB_SO} --single ${_diy_case})
  endforeach()

  message(AUTHOR_WARNING "在实验三复活：请在构建 task2-diy-answer 后再使用 task3 的 diy 测试项目。")

else()
  # 否则以实验零的标准答案作为输入
  add_dependencies(task3-diy-score task0-diy-answer)

  foreach(_diy_case ${DIY_TEST_CASES})
    set(_diy_output_dir ${CMAKE_CURRENT_BINARY_DIR}/diy-cases/${_diy_case})
    file(MAKE_DIRECTORY ${_diy_output_dir})
    add_test(NAME task3-diy/${_diy_case}
             COMMAND task3 ${_task0_out}/diy-cases/${_diy_case}
                     ${_diy_output_dir}/output.ll)
    add_test(
      NAME test3-diy/${_diy_case}
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/score.py
        ${DIY_TEST_CASES_DIR} ${CMAKE_CURRENT_BINARY_DIR}/diy-cases
        ${DIY_TEST_CASES_TXT} ${CTEST_COMMAND} ${CLANG_PLUS_EXECUTABLE}
        ${TEST_RTLIB_SO} --single ${_diy_case})
  endforeach()

  message(AUTHOR_WARNING "实验三复活已禁用，请在构建 task0-diy-answer 后再使用 task3 的 diy 测试项目。")

endif()
