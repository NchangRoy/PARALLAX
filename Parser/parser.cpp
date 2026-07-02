#include <iostream>
#include <sstream>
#include <fstream>
#include <climits>

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <set>
#include <string>
using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
class MyASTConsumer;
class MyFrontEndAction;
class Parser;
class PrintHandler;

class MetaData {
public:
  // source_name → dispatch_stub_name  (used by CallExprHandler for call-site
  // replacement)
  std::map<std::string, std::string> transformedFuncs;
  // source_name → worker_stub_name    (used by matcher on the worker side)
  std::map<std::string, std::string> workerFuncs;
  // dispatch_stub_name → full C prototype string (for forward declarations)
  std::map<std::string, std::string> dispatchPrototypes;
  // unique aggregator names used
  std::set<std::string> aggregators;

  // source_name → full worker stub C source (for __parallax_prog_code__)
  std::map<std::string, std::string> workerStubTexts;
  // source_name → source function body C source (for __parallax_prog_code__)
  std::map<std::string, std::string> sourceFuncTexts;

  void addTranformedFunct(std::string source, std::string transformed) {
    transformedFuncs[source] = transformed;
  }

  void addWorkerFunct(std::string source, std::string worker) {
    workerFuncs[source] = worker;
  }

  void addWorkerStubText(std::string source, std::string stubText) {
    workerStubTexts[source] = stubText;
  }

  void addSourceFuncText(std::string source, std::string funcText) {
    sourceFuncTexts[source] = funcText;
  }

  void addDispatchPrototype(std::string dispatchName, std::string proto) {
    dispatchPrototypes[dispatchName] = proto;
  }

  void printAllTransformedFunctions() {
    for (auto &func : transformedFuncs) {
      llvm::outs() << "\n\nTransformed function " << func.first
                   << " into dispatch stub " << func.second << "\n\n";
    }
    for (auto &func : workerFuncs) {
      llvm::outs() << "Worker stub: " << func.second << "\n";
    }
  }
};

// ── Amalgamation helpers ────────────────────────────────────────────────────

static std::string getDirname(const std::string &path) {
  size_t pos = path.rfind('/');
  return pos != std::string::npos ? path.substr(0, pos) : ".";
}

// Recursively inline local #include "..." files into a single string.
// System headers (#include <...>) are left as-is.
// parallax/ headers are left as-is (available on every worker node).
// seen prevents double-inlining (soft include guard).
static std::string inlineLocalIncludes(const std::string &filePath,
                                       std::set<std::string> &seen) {
  char resolved[PATH_MAX];
  if (!realpath(filePath.c_str(), resolved))
    return "";
  std::string canonical(resolved);
  if (!seen.insert(canonical).second)
    return ""; // already inlined

  std::ifstream f(canonical);
  if (!f.is_open())
    return "";

  std::string result;
  std::string line;
  std::string dir = getDirname(canonical);

  while (std::getline(f, line)) {
    std::string trimmed = line;
    size_t s = trimmed.find_first_not_of(" \t");
    if (s != std::string::npos)
      trimmed = trimmed.substr(s);

    if (trimmed.size() > 10 && trimmed.compare(0, 9, "#include ") == 0 &&
        trimmed[9] == '"') {
      size_t end = trimmed.find('"', 10);
      if (end != std::string::npos) {
        std::string incFile = trimmed.substr(10, end - 10);
        if (incFile.find("parallax/") == 0) {
          result += line + "\n"; // keep parallax headers as-is
        } else {
          result += inlineLocalIncludes(dir + "/" + incFile, seen);
        }
        continue;
      }
    }
    result += line + "\n";
  }
  return result;
}

// ───────────────────────────────────────────────────────────────────────────

class PrintHandler : public MatchFinder::MatchCallback {
private:
  Rewriter &rewriter;
  MetaData &metadata;

public:
  PrintHandler(Rewriter &rewriter, MetaData &MetaData)
      : rewriter(rewriter), metadata(MetaData) {}

