#include <iostream>

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/CompilerInstance.h"
using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;


// =======>========== Callback =================
class FunctionRenamer : public MatchFinder::MatchCallback {
public:
    
    FunctionRenamer(Rewriter &R) : TheRewriter(R) {}

    void run(const MatchFinder::MatchResult &Result) override {

        const FunctionDecl *func =
            Result.Nodes.getNodeAs<FunctionDecl>("func");

        if (!func || !func->hasBody())
            return;

        SourceManager &SM = *Result.SourceManager;

        // function name location
        SourceLocation loc = func->getLocation();

        if (loc.isInvalid() || loc.isMacroID())
            return;

        std::string oldName = func->getNameAsString();
        std::string newName = "patched_" + oldName;

        // replace function name in source
        TheRewriter.ReplaceText(
            loc,
            oldName.length(),
            newName
        );

        llvm::outs() << "Renamed: " << oldName << " -> " << newName << "\n";
    }

private:
    Rewriter &TheRewriter;
};

// ================= AST Consumer =================
class MyASTConsumer : public ASTConsumer {
public:
    MyASTConsumer(Rewriter &R) : Handler(R) {

        Matcher.addMatcher(
            functionDecl(isDefinition()).bind("func"),
            &Handler
        );
    }
    
    void HandleTranslationUnit(ASTContext &Context) override {
        Matcher.matchAST(Context);
    }

private:
    FunctionRenamer Handler;
    MatchFinder Matcher;
};

// ================= Frontend Action =================
class MyFrontendAction : public ASTFrontendAction {
public:
    std::unique_ptr<ASTConsumer>
    CreateASTConsumer(CompilerInstance &CI, StringRef file) override {

        TheRewriter.setSourceMgr(
            CI.getSourceManager(),
            CI.getLangOpts()
        );

        return std::make_unique<MyASTConsumer>(TheRewriter);
    }

    void EndSourceFileAction() override {
        TheRewriter.getEditBuffer(
            TheRewriter.getSourceMgr().getMainFileID()
        ).write(llvm::outs());
    }

private:
    Rewriter TheRewriter;
};

// ================= Main =================
static llvm::cl::OptionCategory ToolCategory("ast-tool");

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