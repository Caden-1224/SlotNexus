#include "slotnexus/version.hpp"
// Author: Caden

namespace slotnexus {

namespace {

const Version kVersion{0, 2, 0};
const std::string kVersionString = "0.2.0";

}  // namespace

const Version& version() { return kVersion; }

const std::string& version_string() { return kVersionString; }

}  // namespace slotnexus