  void run(const MatchFinder::MatchResult &Result) override {
    const FunctionDecl *func = Result.Nodes.getNodeAs<FunctionDecl>("func");
    if (!func)
      return;
  }
};

class VarHandler : public MatchFinder::MatchCallback {
  Rewriter &rewriter;
  MetaData &metadata;

public:
  VarHandler(Rewriter &rewriter, MetaData &metadata)
      : rewriter(rewriter), metadata(metadata) {}

  void run(const MatchFinder::MatchResult &Result) override {
    const VarDecl *var = Result.Nodes.getNodeAs<VarDecl>("var");
    if (!var)
      return;

    for (const Attr *A : var->attrs()) {
      if (const auto *AA = dyn_cast<AnnotateAttr>(A)) {
        llvm::outs() << "Annotation: " << AA->getAnnotation() << " at variable "
                     << var->getNameAsString();
      }
    }
  }
};

class CallExprHandler : public MatchFinder::MatchCallback {
  Rewriter &rewriter;
  MetaData &metadata;

public:
  CallExprHandler(Rewriter &rewriter, MetaData &metadata)
      : rewriter(rewriter), metadata(metadata) {}

  void run(const MatchFinder::MatchResult &Result) override {
    const CallExpr *call = Result.Nodes.getNodeAs<CallExpr>("call");
    if (!call)
      return;

    const FunctionDecl *FD = call->getDirectCallee();
    if (!FD)
      return;

    std::string name = FD->getNameAsString();
    auto it = metadata.transformedFuncs.find(name);
    if (it == metadata.transformedFuncs.end())
      return;

    const std::string &newName = it->second;
    const Expr *callee = call->getCallee();
    SourceRange calleeRange(callee->getBeginLoc(), callee->getEndLoc());
    rewriter.ReplaceText(calleeRange, newName);
  }
};

class FunctionHandler : public MatchFinder::MatchCallback {
  Rewriter &rewriter;
  MetaData &metadata;

public:
  FunctionHandler(Rewriter &rewriter, MetaData &metadata)
      : rewriter(rewriter), metadata(metadata) {}

