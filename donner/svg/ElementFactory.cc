#include "donner/svg/ElementFactory.h"

#include <utility>

#include "donner/base/CompileTimeMap.h"

namespace donner::svg::details {

namespace {

/// Tag-name to type-index entries, indexed like \ref AllSVGElements.
template <typename... Types, std::size_t... Indices>
constexpr auto MakeTypeIndexEntries(entt::type_list<Types...>, std::index_sequence<Indices...>) {
  return std::to_array<std::pair<std::string_view, std::size_t>>({{Types::Tag, Indices}...});
}

template <typename... Types>
constexpr auto MakeTypeIndexEntries(entt::type_list<Types...> types) {
  return MakeTypeIndexEntries(types, std::index_sequence_for<Types...>{});
}

/// Whether each type, indexed like \ref AllSVGElements, is experimental.
template <typename... Types>
constexpr std::array<bool, sizeof...(Types)> MakeExperimentalFlags(entt::type_list<Types...>) {
  return {IsExperimentalElement<Types>()...};
}

constexpr auto kExperimentalTypes = MakeExperimentalFlags(AllSVGElements{});

}  // namespace

SVGUnknownElement ElementFactory::CreateUnknownOn(EntityHandle handle,
                                                  const xml::XMLQualifiedNameRef& tagName) {
  return SVGUnknownElement::CreateOn(handle, tagName);
}

std::optional<std::size_t> ElementFactory::FindType(std::string_view tagName,
                                                    bool experimentalElementsEnabled) {
  static DONNER_CONSTEXPR_MAP auto kTypeIndices =
      makeCompileTimeMap(MakeTypeIndexEntries(AllSVGElements{}));
  const std::size_t* index = kTypeIndices.find(tagName);
  if (index == nullptr || (kExperimentalTypes[*index] && !experimentalElementsEnabled)) {
    return std::nullopt;
  }
  return *index;
}

}  // namespace donner::svg::details
