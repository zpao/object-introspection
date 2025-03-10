#include "runner_common.h"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/process.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <string>
#include <utility>

#include "oi/OIOpts.h"
#include "oi/support/Toml.h"

using namespace std::literals;

namespace bp = boost::process;
namespace bpt = boost::property_tree;
namespace fs = std::filesystem;

bool run_skipped_tests = false;

namespace {

std::string oidExe = OID_EXE_PATH;
std::string configFile = CONFIG_FILE_PATH;

bool verbose = false;
bool preserve = false;
bool preserve_on_failure = false;
std::vector<std::string> global_oid_args{};

constexpr static OIOpts cliOpts{
    OIOpt{'h', "help", no_argument, nullptr, "Print this message and exit"},
    OIOpt{'p', "preserve", no_argument, nullptr,
          "Do not clean up files generated by OID after tests are finished"},
    OIOpt{'P', "preserve-on-failure", no_argument, nullptr,
          "Do not clean up files generated by OID for failed tests"},
    OIOpt{'v', "verbose", no_argument, nullptr,
          "Verbose output. Show OID's stdout and stderr on test failure"},
    OIOpt{'f', "force", no_argument, nullptr,
          "Force running tests, even if they are marked as skipped"},
    OIOpt{'x', "oid", required_argument, nullptr,
          "Path to OID executable to test"},
    OIOpt{'\0', "enable-feature", required_argument, nullptr,
          "Enable extra OID feature."},
};

void usage(std::string_view progname) {
  std::cout << "usage: " << progname << " ...\n";
  std::cout << cliOpts;
}

}  // namespace

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  if (const char* envArgs = std::getenv("OID_TEST_ARGS")) {
    boost::split(global_oid_args, envArgs, boost::is_any_of(" "));
  }

  int c;
  while ((c = getopt_long(argc, argv, cliOpts.shortOpts(), cliOpts.longOpts(),
                          nullptr)) != -1) {
    switch (c) {
      case 'p':
        preserve = true;
        break;
      case 'P':
        preserve_on_failure = true;
        break;
      case 'v':
        verbose = true;
        break;
      case 'f':
        run_skipped_tests = true;
        break;
      case 'x':
        // Must convert to absolute path so it continues to work after working
        // directory is changed
        oidExe = fs::absolute(optarg);
        break;
      case '\0':
        global_oid_args.push_back("-f"s + optarg);
        break;
      case 'h':
      default:
        usage(argv[0]);
        return 0;
    }
  }

  return RUN_ALL_TESTS();
}

void IntegrationBase::SetUp() {
  // Move into a temporary directory to run the test in an isolated environment
  auto tmp_dir = TmpDirStr();
  char* res = mkdtemp(&tmp_dir[0]);
  if (!res)
    abort();
  workingDir = std::move(tmp_dir);
  fs::current_path(workingDir);
}

void IntegrationBase::TearDown() {
  const ::testing::TestInfo* const test_info =
      ::testing::UnitTest::GetInstance()->current_test_info();
  if (preserve || (preserve_on_failure && test_info->result()->Failed())) {
    std::cerr << "Working directory preserved at: " << workingDir << std::endl;
  } else {
    fs::remove_all(workingDir);
  }
}

int IntegrationBase::exit_code(Proc& proc) {
  proc.ctx.run();
  proc.proc.wait();

  // Read stdout and stderr into members, as required by certain tests (see
  // gen_tests.py)
  {
    std::ifstream stdout(workingDir / "stdout");
    stdout_.assign(std::istreambuf_iterator<char>(stdout),
                   std::istreambuf_iterator<char>());
  }
  {
    std::ifstream stderr(workingDir / "stderr");
    stderr_.assign(std::istreambuf_iterator<char>(stderr),
                   std::istreambuf_iterator<char>());
  }
  return proc.proc.exit_code();
}

fs::path IntegrationBase::createCustomConfig(const std::string& prefix,
                                             const std::string& suffix) {
  // If no extra config provided, return the config path unaltered.
  if (prefix.empty() && suffix.empty()) {
    return configFile;
  }

  auto customConfigFile = workingDir / "oid.config.toml";
  auto config = toml::parse_file(configFile);

  // As relative paths are allowed, we must canonicalise the paths before
  // moving the file to the temporary directory.
  fs::path configDirectory = fs::path(configFile).remove_filename();

  if (toml::table* types = config["types"].as_table()) {
    if (toml::array* arr = (*types)["containers"].as_array()) {
      arr->for_each([&](auto&& el) {
        if constexpr (toml::is_string<decltype(el)>) {
          el = configDirectory / el.get();
        }
      });
    }
  }
  if (toml::table* headers = config["headers"].as_table()) {
    for (auto& path : {"user_paths", "system_paths"}) {
      if (toml::array* arr = (*headers)[path].as_array()) {
        arr->for_each([&](auto&& el) {
          if constexpr (toml::is_string<decltype(el)>) {
            el = configDirectory / el.get();
          }
        });
      }
    }
  }

  std::ofstream customConfig(customConfigFile, std::ios_base::app);
  if (!prefix.empty()) {
    customConfig << "\n\n# Test custom config start\n\n";
    customConfig << prefix;
    customConfig << "\n\n# Test custom config end\n\n";
  }
  customConfig << config;
  if (!suffix.empty()) {
    customConfig << "\n\n# Test custom config start\n\n";
    customConfig << suffix;
    customConfig << "\n\n# Test custom config end\n\n";
  }

  return customConfigFile;
}

