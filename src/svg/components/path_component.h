#pragma once

#include <string>

namespace donner {

class PathComponent {
public:
  std::string_view d() const;
  void setD(std::string_view d);

private:
  std::string d_;
};

}  // namespace donner