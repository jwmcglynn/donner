#include "donner/svg/components/style/DocumentLayerOrder.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/resources/StylesheetImportLoader.h"

namespace donner::svg::components {
namespace {

constexpr css::LayerId kRoot = 0;
constexpr css::LayerId kRefused = css::kUnlayered;
constexpr std::size_t kMaximumDocumentLayers = 16384;

bool HasAuthorLayers(const StylesheetComponent& sheet) {
  return sheet.stylesheet.layers() ||
         std::ranges::any_of(sheet.importedStylesheets,
                             [](const css::Stylesheet& imported) { return imported.layers(); });
}

Entity NextTreeNode(const Registry& registry, Entity root, Entity current) {
  auto* node = registry.try_get<donner::components::TreeComponent>(current);
  if (!node) {
    return entt::null;
  }
  if (node->firstChild() != entt::null) {
    return node->firstChild();
  }
  while (current != root && node->nextSibling() == entt::null) {
    current = node->parent();
    node = &registry.get<donner::components::TreeComponent>(current);
  }
  return current == root ? Entity(entt::null) : node->nextSibling();
}

std::vector<Entity> SheetOrder(const Registry& registry) {
  const auto view = registry.view<const StylesheetComponent>();
  const auto* context = registry.ctx().find<SVGDocumentContext>();
  if (!context || !registry.all_of<donner::components::TreeComponent>(context->rootEntity)) {
    return std::vector<Entity>(view.begin(), view.end());
  }
  std::vector<Entity> result;
  const Entity root = context->rootEntity;
  for (Entity current = root; current != entt::null && result.size() != view.size();
       current = NextTreeNode(registry, root, current)) {
    if (view.contains(current)) {
      result.push_back(current);
    }
  }
  return result;
}

bool ValidLayerNodes(std::span<const css::LayerNode> nodes) {
  if (nodes.size() > css::Stylesheet::kMaximumLayers) {
    return false;
  }
  std::array<std::uint8_t, css::Stylesheet::kMaximumLayers> depths;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto parent = nodes[i].parent;
    if (parent == css::kUnlayered) {
      depths[i] = 1;
    } else {
      if (parent >= i || depths[parent] >= css::Stylesheet::kMaximumLayerDepth) {
        return false;
      }
      depths[i] = static_cast<std::uint8_t>(depths[parent] + 1);
    }
  }
  return true;
}

bool ValidLayerEvents(const css::StylesheetLayers& syntax, std::size_t importCount) {
  if (syntax.events.size() > css::parser::RuleParser::kMaximumComponentValues) {
    return false;
  }
  std::size_t nextImport = 0;
  for (const auto& event : syntax.events) {
    switch (event.kind) {
      case css::LayerEvent::Kind::Import:
        if (event.index != nextImport || nextImport >= importCount) {
          return false;
        }
        ++nextImport;
        break;
      case css::LayerEvent::Kind::Statement:
      case css::LayerEvent::Kind::Block:
        if (event.index >= syntax.nodes.size()) {
          return false;
        }
        break;
      default: return false;
    }
  }
  return nextImport == importCount;
}

bool ValidFontFaceLayers(const css::Stylesheet& sheet, const css::StylesheetLayers& syntax) {
  return syntax.fontFaceLayers.empty() ||
         (syntax.fontFaceLayers.size() == sheet.fontFaces().size() &&
          std::ranges::all_of(syntax.fontFaceLayers, [&](css::LayerId layer) {
            return layer == css::kUnlayered || layer < syntax.nodes.size();
          }));
}

bool ValidLayerMetadata(const css::Stylesheet& sheet, const css::StylesheetLayers& syntax) {
  return ValidLayerNodes(syntax.nodes) && ValidLayerEvents(syntax, sheet.imports().size()) &&
         ValidFontFaceLayers(sheet, syntax) &&
         std::ranges::all_of(sheet.imports(), [&](const css::ImportRule& import) {
           return import.layer == css::kUnlayered || import.layer < syntax.nodes.size();
         });
}

struct LayerNode {
  css::LayerId firstChild = kRefused;
  css::LayerId lastChild = kRefused;
  css::LayerId nextSibling = kRefused;
  std::uint16_t rank = 0;
  std::uint16_t depth = 0;
};

struct SheetLayers {
  css::LayerId base;
  std::vector<css::LayerId> bySheetId;
};

struct ImportLinks {
  explicit ImportLinks(const StylesheetComponent& root) : root(root) {
    if (!build()) {
      children.clear();
      valid = false;
    }
  }

