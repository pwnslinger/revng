#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

revng_add_library_internal(
  revngPipeline
  SHARED
  ContainerSet.cpp
  Context.cpp
  Contract.cpp
  Errors.cpp
  GenericLLVMPipe.cpp
  Kind.cpp
  LLVMContainer.cpp
  Loader.cpp
  Runner.cpp
  RegisterKind.cpp
  Registry.cpp
  Step.cpp
  Target.cpp
  SavableObject.cpp
  InvalidationEvent.cpp)

target_link_libraries(revngPipeline revngSupport ${LLVM_LIBRARIES})
