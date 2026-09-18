#pragma once
/// @file
/// Small C++26 reflection experiment independent of Donner's C++20 library.

#include <meta>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#if !defined(__cpp_impl_reflection) || __cpp_impl_reflection < 202506L
#error "This experiment requires C++26 reflection (GCC 16 with -freflection)."
#endif

namespace donner::experimental {

/// Plain data with no reflection registration or generated metadata.
struct Rect {
  double x;
  double y;
  double width;
  double height;
};

/// Example enum whose names are discovered by the compiler.
enum class PaintMode { Fill, Stroke };

/// Visits public nonstatic fields in declaration order, preserving constness.
/// @param object Object whose fields are visited.
/// @param visitor Callable receiving the reflected name and a field reference.
template <typename T, typename Visitor>
void ForEachMember(T& object, Visitor visitor) {
  using Object = std::remove_cv_t<T>;
  static constexpr auto members = std::define_static_array(
      std::meta::nonstatic_data_members_of(^^Object, std::meta::access_context::current()));
  template for (constexpr auto member : members) {
    visitor(std::meta::identifier_of(member), object.[:member:]);
  }
}

/// Formats fields without listing their names or accessing them individually.
/// @param object Object whose public fields can be written to an output stream.
template <typename T>
std::string Describe(const T& object) {
  std::ostringstream output;
  bool first = true;
  ForEachMember(object, [&](std::string_view name, const auto& value) {
    if (!first) {
      output << ", ";
    }
    first = false;
    output << name << "=" << value;
  });
  return output.str();
}

/// Returns the reflected enum name, or an owning fallback for an unknown value.
/// @param value Enum value to identify.
template <typename E>
std::string EnumName(E value) {
  template for (constexpr auto enumerator :
                std::define_static_array(std::meta::enumerators_of(^^E))) {
    if (value == std::meta::extract<E>(enumerator)) {
      return std::string(std::meta::identifier_of(enumerator));
    }
  }
  return "<unknown>";
}

constexpr auto kRectMembers = std::define_static_array(
    std::meta::nonstatic_data_members_of(^^Rect, std::meta::access_context::current()));
static_assert(kRectMembers.size() == 4);
static_assert(std::meta::identifier_of(kRectMembers[0]) == "x");
static_assert(std::meta::type_of(kRectMembers[0]) == ^^double);
using ReflectedRect = [:^^Rect:];
static_assert(std::is_same_v<Rect, ReflectedRect>);

}  // namespace donner::experimental
