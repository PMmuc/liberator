#include "ConditionExtractor.hpp"
#include "Config.h"

#include "FunctionConditions.hpp"
#include "Profiler.hpp"

#include <Util/SVFUtil.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include "json/json.h"
#include <fstream>
#include <string>

using namespace std;
using namespace SVF;
using namespace SVFUtil;
using namespace LLVMUtil;

#include <llvm/Support/Error.h>
#include <llvm/Support/TimeProfiler.h>

using namespace liberator;

static llvm::cl::opt<std::string>
    InputFilename(cl::Positional, llvm::cl::desc("<input bitcode>"),
                  llvm::cl::init("-"));
static llvm::cl::opt<std::string>
    FunctionName("function", llvm::cl::desc("<function name>"));
static llvm::cl::opt<std::string>
    LibInterface("interface", llvm::cl::desc("<library interface>"));

static llvm::cl::opt<Verbosity> Verbose(
    "v", llvm::cl::desc("<verbose>"), llvm::cl::init(v0),
    llvm::cl::values(clEnumVal(v0, "No verbose"),
                     clEnumVal(v1, "Report ICFG nodes"),
                     clEnumVal(v2, "Report Paths if <debug_condition> is met"),
                     clEnumVal(v3, "To implement, no effect atm")));

static llvm::cl::opt<std::string>
    DebugCondition("debug_condition",
                   llvm::cl::desc("<debug_condition> in combination with v2"));

static llvm::cl::opt<bool>
    DumpSVFG("dump_svfg",
             llvm::cl::desc("Dumps the SVFGGraph of the build library"),
             llvm::cl::init(false));

static llvm::cl::opt<std::string>
    ExtractDataLayout("data_layout", llvm::cl::desc("<datalayout file>"),
                      llvm::cl::init(""));

static llvm::cl::opt<std::string> OutputFile("output",
                                             llvm::cl::desc("<output file>"),
                                             llvm::cl::init("conditions.json"));
static llvm::cl::opt<OutType> OutputType(
    "t", cl::desc("Output type:"), llvm::cl::init(stdo),
    llvm::cl::values(clEnumVal(txt, "Text file <output>"),
                     clEnumVal(json, "Json file <output>"),
                     clEnumVal(stdo, "Standard output, no <output>")));

static llvm::cl::opt<bool> useDominator("dom",
                                        llvm::cl::desc("Use Post/Dominators"),
                                        llvm::cl::init(false));
static llvm::cl::opt<bool>
    printDominator("print_dom", llvm::cl::desc("Print Post/Dominators"),
                   llvm::cl::init(false));

static llvm::cl::opt<std::string>
    cacheFolder("cache_folder", llvm::cl::desc("Folder for cache"),
                llvm::cl::init(""));

static llvm::cl::opt<bool>
    doIndJump("do_indirect_jumps",
              llvm::cl::desc("Include indirect jumps in the analysis"),
              llvm::cl::init(false));

static llvm::cl::opt<std::string>
    minimizeApi("minimize_api", llvm::cl::desc("Minimize API <out_folder>"),
                llvm::cl::init(""));

// There are following log tags: Handler, GEPHandler, APARM
static llvm::cl::list<std::string>
    LogTags("log", llvm::cl::desc("Enable logging for specific tags"),
            llvm::cl::ZeroOrMore, llvm::cl::CommaSeparated);

static llvm::cl::opt<bool>
    EnableProfiling("profiling", llvm::cl::desc("Enable time-trace profiling"),
                    llvm::cl::init(false));

static llvm::cl::opt<int>
    RangeStart("range-start",
               llvm::cl::desc("Start index of the function range to analyze"),
               llvm::cl::init(-1));

static llvm::cl::opt<int>
    RangeEnd("range-end",
             llvm::cl::desc("End index of the function range to analyze"),
             llvm::cl::init(-1));

static llvm::cl::opt<std::string>
    ScanRange("range",
              llvm::cl::desc("Scan a range of functions, e.g., 100-120"),
              llvm::cl::init(""));

