# task5-classic: LLVM IR -> RV64 assembly lowering (student-implemented).

set(TASK5_LLVM_RISCV_SOURCE_DIR "${YATCC_LLVM_SOURCE_DIR}/lib/Target/RISCV")
set(TASK5_LLVM_RISCV_GEN_DIR "${YATCC_LLVM_DIR}/task5/include")

# if(NOT EXISTS "${TASK5_LLVM_RISCV_SOURCE_DIR}/RISCV.td")
#   message(
#     FATAL_ERROR
#       "Task5 requires the LLVM RISC-V source tree under '${TASK5_LLVM_RISCV_SOURCE_DIR}'. "
#       "Please ensure YatCC_LLVM_DIR points to a full LLVM setup.")
# endif()

# if(NOT EXISTS "${TASK5_LLVM_RISCV_GEN_DIR}/RISCVGenRegisterInfo.inc"
#    OR NOT EXISTS "${TASK5_LLVM_RISCV_GEN_DIR}/RISCVGenInstrInfo.inc")
#   message(
#     FATAL_ERROR
#       "Task5 requires LLVM TableGen outputs under '${TASK5_LLVM_RISCV_GEN_DIR}'. "
#       "Please run ./llvm/setup.sh or rebuild the devcontainer image with Task5 LLVM artifacts.")
# endif()

add_executable(task5-classic main.cpp EmitMIR.cpp EmitAsm.cpp AllocReg.cpp)
target_include_directories(task5-classic PRIVATE . ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(task5-classic PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(task5-classic PRIVATE
  ${TASK5_LLVM_RISCV_SOURCE_DIR}
  ${TASK5_LLVM_RISCV_SOURCE_DIR}/MCTargetDesc
  ${TASK5_LLVM_RISCV_GEN_DIR}
)
target_link_libraries(task5-classic LLVM)

add_custom_target(task5 DEPENDS task5-classic)
