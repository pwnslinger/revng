/// \file FunctionMetadata.cpp

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/raw_os_ostream.h"

#include "revng/ADT/GenericGraph.h"
#include "revng/EarlyFunctionAnalysis/FunctionMetadata.h"
#include "revng/Model/Binary.h"

using namespace llvm;

namespace efa {

struct FunctionCFGNodeData {
  FunctionCFGNodeData(MetaAddress Start) : Start(Start) {}
  MetaAddress Start;
};

using FunctionCFGNode = ForwardNode<FunctionCFGNodeData>;

/// Graph data structure to represent the CFG for verification purposes
struct FunctionCFG : public GenericGraph<FunctionCFGNode> {
private:
  MetaAddress Entry;
  std::map<MetaAddress, FunctionCFGNode *> Map;

public:
  FunctionCFG(MetaAddress Entry) : Entry(Entry) {}

public:
  MetaAddress entry() const { return Entry; }
  FunctionCFGNode *entryNode() const { return Map.at(Entry); }

public:
  FunctionCFGNode *get(MetaAddress MA) {
    FunctionCFGNode *Result = nullptr;
    auto It = Map.find(MA);
    if (It == Map.end()) {
      Result = addNode(MA);
      Map[MA] = Result;
    } else {
      Result = It->second;
    }

    return Result;
  }

  bool allNodesAreReachable() const {
    if (Map.size() == 0)
      return true;

    // Ensure all the nodes are reachable from the entry node
    df_iterator_default_set<FunctionCFGNode *> Visited;
    for (auto &Ignore : depth_first_ext(entryNode(), Visited))
      ;
    return Visited.size() == size();
  }

