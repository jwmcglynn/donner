#pragma once
/// @file

#include "donner/base/RcString.h"

namespace donner::svg::components {

/**
 * Holds the value of the `id` attribute of an element.
 */
struct IdComponent {
public:
  /**
   * Constructor, create an IdComponent for a given ID. To change the value, destory and recreate
   * this object.
   *
   * @param value ID attribute value.
   */
  explicit IdComponent(const RcString& value) : id_(value) {}

  /// Get the element `id` attribute value. To change the value, destroy and recreate \ref
  /// IdComponent.
  const RcString& id() const { return id_; }

private:
  RcString id_;  //!< The value of the `id` attribute, the element ID.
};

}  // namespace donner::svg::components
