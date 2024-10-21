#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/base/xml/XMLNode.h"

namespace donner::xml {

/**
 * Represents an XML document, which holds a collection of \ref XMLNode as the document tree.
 *
 * Each \ref XMLNode may only belong to a single document, and each document can have only one
 * root. XMLDocument is responsible for managing the lifetime of all elements in the document, by
 * storing a shared pointer to the internal Registry data-store.
 *
 * Data is stored using the Entity Component System (\ref EcsArchitecture) pattern, which is a
 * data-oriented design optimized for fast data access and cache locality, particularly during
 * rendering.
 *
 * XMLDocument and \ref XMLNode provide a facade over the ECS, and surface a familiar Document
 * Object Model (DOM) API to traverse and manipulate the document tree, which is internally stored
 * within Components in the ECS.  This makes \ref XMLNode a thin wrapper around an \ref Entity,
 * making the object lightweight and usable on the stack.
 *
 * @see \ref XMLNode
 * @see \ref EcsArchitecture
 */
class XMLDocument {
  friend class XMLNode;

  /// Internal constructor used by \ref XMLNode, to rehydrate a XMLDocument from the Registry.
  explicit XMLDocument(std::shared_ptr<Registry> registry) : registry_(std::move(registry)) {}

public:
  /**
   * Constructor to create an empty XMLDocument.
   *
   * To load a document from an XML file, use \ref donner::base::xml::XMLParser.
   *
   * @param settings Settings to configure the document.
   */
  XMLDocument();

  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  Registry& registry() { return *registry_; }

  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  const Registry& registry() const { return *registry_; }

  /// Gets the registry as a shared pointer, for advanced use.
  std::shared_ptr<Registry> sharedRegistry() const { return registry_; }

  /// Get the root XMLNdoe of the document.
  XMLNode root() const;

  /// Get the root ECS Entity of the document, for advanced use.
  EntityHandle rootEntityHandle() const;

  /**
   * Returns true if the two XMLDocument handles reference the same underlying document.
   */
  bool operator==(const XMLDocument& other) const;

private:
  /// Owned reference to the registry, which contains all information about the loaded document.
  std::shared_ptr<Registry> registry_;
};

}  // namespace donner::xml