  void run(const MatchFinder::MatchResult &Result) override {
    const FunctionDecl *func = Result.Nodes.getNodeAs<FunctionDecl>("func");
    if (!func)
      return;

    int vcpus = 0; /* 0 = auto — master caps at PARALLAX_DEFAULT_NODE_CAP */
    std::string aggregator = "sum_reduce";
    bool has_annotation = false;

    for (const Attr *A : func->attrs()) {
      if (const auto *AA = dyn_cast<AnnotateAttr>(A)) {
        std::string annotation = AA->getAnnotation().str();
        llvm::outs() << "Annotation: " << annotation << " at function "
                     << func->getNameAsString() << "\n";
        if (annotation.find("vcpus:") == 0) {
          vcpus = std::stoi(annotation.substr(6));
          has_annotation = true;
        } else if (annotation.find("reduce:") == 0) {
          aggregator = annotation.substr(7);
          has_annotation = true;
        } else if (annotation.find("goat:") == 0) {
          has_annotation = true;
        }
      }
    }

    if (!has_annotation)
      return;

    std::string sourceFunction = func->getNameAsString();
    std::string returnType = func->getReturnType().getAsString();
    std::string dispatchName =
        sourceFunction + "_generated";                   // replaces call sites
    std::string workerName = sourceFunction + "_worker"; // runs on worker node

    // ── Build original parameter signature string ──────────────────────────
    std::string params;
    for (unsigned i = 0; i < func->getNumParams(); ++i) {
      const ParmVarDecl *P = func->getParamDecl(i);
      params += P->getType().getAsString() + " " + P->getNameAsString();
      if (i + 1 < func->getNumParams())
        params += ", ";
    }
    std::string paramCount = std::to_string(func->getNumParams());

    // ── Look-ahead pass: find the "size companion" for each pointer param ──
    std::vector<int> sizeCompanion(func->getNumParams(), -1);
    for (unsigned i = 0; i + 1 < func->getNumParams(); ++i) {
      const ParmVarDecl *P = func->getParamDecl(i);
      const ParmVarDecl *Next = func->getParamDecl(i + 1);
      if (P->getType()->isPointerType() && Next->getType()->isIntegerType()) {
        sizeCompanion[i] = (int)(i + 1);
      }
    }

    // ── 1. DISPATCH STUB (_generated) ─────────────────────────────────────
    std::string dispatchStub = "\n" + returnType + " " + dispatchName + "(" +
                               params +
                               ") {\n"
                               "    ParallaxParam __parallax_params[" +
                               paramCount + "];\n";

    bool firstScatterDone = false;
    for (unsigned i = 0; i < func->getNumParams(); ++i) {
      const ParmVarDecl *P = func->getParamDecl(i);
      std::string pName = P->getNameAsString();
      std::string pType = P->getType().getAsString();
      std::string idx = std::to_string(i);
      bool isPtr = P->getType()->isPointerType();

      std::string dist;
      if (isPtr && !firstScatterDone) {
        dist = "PARALLAX_SCATTER";
        firstScatterDone = true;
      } else if (!isPtr && i > 0 && sizeCompanion[i - 1] == (int)i) {
        dist = "PARALLAX_SIZE_OF";
      } else {
        dist = "PARALLAX_BROADCAST";
      }

      dispatchStub += "    __parallax_params[" + idx + "].data = (void *)";
      dispatchStub += (isPtr ? pName : ("&" + pName)) + ";\n";

      dispatchStub += "    __parallax_params[" + idx + "].size = ";
      if (isPtr) {
        if (sizeCompanion[i] >= 0) {
          dispatchStub +=
              func->getParamDecl(sizeCompanion[i])->getNameAsString() + ";\n";
        } else {
          dispatchStub += "0;\n";
        }
      } else {
        dispatchStub += "sizeof(" + pName + ");\n";
      }

      dispatchStub +=
          "    __parallax_params[" + idx + "].distribution = " + dist + ";\n";
      dispatchStub +=
          "    __parallax_params[" + idx + "].index = " + idx + ";\n";
      dispatchStub += "    strncpy(__parallax_params[" + idx +
                      "].type_name, \"" + pType + "\", 63);\n";
    }

    dispatchStub += "    ParallaxExecutionCtx __parallax_ctx;\n"
                    "    __parallax_ctx.expected_node_count = " +
                    std::to_string(vcpus) +
                    ";\n"
                    "    strncpy(__parallax_ctx.aggregator_name, \"" +
                    aggregator +
                    "\", 63);\n"
                    "    __parallax_ctx.aggregator_name[63] = '\\0';\n"
                    "    execute_fxn(__parallax_params, " +
                    paramCount + ", \"" + workerName +
                    "\", &__parallax_ctx, __parallax_prog_code__, "
                    "__parallax_prog_name__);\n";

    // Fix 3: correct return value for all return-type categories
    if (returnType != "void") {
      if (func->getReturnType()->isPointerType())
        dispatchStub += "    return NULL;\n";
      else if (func->getReturnType()->isIntegerType() ||
               func->getReturnType()->isFloatingType())
        dispatchStub += "    return (" + returnType + ")0;\n";
      else
        dispatchStub += "    return (" + returnType + "){0};\n";
    }
    dispatchStub += "}\n";

    // ── 2. WORKER STUB (_worker) ───────────────────────────────────────────
    std::string workerStub =
        "\nvoid *" + workerName +
        "(void *__arg) {\n"
        "    ParallaxParam *__p = (ParallaxParam *)__arg;\n";

    std::string callArgs;
    for (unsigned i = 0; i < func->getNumParams(); ++i) {
      const ParmVarDecl *P = func->getParamDecl(i);
      std::string pName = P->getNameAsString();
      std::string pType = P->getType().getAsString();
      std::string idx = std::to_string(i);
      bool isPtr = P->getType()->isPointerType();

      if (isPtr) {
        workerStub += "    " + pType + " " + pName + " = (" + pType + ")__p[" +
                      idx + "].data;\n";
      } else {
        workerStub += "    " + pType + " " + pName + " = *(" + pType +
                      " *)__p[" + idx + "].data;\n";
      }

      if (i > 0)
        callArgs += ", ";
      callArgs += pName;
    }

    if (returnType == "void") {
      workerStub += "    " + sourceFunction + "(" + callArgs + ");\n";
      workerStub += "    return NULL;\n";
    } else {
      workerStub +=
          "    return (void *)" + sourceFunction + "(" + callArgs + ");\n";
    }
    workerStub += "}\n";

    // Fix 4: only dispatch stub goes into master binary.
    // Worker stub lives only in __parallax_prog_code__ — workers compile their
    // own binary, master never calls the worker stub directly.
    clang::SourceLocation endOfFile =
        rewriter.getSourceMgr().getLocForEndOfFile(
            rewriter.getSourceMgr().getMainFileID());
    rewriter.InsertText(endOfFile, dispatchStub, true, true);

    metadata.addTranformedFunct(sourceFunction, dispatchName);
    metadata.addWorkerFunct(sourceFunction, workerName);
    metadata.addWorkerStubText(sourceFunction, workerStub);
    metadata.aggregators.insert(aggregator);
    metadata.addDispatchPrototype(
        dispatchName, returnType + " " + dispatchName + "(" + params + ");");

    // Fix 2: capture CLEAN source function text (no Parallax annotations) for
    // __parallax_prog_code__. Reconstruct from AST components instead of raw
    // source text so __attribute__((annotate(...))) is never embedded in the
    // code shipped to workers.
    const Stmt *body = func->getBody();
    if (body) {
      SourceManager &SM = rewriter.getSourceMgr();
      const LangOptions &LO = rewriter.getLangOpts();
      SourceRange bodyRange = body->getSourceRange();
      clang::SourceLocation bodyEnd =
          clang::Lexer::getLocForEndOfToken(bodyRange.getEnd(), 0, SM, LO);
      bool invalid = false;
      StringRef bodyText = clang::Lexer::getSourceText(
          clang::CharSourceRange::getCharRange(bodyRange.getBegin(), bodyEnd),
          SM, LO, &invalid);
      if (!invalid) {
        std::string cleanFunc =
            returnType + " " + sourceFunction + "(" + params + ") " +
            bodyText.str();
        metadata.addSourceFuncText(sourceFunction, cleanFunc);
      }
    }
  }
};