std::string OidIntegration::TmpDirStr() {
  return std::string("/tmp/oid-integration-XXXXXX");
}

OidProc OidIntegration::runOidOnProcess(OidOpts opts,
                                        std::vector<std::string> extra_args,
                                        std::string configPrefix,
                                        std::string configSuffix) {
  // Binary paths are populated by CMake
  std::string targetExe =
      std::string(TARGET_EXE_PATH) + " " + opts.targetArgs + " 1000";

  /* Spawn the target process with all IOs redirected to /dev/null to not polute
   * the terminal */
  // clang-format off
  bp::child targetProcess(
      targetExe,
      bp::std_in  < bp::null,
      bp::std_out > bp::null,
      bp::std_err > bp::null,
      opts.ctx);
  // clang-format on

  {
    // Delete previous segconfig, in case oid re-used the PID
    fs::path segconfigPath =
        "/tmp/oid-segconfig-" + std::to_string(targetProcess.id());
    fs::remove(segconfigPath);
    // Create the segconfig, so we have the rights to delete it above
    std::ofstream touch(segconfigPath);
  }

  fs::path thisConfig = createCustomConfig(configPrefix, configSuffix);

  // Keep PID as the last argument to make it easier for users to directly copy
  // and modify the command from the verbose mode output.
  // clang-format off
  auto default_args = std::array{
      "--debug-level=3"s,
      "--timeout=20"s,
      "--dump-json"s,
      "--config-file"s, thisConfig.string(),
      "--script-source"s, opts.scriptSource,
      "--mode=strict"s,
      "--pid"s, std::to_string(targetProcess.id()),
  };
  // clang-format on

  // The arguments are appended in ascending order of precedence (low -> high)
  std::vector<std::string> oid_args;
  oid_args.insert(oid_args.end(), global_oid_args.begin(),
                  global_oid_args.end());
  oid_args.insert(oid_args.end(), extra_args.begin(), extra_args.end());
  oid_args.insert(oid_args.end(), default_args.begin(), default_args.end());

  if (verbose) {
    std::cerr << "Running: " << targetExe << "\n";
    std::cerr << "Running: " << oidExe << " ";
    for (const auto& arg : oid_args) {
      std::cerr << arg << " ";
    }
    std::cerr << std::endl;
  }

  // Use tee to write the output to files. If verbose is on, also redirect the
  // output to stderr.
  bp::async_pipe std_out_pipe(opts.ctx), std_err_pipe(opts.ctx);
  bp::child std_out, std_err;
  if (verbose) {
    // clang-format off
    std_out = bp::child(bp::search_path("tee"),
                        (workingDir / "stdout").string(),
                        bp::std_in < std_out_pipe,
                        bp::std_out > stderr,
                        opts.ctx);
    std_err = bp::child(bp::search_path("tee"),
                        (workingDir / "stderr").string(),
                        bp::std_in < std_err_pipe,
                        bp::std_out > stderr,
                        opts.ctx);
    // clang-format on
  } else {
    // clang-format off
    std_out = bp::child(bp::search_path("tee"),
                        (workingDir / "stdout").string(),
                        bp::std_in < std_out_pipe,
                        bp::std_out > bp::null,
                        opts.ctx);
    std_err = bp::child(bp::search_path("tee"),
                        (workingDir / "stderr").string(),
                        bp::std_in < std_err_pipe,
                        bp::std_out > bp::null,
                        opts.ctx);
    // clang-format on
  }

  /* Spawn `oid` with tracing on and IOs redirected */
  // clang-format off
  bp::child oidProcess(
      oidExe,
      bp::args(oid_args),
      bp::env["OID_METRICS_TRACE"] = "time",
      bp::std_in  < bp::null,
      bp::std_out > std_out_pipe,
      bp::std_err > std_err_pipe,
      opts.ctx);
  // clang-format on

  return OidProc{
      .target = Proc{opts.ctx, std::move(targetProcess), {}, {}},
      .oid = Proc{opts.ctx, std::move(oidProcess), std::move(std_out),
                  std::move(std_err)},
  };
}

