// yaml_writer_test — json_to_yaml quotes what YAML would read otherwise:
// values that open another construct and keys that hold ": " or "#". The
// output is read back by python3's yaml.safe_load when it is installed
// (77 — skipped — otherwise) and must equal the input.

#include "apostol/yaml_writer.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

int main(int argc, char* argv[])
{
    if (std::system("python3 -c 'import yaml' >/dev/null 2>&1") != 0) {
        std::printf("yaml_writer_test: python3 yaml not installed — skipped\n");
        return 77;
    }
    const std::string dir = argc > 1 ? argv[1] : ".";
    const nlohmann::json in = {
        {"summary", "- starts with dash"}, {"q", "? question"}, {"b", "`backtick"},
        {"tab", "a\tb"}, {"a b: c", "k"}, {"#hash", "v"}, {"plain", "text"},
        {"nested", {{"k: v", "- x"}}}, {"list", {"- one", "two"}},
        {"lines", "a\nb\r\nc"}, {"ctl", "x\x01y\x7f"}, {"esc", "q\"\\ \n"}, {"key\nbreak", "v"}};
    const auto yaml = apostol::json_to_yaml(in);
    const std::string y = dir + "/yaml_writer_test.yaml", j = dir + "/yaml_writer_test.json";
    std::ofstream(y) << yaml;
    std::ofstream(j) << in.dump();
    const std::string cmd = "python3 -c 'import sys, json, yaml; "
                            "sys.exit(0 if yaml.safe_load(open(sys.argv[1])) == json.load(open(sys.argv[2])) else 1)' "
                            + y + " " + j;
    const int rc = std::system(cmd.c_str());
    std::printf("yaml_writer_test: %s\n%s", rc == 0 ? "PASS" : "FAIL — read back differs or does not parse", yaml.c_str());
    return rc == 0 ? 0 : 1;
}
