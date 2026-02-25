#include "FunctionConditions.hpp"

namespace liberator {

Json::Value FunctionConditions::toJson(bool verbose) {
  Json::Value functionResult;

  functionResult["function_name"] = function_name;

  int pn = 0;
  for (auto param : parameter_metadata) {
    auto param_key = "param_" + std::to_string(pn);
    functionResult[param_key] = param.toJson(verbose);
    pn++;
    std::vector<ValueMetadata> parameter_metadata;
  }

  functionResult["return"] = return_metadata.toJson(verbose);

  return functionResult;
}

std::string FunctionConditions::toString(bool verbose) {

  std::stringstream sstream;

  sstream << "Function: " << function_name << "\n";

  int pn = 0;
  for (auto param : parameter_metadata) {
    sstream << "param_" + std::to_string(pn) << ":\n";
    sstream << param.toString(verbose);
    pn++;
  }

  sstream << "return:\n";
  sstream << return_metadata.toString(verbose);

  return sstream.str();
}
std::string FunctionConditions::getSummary() {
  std::stringstream sstream;

  sstream << "[INFO] Summary " << function_name << ":\n";

  int pn = 0;
  for (auto param : parameter_metadata) {
    sstream << "param_" + std::to_string(pn) << ": " << param.getAccessNum()
            << " access types\n";
    pn++;
  }

  sstream << "return: " << return_metadata.getAccessNum() << " access types\n";

  return sstream.str();
}

void store_into_json_file(const function_condition_set_t &cs,
                          const std::string &filename, bool verbose) {
  Json::Value jsonResult = to_json(cs, verbose);

  std::ofstream jsonOutFile(filename);
  Json::StreamWriterBuilder jsonBuilder;
  if (!verbose)
    jsonBuilder.settings_["indentation"] = "";

  std::unique_ptr<Json::StreamWriter> writer(jsonBuilder.newStreamWriter());
  writer->write(jsonResult, &jsonOutFile);
  jsonOutFile.close();
}
void store_into_text_file(const function_condition_set_t &cs,
                          const std::string &filename, bool verbose) {
  std::ofstream txtOutFile(filename);
  txtOutFile << to_string(cs, verbose);
  txtOutFile.close();
}
Json::Value to_json(const function_condition_set_t &cs, bool verbose) {
  Json::Value funCondJson(Json::arrayValue);

  for (auto fc : cs)
    funCondJson.append(fc.second.toJson(verbose));

  return funCondJson;
}

std::string to_string(const function_condition_set_t &cs, bool verbose) {
  std::stringstream sstream;

  for (auto fc : cs)
    sstream << fc.second.toString(verbose);

  return sstream.str();
}

std::string get_summary(const function_condition_set_t &cs) {
  std::stringstream sstream;

  for (auto fc : cs)
    sstream << fc.second.getSummary();

  return sstream.str();
}

void save_condition_set(const function_condition_set_t &cs, OutType out) {
  if (out == OutType::txt) {
    store_into_text_file(cs, config_t::instance()->output_file,
                         config_t::instance()->verbose >= Verbosity::v1);
  } else if (out == OutType::json) {
    store_into_json_file(cs, config_t::instance()->output_file,
                         config_t::instance()->verbose >= Verbosity::v1);
  } else if (out == OutType::stdo) {
    SVFUtil::outs() << to_string(cs, config_t::instance()->verbose >=
                                         Verbosity::v1);
  }
}
} // namespace liberator
