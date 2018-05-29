/// \file dump.cpp
/// \brief Standalone program to extract various information from the LLVM IR
///        generated by revamb

// Standard includes
#include <cstdlib>
#include <iostream>
#include <memory>

// LLVM includes
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"

// Local includes
#include "argparse.h"
#include "collectcfg.h"
#include "collectfunctionboundaries.h"
#include "collectnoreturn.h"
#include "debug.h"
#include "debughelper.h"
#include "isolatefunctions.h"
#include "stackanalysis.h"

using namespace llvm;

struct ProgramParameters {
  const char *InputPath;
  const char *CFGPath;
  const char *NoreturnPath;
  const char *FunctionBoundariesPath;
  const char *StackAnalysisPath;
  const char *FunctionIsolationPath;
};

static const char *const Usage[] = {
  "revamb-dump [options] INFILE",
  nullptr,
};

static bool parseArgs(int Argc, const char *Argv[], ProgramParameters &Result) {
  // Initialize argument parser
  const char *DebugLoggingString = nullptr;
  struct argparse Arguments;
  struct argparse_option Options[] = {
    OPT_HELP(),
    OPT_STRING('d', "debug",
               &DebugLoggingString,
               "enable verbose logging."),
    OPT_STRING('c', "cfg",
               &Result.CFGPath,
               "path where the CFG should be stored."),
    OPT_STRING('n', "noreturn",
               &Result.NoreturnPath,
               "path where the list of noreturn basic blocks should be "
               "stored."),
    OPT_STRING('f', "functions-boundaries",
               &Result.FunctionBoundariesPath,
               "path where the list of function boundaries blocks should be "
               "stored."),
    OPT_STRING('s', "stack-analysis",
               &Result.StackAnalysisPath,
               "path where the result of the stack analysis should be stored."),
    OPT_STRING('i', "functions-isolation",
               &Result.FunctionIsolationPath,
               "path where a new LLVM module containing the reorganization of "
               "the basic blocks into the corresponding functions identified "
               "by function boundaries analysis performed by revamb should be "
               "stored."),
    OPT_END(),
  };

  argparse_init(&Arguments, Options, Usage, 0);
  argparse_describe(&Arguments, "\nrevamb-dump.",
                    "\nDump several high-level information from the "
                    "revamb-generated LLVM IR.\n");
  Argc = argparse_parse(&Arguments, Argc, Argv);

  if (DebugLoggingString != nullptr) {
    DebuggingEnabled = true;
    std::string Input(DebugLoggingString);
    std::stringstream Stream(Input);
    std::string Type;
    while (std::getline(Stream, Type, ','))
      enableDebugFeature(Type.c_str());
  }

  // Handle positional arguments
  if (Argc != 1) {
    fprintf(stderr, "Please specify one and only one input file.\n");
    return false;
  }

  Result.InputPath = Argv[0];

  return true;
}

class DumpPass : public FunctionPass {
public:
  static char ID;

public:
  DumpPass(ProgramParameters &Parameters) : FunctionPass(ID),
                                            Parameters(Parameters) { }

  bool runOnFunction(Function &F) override {
    std::ofstream Output;

    if (Parameters.CFGPath != nullptr) {
      auto &Analysis = getAnalysis<CollectCFG>();
      Analysis.serialize(pathToStream(Parameters.CFGPath, Output));
    }

    if (Parameters.NoreturnPath != nullptr) {
      auto &Analysis = getAnalysis<CollectNoreturn>();
      Analysis.serialize(pathToStream(Parameters.NoreturnPath, Output));
    }

    if (Parameters.FunctionBoundariesPath != nullptr) {
      auto &Analysis = getAnalysis<CollectFunctionBoundaries>();
      Analysis.serialize(pathToStream(Parameters.FunctionBoundariesPath,
                                      Output));
    }

    if (Parameters.StackAnalysisPath != nullptr) {
      auto &Analysis = getAnalysis<StackAnalysis::StackAnalysis>();
      Analysis.serialize(pathToStream(Parameters.StackAnalysisPath, Output));
    }

    if (Parameters.FunctionIsolationPath != nullptr) {
      auto &Analysis = getAnalysis<IsolateFunctions>();
      Module *ModifiedModule = Analysis.getModule();
      dumpModule(ModifiedModule, Parameters.FunctionIsolationPath);
    }

    return false;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();

    if (Parameters.CFGPath != nullptr)
      AU.addRequired<CollectCFG>();

    if (Parameters.NoreturnPath != nullptr)
      AU.addRequired<CollectNoreturn>();

    if (Parameters.FunctionBoundariesPath != nullptr)
      AU.addRequired<CollectFunctionBoundaries>();

    if (Parameters.StackAnalysisPath != nullptr)
      AU.addRequired<StackAnalysis::StackAnalysis>();

    if (Parameters.FunctionIsolationPath != nullptr)
      AU.addRequired<IsolateFunctions>();

  }

private:
  std::ostream &pathToStream(const char *Path, std::ofstream &File) {
    if (Path[0] == '-' && Path[1] == '\0') {
      return std::cout;
    } else {
      if (File.is_open())
        File.close();
      File.open(Path);
      return File;
    }
  }

  void dumpModule(Module *Module, const char *Path) {
    std::ofstream Output;

    // If output path is `-` print on stdout
    // TODO: this solution is not portable, make DebugHelper accept streams
    if (Path[0] == '-' && Path[1] == '\0') {
      Path = "/dev/stdout";
    }

    // Initialize the debug helper object
    DebugHelper Debug(Path, Path, Module, DebugInfoType::LLVMIR);
    Debug.generateDebugInfo();
  }

private:
  ProgramParameters &Parameters;
};

char DumpPass::ID = 0;

int main(int argc, const char *argv[]) {
  ProgramParameters Parameters = { nullptr, nullptr };

  if (!parseArgs(argc, argv, Parameters))
    return EXIT_FAILURE;

  LLVMContext &Context = getGlobalContext();
  SMDiagnostic Err;
  std::unique_ptr<Module> TheModule = parseIRFile(Parameters.InputPath,
                                                  Err,
                                                  Context);

  if (!TheModule) {
    fprintf(stderr, "Couldn't load the LLVM IR.");
    return EXIT_FAILURE;
  }

  legacy::FunctionPassManager FPM(TheModule.get());
  FPM.add(new DumpPass(Parameters));
  FPM.run(*TheModule->getFunction("root"));

  return EXIT_SUCCESS;
}