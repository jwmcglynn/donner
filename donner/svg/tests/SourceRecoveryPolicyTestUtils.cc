#include "donner/svg/tests/SourceRecoveryPolicyTestUtils.h"

#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "gtest/gtest.h"
namespace donner::svg::test {
class CountingRecoveryLoader final : public svg::ResourceLoaderInterface {
public:
  explicit CountingRecoveryLoader(int& calls) : calls_(calls) {}
  std::variant<std::vector<uint8_t>, svg::ResourceLoaderError> fetchExternalResource(
      std::string_view) override {
    ++calls_;
    return svg::ResourceLoaderError::NotFound;
  }

private:
  int& calls_;
};

void ChangeSourceRecoveryPolicy(SVGDocument& document, int mode, int& callbacks) {
  document.withWriteAccess([&](svg::DocumentWriteAccess& access) {
    auto& registry = access.registry();
    auto& resources = registry.ctx().get<svg::components::ResourceManagerContext>();
    switch (mode) {
      case 0:
        resources.setResourceLoader(std::make_unique<CountingRecoveryLoader>(callbacks));
        break;
      case 1: resources.setProcessingMode(svg::ProcessingMode::SecureStatic); break;
      case 2:
        resources.setSvgParseCallback(
            [&](const std::vector<uint8_t>&,
                ParseWarningSink&) -> std::optional<svg::SVGDocumentHandle> {
              ++callbacks;
              return std::nullopt;
            });
        break;
      case 3: {
        auto* store = document.xmlDocument().sourceStore();
        auto limits = store->resourceLimits();
        limits.maximumSourceSize = store->source().size() + 1024;
        ASSERT_TRUE(store->setResourceLimits(limits));
        break;
      }
      case 4: {
        auto budget = registry.ctx().get<svg::components::DocumentResourceFamilyContext>().budget;
        registry.ctx().erase<svg::components::ParsedPayloadResourceBudget>();
        registry.ctx().emplace<svg::components::ParsedPayloadResourceBudget>(
            svg::components::ParsedPayloadResourceBudget::Limits{.maximumRetainedBytes = 1},
            budget);
        break;
      }
      case 5:
        registry.ctx().get<svg::components::DocumentResourceFamilyContext>().budget =
            std::make_shared<svg::components::DocumentResourceFamilyBudget>(
                svg::components::DocumentResourceFamilyBudget::Limits{.maximumTotalRetainedBytes =
                                                                          1});
        break;
    }
  });
}

}  // namespace donner::svg::test
