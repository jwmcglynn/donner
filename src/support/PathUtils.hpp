// Copyright 2019-2023 hdoc
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>

namespace hdoc::utils {

std::string pathToRelative(const std::string& path, const std::string& rootDir);

} // namespace hdoc::utils