Verbosity verbose;

int main(int argc, char **argv) {

  int arg_num = 0;
  std::vector<char *> arg_value(argc);
  std::vector<std::string> moduleNameVec;
  // LLVMUtil::processArguments(argc, argv, arg_num, arg_value.data(),
  //                           moduleNameVec);
  cl::ParseCommandLineOptions(argc, argv,
                              "Extract constraints from functions\n");

  if (EnableProfiling) {
    llvm::timeTraceProfilerInitialize(500, "condition_extractor");
  }

  auto config = config_t::instance();
  config->function = FunctionName;
  config->verbose = Verbose;
  config->consider_indirect_calls = doIndJump;
  config->debug_condition = DebugCondition;
  config->output_type = OutputType;
  config->output_file = OutputFile;
  config->dump_svfg = DumpSVFG;
  config->interface_file = LibInterface;
  config->minimize_api = minimizeApi;
  config->cache_folder = cacheFolder;
  config->input_filename = InputFilename;
  config->print_dominator = printDominator;
  config->use_dominator = useDominator;
  config->extract_data_layout = ExtractDataLayout;
  config->range_start = RangeStart;
  config->range_end = RangeEnd;

  if (!ScanRange.empty()) {
    size_t dash_pos = ScanRange.find('-');
    if (dash_pos != std::string::npos) {
      config->range_start = std::stoi(ScanRange.substr(0, dash_pos));
      config->range_end = std::stoi(ScanRange.substr(dash_pos + 1));
    }
  }

  for (const auto &tag : LogTags) {
    config->log_tags.insert(tag);
  }
  if (config->verbose >= Verbosity::v2) {
    config->debug = true;
    config->debug_condition = DebugCondition;
  }

  moduleNameVec.push_back(config->input_filename);

  if (config->interface_file.empty() && config->function.empty()) {
    SVFUtil::errs()
        << "[ERROR] You should either provide a -libinterface or -function "
           "command line option";
    exit(1);
  }
  // read all the functions from apis_clang.json
  std::set<string> functions;
  if (config->function.empty() && !config->interface_file.empty()) {
    SVFUtil::outs() << "[INFO] Analyze functions in " << config->interface_file
                    << "\n";

    ifstream f(LibInterface);

    Json::Value root;
    Json::Reader reader;

    std::string line;
    while (std::getline(f, line)) {
      // SVFUtil::outs() << line << "\n";
      // FIXME: is this maybe wrong because we read only 1 line here and
      // reader.parse expects the whole json document
      bool parsingSuccessful = reader.parse(line.c_str(), root);
      if (!parsingSuccessful) {
        SVFUtil::errs() << "Failed to parse "
                        << reader.getFormattedErrorMessages();
        exit(1);
      }

      // some clang functions are the result of macro expansion
      // they are not present in the llvm module
      std::string function_name = root["function_name"].asString();
      functions.insert(function_name);
    }

    f.close();
  } else {
    SVFUtil::outs() << "[INFO] analyzing function: " << config->function
                    << "\n";
    functions.insert(config->function);
  }

  // exit(1);

  // Function -> SVFVar

  // SVFUtil::outs() << " === EXIT FOR DEBUG ===\n";
  // exit(1);
  //

  auto extractor = make_condition_extractor(moduleNameVec, functions);
  if (!extractor)
    return 1;

  auto conds = extractor->extract_function_conditions();
  SVFUtil::outs() << get_summary(conds);
  save_condition_set(conds, config->output_type);

  if (config->extract_data_layout != "") {
    save_llvm_data_layout();
  }

  if (EnableProfiling) {
    if (auto E = llvm::timeTraceProfilerWrite("time_trace.json", "")) {
      SVFUtil::errs() << llvm::toString(std::move(E)) << "\n";
    }
    llvm::timeTraceProfilerCleanup();
  }

  SVFUtil::outs() << "\n" << liberator::Profiler::instance().dump() << "\n";

  return 0;
}