void IntegrationBase::compare_json(const bpt::ptree& expected_json,
                                   const bpt::ptree& actual_json,
                                   const std::string& full_key,
                                   bool expect_eq) {
  if (expected_json.empty()) {
    if (expect_eq) {
      ASSERT_EQ(expected_json.data(), actual_json.data())
          << "Incorrect value for key: " << full_key;
    } else {
      ASSERT_NE(expected_json.data(), actual_json.data())
          << "Incorrect value for key: " << full_key;
    }
  }

  if (expected_json.begin()->first == "") {
    // Empty key - assume this is an array

    ASSERT_EQ(expected_json.size(), actual_json.size())
        << "Array size difference for key: " << full_key;

    int i = 0;
    for (auto e_it = expected_json.begin(), a_it = actual_json.begin();
         e_it != expected_json.end() && a_it != actual_json.end();
         e_it++, a_it++) {
      compare_json(e_it->second, a_it->second,
                   full_key + "[" + std::to_string(i) + "]", expect_eq);
      i++;
    }
    return;
  }

  // Compare as Key-Value pairs
  for (const auto& [key, val] : expected_json) {
    if (key == "NOT") {
      auto curr_key = full_key + ".NOT";
      ASSERT_EQ(true, expect_eq) << "Invalid expected data: " << curr_key
                                 << " - Can not use nested \"NOT\" expressions";
      if (val.empty()) {
        // Check that a given single key does not exist
        const auto& key_to_check = val.data();
        auto actual_it = actual_json.find(key_to_check);
        auto bad_key = full_key + "." + key_to_check;
        if (actual_it != actual_json.not_found()) {
          ADD_FAILURE() << "Unexpected key found in output: " << bad_key;
        }
      } else {
        // Check that a key's value is not equal to something
        compare_json(val, actual_json, curr_key, !expect_eq);
      }
      continue;
    }

    auto actual_it = actual_json.find(key);
    auto curr_key = full_key + "." + key;
    if (actual_it == actual_json.not_found()) {
      // TODO: Remove these with the switch to treebuilderv2. This is a hack to
      // make some old JSON output compatible with new JSON output.
      if (key == "typeName") {
        auto type_names = actual_json.find("typeNames");
        if (type_names != actual_json.not_found()) {
          if (auto name = type_names->second.rbegin();
              name != type_names->second.rend()) {
            compare_json(val, name->second, curr_key, expect_eq);
            continue;
          }
        }
      } else if (key == "dynamicSize" && val.get_value<size_t>() == 0) {
        continue;
      }

      ADD_FAILURE() << "Expected key not found in output: " << curr_key;
      continue;
    }
    compare_json(val, actual_it->second, curr_key, expect_eq);
  }
}

std::string OilIntegration::TmpDirStr() {
  return std::string("/tmp/oil-integration-XXXXXX");
}

Proc OilIntegration::runOilTarget(OilOpts opts,
                                  std::string configPrefix,
                                  std::string configSuffix) {
  fs::path thisConfig = createCustomConfig(configPrefix, configSuffix);

  std::string targetExe = std::string(TARGET_EXE_PATH) + " " + opts.targetArgs +
                          " " + thisConfig.string();

  if (verbose) {
    std::cerr << "Running: " << targetExe << std::endl;
  }

  // Use tee to write the output to files. If verbose is on, also redirect the
  // output to stderr.
  bp::async_pipe std_out_pipe(opts.ctx), std_err_pipe(opts.ctx);
  bp::child std_out, std_err;
  if (verbose) {
    // clang-format off
    std_out = bp::child(bp::search_path("tee"),
                        (workingDir / "stdout").string(),
                        bp::std_in < std_out_pipe,
                        bp::std_out > stderr,
                        opts.ctx);
    std_err = bp::child(bp::search_path("tee"),
                        (workingDir / "stderr").string(),
                        bp::std_in < std_err_pipe,
                        bp::std_out > stderr,
                        opts.ctx);
    // clang-format on
  } else {
    // clang-format off
    std_out = bp::child(bp::search_path("tee"),
                        (workingDir / "stdout").string(),
                        bp::std_in < std_out_pipe,
                        bp::std_out > bp::null,
                        opts.ctx);
    std_err = bp::child(bp::search_path("tee"),
                        (workingDir / "stderr").string(),
                        bp::std_in < std_err_pipe,
                        bp::std_out > bp::null,
                        opts.ctx);
    // clang-format on
  }

  /* Spawn target with tracing on and IOs redirected in custom pipes to be read
   * later */
  // clang-format off
  bp::child targetProcess(
      targetExe,
      bp::std_in  < bp::null,
      bp::std_out > std_out_pipe,
      bp::std_err > std_err_pipe,
      opts.ctx);
  // clang-format on

  return Proc{opts.ctx, std::move(targetProcess), std::move(std_out),
              std::move(std_err)};
}
