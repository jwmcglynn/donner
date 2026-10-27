#pragma once
/// @file

#include <iosfwd>

namespace donner::svg {

/**
 * Run the donner-cli command line tool.
 *
 * @param argc Number of command line arguments.
 * @param argv Command line arguments.
 * @param out Output stream for normal user-facing output.
 * @param err Output stream for errors.
 * @return Process-style exit code.
 */
int RunDonnerSvgTool(int argc, char* argv[], std::ostream& out, std::ostream& err);

}  // namespace donner::svg
