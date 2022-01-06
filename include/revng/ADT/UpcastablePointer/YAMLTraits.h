#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/Support/YAMLTraits.h"

#include "revng/ADT/UpcastablePointer.h"

template<typename O, size_t I = 0>
void initializeOwningPointer(llvm::yaml::IO &TheIO, O &Obj) {
  using concrete_types = concrete_types_traits_t<typename O::element_type>;

  if constexpr (I < std::tuple_size_v<concrete_types>) {
    using type = typename std::tuple_element_t<I, concrete_types>;
    if (TheIO.mapTag(type::Tag)) {
      Obj.reset(new type);
    } else {
      initializeOwningPointer<O, I + 1>(TheIO, Obj);
    }
  } else {
    revng_abort();
  }
}

template<typename O, size_t I = 0>
void dispatchMappingTraits(llvm::yaml::IO &TheIO, O &Obj) {
  using concrete_types = concrete_types_traits_t<typename O::element_type>;

  if constexpr (I < std::tuple_size_v<concrete_types>) {
    using type = typename std::tuple_element_t<I, concrete_types>;
    auto Pointer = Obj.get();
    if (llvm::isa<type>(Pointer)) {
      TheIO.mapTag(type::Tag, true);
      llvm::yaml::MappingTraits<type>::mapping(TheIO,
                                               *llvm::cast<type>(Pointer));
    } else {
      dispatchMappingTraits<O, I + 1>(TheIO, Obj);
    }
  } else {
    revng_abort();
  }
}

template<UpcastablePointerLike T>
struct PolymorphicMappingTraits {
  static void mapping(llvm::yaml::IO &TheIO, T &Obj) {
    if (!TheIO.outputting())
      initializeOwningPointer(TheIO, Obj);

    dispatchMappingTraits(TheIO, Obj);
  }
};