// Escape a raw C string so it can be embedded as a C string literal body.
static std::string escapeForCString(const std::string &s) {
  std::string out;
  out.reserve(s.size() * 2);
  for (unsigned char c : s) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20 || c == 0x7f) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\x%02x", c);
        out += buf;
      } else {
        out += (char)c;
      }
    }
  }
  return out;
}

class MyASTConsumer : public ASTConsumer {
private:
  MetaData &MymetaData;
  Rewriter &MyRewriter;
  PrintHandler printhandler;
  VarHandler varHandler;
  FunctionHandler functionHandler;
  CallExprHandler callExprHandler;
  MatchFinder FunctionMatcher;
  MatchFinder VariableMatcher;
  MatchFinder CallExprMatcher;

public:
  MyASTConsumer(Rewriter &rewriter, MetaData &metadata)
      : MyRewriter(rewriter), MymetaData(metadata),
        printhandler(this->MyRewriter, this->MymetaData),
        varHandler(this->MyRewriter, this->MymetaData),
        functionHandler(this->MyRewriter, this->MymetaData),
        callExprHandler(this->MyRewriter, this->MymetaData) {

    FunctionMatcher.addMatcher(functionDecl(isDefinition()).bind("func"),
                               &printhandler);
    VariableMatcher.addMatcher(varDecl().bind("var"), &varHandler);
    FunctionMatcher.addMatcher(functionDecl(isDefinition()).bind("func"),
                               &functionHandler);
    CallExprMatcher.addMatcher(callExpr().bind("call"), &callExprHandler);
  }

