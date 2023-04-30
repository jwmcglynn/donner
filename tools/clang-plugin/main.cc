// Declares clang::SyntaxOnlyAction.
#include <clang/Frontend/FrontendActions.h>

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
// Declares llvm::cl::extrahelp.
#include "llvm/Support/CommandLine.h"

using namespace clang::tooling;
using namespace llvm;

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static llvm::cl::OptionCategory MyToolCategory("my-tool options");

// CommonOptionsParser declares HelpMessage with a description of the common
// command-line options related to the compilation database and input files.
// It's nice to have this help message in all tools.
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// A help message for this specific tool can be added afterwards.
static cl::extrahelp MoreHelp("\nMore help text...\n");

template <typename T>
std::unique_ptr<FrontendActionFactory> myNewFrontendActionFactory() {
  class SimpleFrontendActionFactory : public FrontendActionFactory {
  public:
    std::unique_ptr<clang::FrontendAction> create() override { return std::make_unique<T>(); }
  };

  return std::unique_ptr<FrontendActionFactory>(new SimpleFrontendActionFactory);
}

int main(int argc, const char** argv) {
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
  if (!ExpectedParser) {
    // Fail gracefully for unsupported options.
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser& OptionsParser = ExpectedParser.get();
  ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
  return Tool.run(myNewFrontendActionFactory<clang::SyntaxOnlyAction>().get());
  // return 0;
}