  bool addLink(std::size_t i, std::span<std::uint8_t> depths, std::size_t count) {
    const auto& origin = root.importOrigins[i];
    const auto parent = origin.parentEntry.value_or(count);
    if (parent <= i || (origin.parentEntry && parent >= count)) {
      return false;  // The loader's flattened order is always children before parents.
    }
    const std::size_t depth = parent == count ? 1 : depths[parent] + 1;
    if (depth > StylesheetImportBudget::kMaximumDepth) {
      return false;
    }
    depths[i] = static_cast<std::uint8_t>(depth);
    const auto& parentSheet = parent == count ? root.stylesheet : root.importedStylesheets[parent];
    return origin.importIndex < parentSheet.imports().size() &&
           children.emplace(std::pair(parent, origin.importIndex), i).second;
  }

  bool build() {
    const std::size_t count = root.importedStylesheets.size();
    if (root.importOrigins.size() != count || count > StylesheetImportBudget::kMaximumImports) {
      return false;
    }
    std::array<std::uint8_t, StylesheetImportBudget::kMaximumImports> depths;
    for (std::size_t i = count; i != 0;) {
      if (!addLink(--i, depths, count)) {
        return false;
      }
    }
    return true;
  }

  const StylesheetComponent& root;
  std::map<std::pair<std::size_t, std::size_t>, std::size_t> children;
  bool valid = true;
};

}  // namespace

struct DocumentLayerOrder::Data {
  css::LayerId append(css::LayerId parent, const RcString& name) {
    const auto id = static_cast<css::LayerId>(nodes.size());
    auto& previous = nodes[parent];
    if (previous.lastChild == kRefused) {
      previous.firstChild = id;
    } else {
      nodes[previous.lastChild].nextSibling = id;
    }
    previous.lastChild = id;
    nodes.push_back({.depth = static_cast<std::uint16_t>(previous.depth + 1)});
    if (!name.empty()) {
      names.emplace(std::pair(parent, name), id);
    }
    return id;
  }

  void adoptExistingPath(const css::StylesheetLayers& syntax, SheetLayers& sheet,
                         std::span<const css::LayerId> path, css::LayerId& parent,
                         std::size_t& length) {
    // Named segments may have been established by a different sheet occurrence.
    while (length > 0) {
      const auto& name = syntax.nodes[path[length - 1]].name;
      if (name.empty()) {
        break;
      }
      const auto existing = names.find({parent, name});
      if (existing == names.end()) {
        break;
      }
      parent = existing->second;
      sheet.bySheetId[path[--length]] = parent;
    }
  }

  css::LayerId declare(const css::StylesheetLayers& syntax, SheetLayers& sheet,
                       css::LayerId local) {
    if (local == css::kUnlayered) {
      return sheet.base;
    }
    if (sheet.base == kRefused || sheet.bySheetId[local] != kRefused) {
      return sheet.bySheetId[local];
    }
    std::array<css::LayerId, css::Stylesheet::kMaximumLayerDepth> path;
    std::size_t length = 0;
    css::LayerId cursor = local;
    while (cursor != css::kUnlayered && sheet.bySheetId[cursor] == kRefused) {
      path[length++] = cursor;
      cursor = syntax.nodes[cursor].parent;
    }
    css::LayerId parent = cursor == css::kUnlayered ? sheet.base : sheet.bySheetId[cursor];
    adoptExistingPath(syntax, sheet, path, parent, length);
    if (length > kMaximumDocumentLayers + 1 - nodes.size() ||
        length + nodes[parent].depth > css::Stylesheet::kMaximumLayerDepth) {
      refused = true;
      return kRefused;
    }
    while (length > 0) {
      const auto segment = path[--length];
      parent = append(parent, syntax.nodes[segment].name);
      sheet.bySheetId[segment] = parent;
    }
    return parent;
  }

  void visitImport(const css::Stylesheet& sheet, const css::StylesheetLayers* syntax,
                   SheetLayers& mapping, const ImportLinks& links, std::size_t occurrence,
                   std::size_t position) {
    const auto local = sheet.imports()[position].layer;
    const auto target = syntax ? declare(*syntax, mapping, local) : mapping.base;
    // An applicable import establishes its layer at this point even if the URL failed to load.
    if (const auto found = links.children.find({occurrence, position});
        found != links.children.end()) {
      visit(links, found->second, target);
    }
  }

  void visit(const ImportLinks& links, std::size_t occurrence, css::LayerId base) {
    const auto& imports = links.root.importedStylesheets;
    const auto& sheet = occurrence == imports.size() ? links.root.stylesheet : imports[occurrence];
    const auto* syntax = sheet.layers();
    auto& mapping = sheets[&sheet];
    if (base == kRefused || (syntax && !ValidLayerMetadata(sheet, *syntax))) {
      refused = true;
      mapping.base = kRefused;
      return;
    }
    mapping.base = base;
    if (!syntax) {
      for (std::size_t i = 0; i < sheet.imports().size(); ++i) {
        visitImport(sheet, nullptr, mapping, links, occurrence, i);
      }
      return;
    }
    mapping.bySheetId.resize(syntax->nodes.size(), kRefused);
    for (const auto& event : syntax->events) {
      if (event.kind == css::LayerEvent::Kind::Import) {
        visitImport(sheet, syntax, mapping, links, occurrence, event.index);
      } else {
        declare(*syntax, mapping, static_cast<css::LayerId>(event.index));
      }
    }
  }