  void HandleTranslationUnit(ASTContext &context) override {
    FunctionMatcher.matchAST(context);
    VariableMatcher.matchAST(context); // Fix 1: was registered but never invoked
    MymetaData.printAllTransformedFunctions();
    CallExprMatcher.matchAST(context);

    // Fix 4: master's matcher only needs aggregator functions.
    // Worker stubs are resolved on the worker side via their own matcher
    // (inside __parallax_prog_code__). Master uses matcher solely to look up
    // the user-supplied reduce function by name at aggregation time.
    std::string matcherCode = "\n\ntypedef void *(*fn)(void *);\n\n";
    matcherCode += "fn matcher(char *name) {\n";
    for (const auto &agg : MymetaData.aggregators) {
      if (agg != "sum_reduce" && !agg.empty()) {
        matcherCode += "    if (strcmp(name, \"" + agg + "\") == 0) {\n";
        matcherCode += "        return (fn)" + agg + ";\n";
        matcherCode += "    }\n";
      }
    }
    matcherCode += "    return NULL;\n";
    matcherCode += "}\n";

    clang::SourceLocation endOfFile =
        MyRewriter.getSourceMgr().getLocForEndOfFile(
            MyRewriter.getSourceMgr().getMainFileID());
    MyRewriter.InsertText(endOfFile, matcherCode, true, true);
  }
};

