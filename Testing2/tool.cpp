#include <iostream>

#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <clang/Frontend/FrontendActions.h>

#include <clang/AST/AST.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/AST/Decl.h>

#include <llvm/Support/CommandLine.h>

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;

// ---------------- Callback ----------------
class MyPrintFunction : public MatchFinder::MatchCallback {
public:
    void run(const MatchFinder::MatchResult &Result) override {

        const FunctionDecl *func =
            Result.Nodes.getNodeAs<FunctionDecl>("func");

        if (!func)
            return;

        std::cout << "Function Name: "
                  << func->getNameAsString()
                  << "\n";
    }
    
};

// ---------------- AST Consumer ----------------
class MyASTConsumer : public ASTConsumer {
    MatchFinder matcher;
    MyPrintFunction printer;

public:
    MyASTConsumer() {
        matcher.addMatcher(
            functionDecl(isDefinition()).bind("func"),
            &printer
        );
    }

    void HandleTranslationUnit(ASTContext &context) override {
        matcher.matchAST(context);
    }
};

// ---------------- Frontend Action ----------------
class MyFrontendAction : public ASTFrontendAction {
public:
    std::unique_ptr<ASTConsumer>
    CreateASTConsumer(CompilerInstance &CI, StringRef file) override {
        return std::make_unique<MyASTConsumer>();
    }
};

// ---------------- CLI ----------------
static llvm::cl::OptionCategory ToolCategory("my-tool options");

int main(int argc, const char **argv) {

    auto ExpectedParser =
        CommonOptionsParser::create(argc, argv, ToolCategory);

    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }

    CommonOptionsParser &OptionsParser = ExpectedParser.get();

    ClangTool Tool(
        OptionsParser.getCompilations(),
        OptionsParser.getSourcePathList()
    );

    return Tool.run(
        newFrontendActionFactory<MyFrontendAction>().get()
    );
}