  bool hasOnlyInvalidExits() const {
    for (auto &[Address, Node] : Map)
      if (Address.isValid() and not Node->hasSuccessors())
        return false;
    return true;
  }
};

static FunctionCFG getGraph(const model::Binary &Binary,
                            const SortedVector<efa::BasicBlock> &CFG,
                            MetaAddress Entry) {
  using namespace efa::FunctionEdgeType;

  FunctionCFG Graph(Entry);
  for (const BasicBlock &Block : CFG) {
    auto *Source = Graph.get(Block.Start);

    for (const auto &Edge : Block.Successors) {
      switch (Edge->Type) {
      case DirectBranch:
      case FakeFunctionCall:
      case FakeFunctionReturn:
      case Return:
      case BrokenReturn:
      case IndirectTailCall:
      case LongJmp:
      case Unreachable:
        Source->addSuccessor(Graph.get(Edge->Destination));
        break;

      case FunctionCall:
      case IndirectCall: {
        auto *CE = cast<efa::CallEdge>(Edge.get());
        if (hasAttribute(Binary, *CE, model::FunctionAttribute::NoReturn))
          Source->addSuccessor(Graph.get(MetaAddress::invalid()));
        else
          Source->addSuccessor(Graph.get(Block.End));
        break;
      }

      case Killer:
        Source->addSuccessor(Graph.get(MetaAddress::invalid()));
        break;

      case Invalid:
      case Count:
        revng_abort();
        break;
      }
    }
  }

  return Graph;
}

bool FunctionMetadata::verify(const model::Binary &Binary) const {
  return verify(Binary, false);
}

bool FunctionMetadata::verify(const model::Binary &Binary, bool Assert) const {
  model::VerifyHelper VH(Assert);
  return verify(Binary, VH);
}

bool FunctionMetadata::verify(const model::Binary &Binary,
                              model::VerifyHelper &VH) const {
  const auto &Function = Binary.Functions.at(Entry);

  if (Function.Type == model::FunctionType::Fake
      || Function.Type == model::FunctionType::Invalid)
    return VH.maybeFail(ControlFlowGraph.size() == 0);

  // Populate graph
  FunctionCFG Graph = getGraph(Binary, ControlFlowGraph, Entry);

  // Ensure all the nodes are reachable from the entry node
  if (not Graph.allNodesAreReachable())
    return VH.fail();

  // Ensure the only node with no successors is invalid
  if (not Graph.hasOnlyInvalidExits())
    return VH.fail();

  // Verify blocks
  if (ControlFlowGraph.size() > 0) {
    bool HasEntry = false;
    for (const BasicBlock &Block : ControlFlowGraph) {

      if (Block.Start == Entry) {
        if (HasEntry)
          return VH.fail();
        HasEntry = true;
      }

      for (const auto &Edge : Block.Successors)
        if (not Edge->verify(VH))
          return VH.fail();
    }

    if (not HasEntry) {
      return VH.fail("The function CFG does not contain a block starting at "
                     "the entry point",
                     *this);
    }
  }

  // Check function calls
  for (const auto &Block : ControlFlowGraph) {
    for (const auto &Edge : Block.Successors) {
      if (Edge->Type == efa::FunctionEdgeType::FunctionCall) {
        // We're in a direct call, get the callee
        const auto *Call = dyn_cast<CallEdge>(Edge.get());

        if (not Call->DynamicFunction.empty()) {
          // It's a dynamic call

          if (Call->Destination.isValid()) {
            return VH.fail("Destination must be invalid for dynamic function "
                           "calls");
          }

          auto It = Binary.ImportedDynamicFunctions.find(Call->DynamicFunction);

          // If missing, fail
          if (It == Binary.ImportedDynamicFunctions.end())
            return VH.fail("Can't find callee \"" + Call->DynamicFunction
                           + "\"");
        } else {
          // Regular call
          auto It = Binary.Functions.find(Call->Destination);

          // If missing, fail
          if (It == Binary.Functions.end())
            return VH.fail("Can't find callee");
        }
      }
    }
  }

  return true;
}

void FunctionMetadata::dump() const {
  serialize(dbg, *this);
}

void FunctionMetadata::dumpCFG(const model::Binary &Binary) const {
  FunctionCFG FuncCFG = getGraph(Binary, ControlFlowGraph, Entry);
  raw_os_ostream Stream(dbg);
  WriteGraph(Stream, &FuncCFG);
}

void FunctionEdge::dump() const {
  serialize(dbg, *this);
}

bool FunctionEdge::verify() const {
  return verify(false);
}

bool FunctionEdge::verify(bool Assert) const {
  model::VerifyHelper VH(Assert);
  return verify(VH);
}

static bool
verifyFunctionEdge(model::VerifyHelper &VH, const FunctionEdgeBase &E) {
  using namespace efa::FunctionEdgeType;

  switch (E.Type) {
  case Invalid:
  case Count:
    return VH.fail();

  case DirectBranch:
  case FakeFunctionCall:
  case FakeFunctionReturn:
    if (E.Destination.isInvalid())
      return VH.fail();
    break;
  case FunctionCall: {
    const auto &Call = cast<const CallEdge>(E);
    if (not(E.Destination.isValid() == Call.DynamicFunction.empty()))
      return VH.fail();
  } break;

  case IndirectCall:
  case Return:
  case BrokenReturn:
  case IndirectTailCall:
  case LongJmp:
  case Killer:
  case Unreachable:
    if (E.Destination.isValid())
      return VH.fail();
    break;
  }

  return true;
}

bool FunctionEdgeBase::verify() const {
  return verify(false);
}

bool FunctionEdgeBase::verify(bool Assert) const {
  model::VerifyHelper VH(Assert);
  return verify(VH);
}

bool FunctionEdgeBase::verify(model::VerifyHelper &VH) const {
  if (auto *Edge = dyn_cast<efa::CallEdge>(this))
    return VH.maybeFail(Edge->verify(VH));
  else if (auto *Edge = dyn_cast<efa::FunctionEdge>(this))
    return VH.maybeFail(Edge->verify(VH));
  else
    revng_abort("Invalid FunctionEdgeBase instance");

  return false;
}

void FunctionEdgeBase::dump() const {
  serialize(dbg, *this);
}

bool FunctionEdge::verify(model::VerifyHelper &VH) const {
  if (auto *Call = dyn_cast<efa::CallEdge>(this))
    return VH.maybeFail(Call->verify(VH));
  else
    return verifyFunctionEdge(VH, *this);
}

void CallEdge::dump() const {
  serialize(dbg, *this);
}

bool CallEdge::verify() const {
  return verify(false);
}

bool CallEdge::verify(bool Assert) const {
  model::VerifyHelper VH(Assert);
  return verify(VH);
}

bool CallEdge::verify(model::VerifyHelper &VH) const {
  if (Type == efa::FunctionEdgeType::FunctionCall) {
    // We're in a direct function call (either dynamic or not)
    bool IsDynamic = not DynamicFunction.empty();
    bool HasDestination = Destination.isValid();
    if (not HasDestination and not IsDynamic)
      return VH.fail("Direct call is missing Destination");
    else if (HasDestination and IsDynamic)
      return VH.fail("Dynamic function calls cannot have a valid Destination");
  }

  return VH.maybeFail(verifyFunctionEdge(VH, *this));
}

model::Identifier BasicBlock::name() const {
  using llvm::Twine;
  return model::Identifier(std::string("bb_") + Start.toString());
}

void BasicBlock::dump() const {
  serialize(dbg, *this);
}

bool BasicBlock::verify() const {
  return verify(false);
}

bool BasicBlock::verify(bool Assert) const {
  model::VerifyHelper VH(Assert);
  return verify(VH);
}

bool BasicBlock::verify(model::VerifyHelper &VH) const {
  if (Start.isInvalid() or End.isInvalid())
    return VH.fail();

  for (auto &Edge : Successors)
    if (not Edge->verify(VH))
      return VH.fail();

  return true;
}

} // namespace efa

template<>
struct llvm::DOTGraphTraits<efa::FunctionCFG *> : public DefaultDOTGraphTraits {
  DOTGraphTraits(bool Simple = false) : DefaultDOTGraphTraits(Simple) {}

  static std::string
  getNodeLabel(const efa::FunctionCFGNode *Node, const efa::FunctionCFG *) {
    return Node->Start.toString();
  }

  static std::string getNodeAttributes(const efa::FunctionCFGNode *Node,
                                       const efa::FunctionCFG *Graph) {
    if (Node->Start == Graph->entry()) {
      return "shape=box,peripheries=2";
    }

    return "";
  }
};
