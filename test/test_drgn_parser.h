#pragma once

#include <gmock/gmock.h>

#include "oi/type_graph/DrgnParser.h"

namespace oi::detail {
class SymbolService;
}
namespace oi::detail::type_graph {
class TypeGraph;
}
struct drgn_type;

using namespace oi::detail;

class DrgnParserTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    symbols_ = new SymbolService{TARGET_EXE_PATH};
  }

  static void TearDownTestSuite() {
    delete symbols_;
  }

  static type_graph::DrgnParser getDrgnParser(
      type_graph::TypeGraph& typeGraph, type_graph::DrgnParserOptions options);
  drgn_type* getDrgnRoot(std::string_view function);

  virtual std::string run(std::string_view function,
                          type_graph::DrgnParserOptions options);
  void test(std::string_view function,
            std::string_view expected,
            type_graph::DrgnParserOptions options);
  void test(std::string_view function, std::string_view expected);
  void testContains(std::string_view function,
                    std::string_view expected,
                    type_graph::DrgnParserOptions options = {});
  void testMultiCompiler(std::string_view function,
                         std::string_view expectedClang,
                         std::string_view expectedGcc,
                         type_graph::DrgnParserOptions options = {});
  void testMultiCompilerContains(std::string_view function,
                                 std::string_view expectedClang,
                                 std::string_view expectedGcc,
                                 type_graph::DrgnParserOptions options = {});

  static SymbolService* symbols_;
};
