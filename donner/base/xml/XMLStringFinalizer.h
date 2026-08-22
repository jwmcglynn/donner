#pragma once
/// @file

namespace donner::xml {

class XMLDocument;

/// Intern and detach all persistent XML strings from parse-time source storage.
void FinalizeXMLDocumentStrings(XMLDocument& document);

}  // namespace donner::xml
