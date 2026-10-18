#pragma once
/// @file

#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg::components {

/** Visit a matching numeric field on an existing component.
 * @param registry Document storage with write access held.
 * @param target Animation target.
 * @param name Attribute name.
 * @param fields Attribute/member mappings for this component type.
 * @param visitor Called once with the component and member pointer when found.
 * @return Whether a matching field exists on this target.
 */
template <typename Component, typename Value, typename Visitor>
bool VisitAnimationNumberFields(
    Registry& registry, Entity target, std::string_view name,
    std::initializer_list<std::pair<std::string_view, Value Component::*>> fields,
    Visitor& visitor) {
  auto* component = registry.try_get<Component>(target);
  if (!component) {
    return false;
  }
  for (const auto& [attribute, field] : fields) {
    if (attribute == name) {
      visitor(*component, field);
      return true;
    }
  }
  return false;
}

/** Use one field mapping for numeric animation application and underlying-value reads.
 * @param registry Document storage with write access held.
 * @param target Animation target.
 * @param name Attribute name.
 * @param visitor Called with the component and its numeric member pointer.
 * @return Whether this is a supported numeric attribute on the target.
 */
template <typename Visitor>
bool VisitAnimationNumberAttribute(Registry& registry, Entity target, std::string_view name,
                                   Visitor&& visitor) {
  return VisitAnimationNumberFields<FEOffsetComponent, double>(
             registry, target, name,
             {{"dx", &FEOffsetComponent::dx}, {"dy", &FEOffsetComponent::dy}}, visitor) ||
         VisitAnimationNumberFields<FEDropShadowComponent, double>(
             registry, target, name,
             {{"dx", &FEDropShadowComponent::dx}, {"dy", &FEDropShadowComponent::dy}}, visitor) ||
         VisitAnimationNumberFields<FECompositeComponent, double>(
             registry, target, name,
             {{"k1", &FECompositeComponent::k1},
              {"k2", &FECompositeComponent::k2},
              {"k3", &FECompositeComponent::k3},
              {"k4", &FECompositeComponent::k4}},
             visitor) ||
         VisitAnimationNumberFields<FEFuncComponent, double>(
             registry, target, name,
             {{"slope", &FEFuncComponent::slope},
              {"intercept", &FEFuncComponent::intercept},
              {"amplitude", &FEFuncComponent::amplitude},
              {"exponent", &FEFuncComponent::exponent},
              {"offset", &FEFuncComponent::offset}},
             visitor) ||
         VisitAnimationNumberFields<FEConvolveMatrixComponent, double>(
             registry, target, name, {{"bias", &FEConvolveMatrixComponent::bias}}, visitor) ||
         VisitAnimationNumberFields<FEConvolveMatrixComponent, std::optional<double>>(
             registry, target, name, {{"divisor", &FEConvolveMatrixComponent::divisor}}, visitor) ||
         VisitAnimationNumberFields<FEDisplacementMapComponent, double>(
             registry, target, name, {{"scale", &FEDisplacementMapComponent::scale}}, visitor) ||
         VisitAnimationNumberFields<FETurbulenceComponent, double>(
             registry, target, name, {{"seed", &FETurbulenceComponent::seed}}, visitor) ||
         VisitAnimationNumberFields<FEDiffuseLightingComponent, double>(
             registry, target, name,
             {{"surfaceScale", &FEDiffuseLightingComponent::surfaceScale},
              {"diffuseConstant", &FEDiffuseLightingComponent::diffuseConstant}},
             visitor) ||
         VisitAnimationNumberFields<FESpecularLightingComponent, double>(
             registry, target, name,
             {{"surfaceScale", &FESpecularLightingComponent::surfaceScale},
              {"specularConstant", &FESpecularLightingComponent::specularConstant},
              {"specularExponent", &FESpecularLightingComponent::specularExponent}},
             visitor) ||
         VisitAnimationNumberFields<LightSourceComponent, double>(
             registry, target, name,
             {{"azimuth", &LightSourceComponent::azimuth},
              {"elevation", &LightSourceComponent::elevation},
              {"x", &LightSourceComponent::x},
              {"y", &LightSourceComponent::y},
              {"z", &LightSourceComponent::z},
              {"pointsAtX", &LightSourceComponent::pointsAtX},
              {"pointsAtY", &LightSourceComponent::pointsAtY},
              {"pointsAtZ", &LightSourceComponent::pointsAtZ},
              {"specularExponent", &LightSourceComponent::spotExponent}},
             visitor) ||
         VisitAnimationNumberFields<LightSourceComponent, std::optional<double>>(
             registry, target, name,
             {{"limitingConeAngle", &LightSourceComponent::limitingConeAngle}}, visitor);
}

}  // namespace donner::svg::components