class MyFrontEndAction : public ASTFrontendAction {
private:
  Rewriter MyRewriter;
  MetaData metaData;

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef file) override {
    MyRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<MyASTConsumer>(MyRewriter, metaData);
  }

  void EndSourceFileAction() override {
    SourceManager &SM = MyRewriter.getSourceMgr();

    std::string originalFile =
        SM.getFileEntryForID(SM.getMainFileID())->getName().str();

    // 1. Capture the full rewritten buffer into a std::string
    std::string rewrittenCode;
    llvm::raw_string_ostream codeStream(rewrittenCode);
    MyRewriter.getEditBuffer(SM.getMainFileID()).write(codeStream);
    codeStream.flush();

    // 2. Derive prog_name from the source filename (basename, no extension)
    std::string progName = originalFile;
    size_t slashPos = progName.rfind('/');
    if (slashPos != std::string::npos)
      progName = progName.substr(slashPos + 1);
    size_t dotPos = progName.rfind('.');
    if (dotPos != std::string::npos)
      progName = progName.substr(0, dotPos);

    // 3. Build a MINIMAL worker-only code string.
    //    Workers only need: computation functions, _worker stubs, matcher.
    //    They must NOT get master-side dispatch stubs or references to
    //    __parallax_prog_code__/__parallax_prog_name__ (undefined on worker).
    //
    //    Fix 5: inline local #include "..." files from the original source so
    //    workers have all user-defined types and helper functions available
    //    when they compile the embedded code with gcc.
    std::set<std::string> inlinedFiles;
    std::string localIncludeCode;
    std::string extraSystemHeaders;
    // These three are always emitted in the hardcoded defaults below.
    std::set<std::string> defaultSysHeaders = {"<stdio.h>", "<stdlib.h>", "<string.h>"};
    {
      std::string dir = getDirname(originalFile);
      std::ifstream origSrc(originalFile);
      std::string srcLine;
      while (std::getline(origSrc, srcLine)) {
        std::string trimmed = srcLine;
        size_t s = trimmed.find_first_not_of(" \t");
        if (s != std::string::npos)
          trimmed = trimmed.substr(s);
        if (trimmed.size() > 10 && trimmed.compare(0, 9, "#include ") == 0) {
          if (trimmed[9] == '<') {
            // System header: forward to worker unless already in defaults.
            size_t end = trimmed.find('>');
            if (end != std::string::npos) {
              std::string hdr = trimmed.substr(9, end - 8);
              if (defaultSysHeaders.find(hdr) == defaultSysHeaders.end())
                extraSystemHeaders += srcLine + "\n";
            }
          } else if (trimmed[9] == '"') {
            size_t end = trimmed.find('"', 10);
            if (end != std::string::npos) {
              std::string incFile = trimmed.substr(10, end - 10);
              if (incFile.find("parallax/") == 0) {
                localIncludeCode += srcLine + "\n";
              } else {
                localIncludeCode +=
                    inlineLocalIncludes(dir + "/" + incFile, inlinedFiles);
              }
            }
          }
        }
      }
    }

    std::string workerOnlyCode;
    workerOnlyCode += "#include <stdio.h>\n";
    workerOnlyCode += "#include <stdlib.h>\n";
    workerOnlyCode += "#include <string.h>\n";
    workerOnlyCode += "#include \"parallax/parallax_param.h\"\n";
    if (!extraSystemHeaders.empty())
      workerOnlyCode += extraSystemHeaders;
    if (!localIncludeCode.empty())
      workerOnlyCode += localIncludeCode;
    workerOnlyCode += "\n";

    for (const auto &pair : metaData.workerFuncs) {
      const std::string &srcName = pair.first;

      auto srcIt = metaData.sourceFuncTexts.find(srcName);
      if (srcIt != metaData.sourceFuncTexts.end())
        workerOnlyCode += srcIt->second + "\n\n";

      auto wrkIt = metaData.workerStubTexts.find(srcName);
      if (wrkIt != metaData.workerStubTexts.end())
        workerOnlyCode += wrkIt->second + "\n\n";
    }

    // Worker's matcher: maps worker stub names to function pointers
    workerOnlyCode += "\ntypedef void *(*fn)(void *);\n\n";
    workerOnlyCode += "fn matcher(char *name) {\n";
    for (const auto &pair : metaData.workerFuncs) {
      workerOnlyCode += "    if (strcmp(name, \"" + pair.second + "\") == 0) {\n";
      workerOnlyCode += "        return (fn)" + pair.second + ";\n";
      workerOnlyCode += "    }\n";
    }
    workerOnlyCode += "    return NULL;\n";
    workerOnlyCode += "}\n";
    workerOnlyCode += "\nint main() { return 0; }\n";

    std::string escaped = escapeForCString(workerOnlyCode);

    // 4. Forward declarations for master-side output.
    //    Fix 4: only dispatch stubs — worker stubs are NOT in the master binary.
    std::string fwdDecls;
    for (const auto &pair : metaData.dispatchPrototypes) {
      fwdDecls += pair.second + "\n";
    }

    std::string header =
        "/* === Parallax: embedded program source (auto-generated) === */\n"
        "#include <string.h>\n"
        "#include \"parallax/parallax_param.h\"\n"
        "extern void execute_fxn(ParallaxParam *, int, char *, "
        "ParallaxExecutionCtx *, const char *, const char *);\n" +
        fwdDecls +
        "static const char *__parallax_prog_code__ = \"" + escaped + "\";\n"
        "static const char *__parallax_prog_name__ = \"" + progName + "\";\n\n";

    // 5. Write: header globals first, then the full rewritten source
    std::string outputFile = originalFile + "_parsed.c";
    std::error_code EC;
    llvm::raw_fd_ostream outFile(outputFile, EC, llvm::sys::fs::OF_Text);
    if (EC) {
      llvm::errs() << "Cannot open output file: " << EC.message() << "\n";
      return;
    }

    outFile << header;
    outFile << rewrittenCode;

    llvm::outs() << "Written rewritten file to: " << outputFile << "\n";
  }
};

class Parser {
public:
  Parser() {}

  int init(int argc, const char **argv) {
    static llvm::cl::OptionCategory ToolCategory("ast-tool");
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
    if (!ExpectedParser) {
      llvm::errs() << ExpectedParser.takeError();
      return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    ClangTool Tool(OptionsParser.getCompilations(),
                   OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<MyFrontEndAction>().get());
  }
};

int main(int argc, const char **argv) {
  Parser parser;
  parser.init(argc, argv);
}