  void assignRanks(css::LayerId parent, std::uint16_t& rank) {
    auto& node = nodes[parent];
    for (auto child = node.firstChild; child != kRefused; child = nodes[child].nextSibling) {
      assignRanks(child, rank);
    }
    node.rank = ++rank;
  }

  void visitRoot(const StylesheetComponent& sheet) {
    const ImportLinks links(sheet);
    if (links.valid) {
      for (const auto& imported : sheet.importedStylesheets) {
        // An unreachable occurrence cannot contribute even its unlayered declarations.
        sheets.emplace(&imported, SheetLayers{kRefused, {}});
      }
    } else {
      refused = true;
      blockedImports.insert(&sheet.stylesheet);
    }
    visit(links, sheet.importedStylesheets.size(), kRoot);
  }

  void freezeRanks() {
    std::uint16_t rank = 0;
    assignRanks(kRoot, rank);
    // Resolve node identities to ranks once. The tree and its name index are not needed on workers.
    const auto rankOf = [&](css::LayerId node) {
      return node == kRefused ? std::uint16_t(0) : nodes[node].rank;
    };
    rootRank = rankOf(kRoot);
    for (auto& [key, sheet] : sheets) {
      sheet.base = rankOf(sheet.base);
      std::ranges::transform(sheet.bySheetId, sheet.bySheetId.begin(), rankOf);
    }
    names.clear();
    std::vector<LayerNode>().swap(nodes);
  }

  std::vector<LayerNode> nodes{1};
  std::vector<Entity> orderedSheets;
  std::map<std::pair<css::LayerId, RcString>, css::LayerId> names;
  std::unordered_map<const css::Stylesheet*, SheetLayers> sheets;
  std::unordered_set<const css::Stylesheet*> blockedImports;
  std::uint16_t rootRank = 0;
  bool refused = false;
};

DocumentLayerOrder::DocumentLayerOrder() : data_(std::make_unique<Data>()) {}
DocumentLayerOrder::~DocumentLayerOrder() = default;

std::unique_ptr<DocumentLayerOrder> DocumentLayerOrder::Build(const Registry& registry) {
  const auto view = registry.view<const StylesheetComponent>();
  const auto usesLayers = [&](Entity entity) {
    const auto& sheet = view.get<const StylesheetComponent>(entity);
    return !sheet.isUserAgentStylesheet && sheet.isCssType() && HasSvgNamespace(registry, entity) &&
           HasAuthorLayers(sheet);
  };
  if (std::ranges::none_of(view, usesLayers)) {
    return nullptr;
  }
  std::unique_ptr<DocumentLayerOrder> order(new DocumentLayerOrder());
  auto& sheets = order->data_->orderedSheets;
  sheets = SheetOrder(registry);
  if (std::ranges::none_of(sheets, usesLayers)) {
    // Only detached sheets use layers. Preserve ordinary sheet traversal and skip those sheets.
    sheets.clear();
    std::ranges::remove_copy_if(view, std::back_inserter(sheets), usesLayers);
    return order;
  }
  for (Entity entity : sheets) {
    if (usesLayers(entity)) {
      order->data_->visitRoot(view.get<const StylesheetComponent>(entity));
    }
  }
  order->data_->freezeRanks();
  return order;
}

bool DocumentLayerOrder::allowsImportsOf(const css::Stylesheet& sheet) const {
  return !data_->blockedImports.contains(&sheet);
}

bool DocumentLayerOrder::hasRanks() const {
  return data_->rootRank != 0;
}

DocumentLayerOrder::SheetRanks DocumentLayerOrder::ranksFor(const css::Stylesheet& sheet) const {
  const auto found = data_->sheets.find(&sheet);
  if (found == data_->sheets.end()) {
    return {data_->rootRank, {}};
  }
  const auto& mapping = found->second;
  return {mapping.base, mapping.bySheetId};
}

std::vector<Entity> DocumentLayerOrder::StylesheetsInDocumentOrder(const Registry& registry) {
  return SheetOrder(registry);
}

std::span<const Entity> DocumentLayerOrder::stylesheets() const {
  return data_->orderedSheets;
}

void DocumentLayerOrder::reportRefusal(ParseWarningSink& warnings) const {
  if (data_->refused) {
    warnings.add([] {
      return ParseDiagnostic::Warning("CSS document cascade layer metadata was refused",
                                      FileOffset::Offset(0));
    });
  }
}

}  // namespace donner::svg::components
