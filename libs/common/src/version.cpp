#include "voxorchestra/version.hpp"

namespace voxorchestra {

namespace {

const Version kVersion{0, 1, 0};
const std::string kVersionString = "0.1.0";

}  // namespace

const Version& version() { return kVersion; }

const std::string& version_string() { return kVersionString; }

}  // namespace voxorchestra
