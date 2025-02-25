#
# Copyright (c) 2021-present, Trail of Bits, Inc.
# All rights reserved.
#
# This source code is licensed in accordance with the terms specified in
# the LICENSE file found in the root directory of this source tree.
#

add_executable(test_anvill_passes
  src/main.cpp

  src/Utils.h
  src/Utils.cpp

  src/BaseFunctionPass.cpp
  src/RecoverStackFrameInformation.cpp
  src/SplitStackFrameAtReturnAddress.cpp
  src/InstructionFolderPass.cpp
  src/BrightenPointers.cpp
  src/TransformRemillJump.cpp
)

target_link_libraries(test_anvill_passes PRIVATE
  remill_settings
  remill
  anvill_passes
  thirdparty_doctest
  thirdparty_magicenum
)

target_compile_definitions(test_anvill_passes PRIVATE
  ANVILL_TEST_DATA_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"
)

target_include_directories(test_anvill_passes PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

add_test(
  NAME test_anvill_passes
  COMMAND "$<TARGET_FILE:test_anvill_passes>"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
