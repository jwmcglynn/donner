#include "donner/editor/NativeFileDialog.h"

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace donner::editor {

namespace {

/// Build the AppKit content-type list for the given filters. Returns an
/// empty array when no usable extensions were provided (dialog allows any).
NSArray<UTType*>* ContentTypesForFilters(const std::vector<FileDialogFilter>& filters)
    API_AVAILABLE(macos(11.0)) {
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  for (const FileDialogFilter& filter : filters) {
    for (const std::string& ext : filter.extensions) {
      NSString* extString = [NSString stringWithUTF8String:ext.c_str()];
      UTType* type = [UTType typeWithFilenameExtension:extString];
      if (type != nil) {
        [types addObject:type];
      }
    }
  }
  return types;
}

void ApplyContentTypeFilters(NSSavePanel* panel, const std::vector<FileDialogFilter>& filters) {
  if (filters.empty()) {
    return;
  }
  // UniformTypeIdentifiers requires macOS 11+. On older systems the dialog is
  // simply left unfiltered rather than falling back to the deprecated
  // -allowedFileTypes API.
  if (@available(macOS 11.0, *)) {
    NSArray<UTType*>* types = ContentTypesForFilters(filters);
    if (types.count > 0) {
      panel.allowedContentTypes = types;
    }
  }
}

void ApplyDefaultDirectory(NSSavePanel* panel, const std::optional<std::string>& directory) {
  if (directory.has_value() && !directory->empty()) {
    NSString* dirString = [NSString stringWithUTF8String:directory->c_str()];
    panel.directoryURL = [NSURL fileURLWithPath:dirString isDirectory:YES];
  }
}

}  // namespace

bool NativeFileDialogAvailable() {
  return true;
}

std::optional<std::string> ShowNativeOpenFileDialog(GLFWwindow* parent,
                                                    const NativeOpenDialogRequest& request) {
  (void)parent;
  @autoreleasepool {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.message = [NSString stringWithUTF8String:request.title.c_str()];

    ApplyDefaultDirectory(panel, request.defaultDirectory);
    ApplyContentTypeFilters(panel, request.filters);

    if ([panel runModal] != NSModalResponseOK) {
      return std::nullopt;
    }
    NSURL* url = panel.URL;
    if (url == nil || url.path == nil) {
      return std::nullopt;
    }
    return std::string(url.path.UTF8String);
  }
}

std::optional<std::string> ShowNativeSaveFileDialog(GLFWwindow* parent,
                                                    const NativeSaveDialogRequest& request) {
  (void)parent;
  @autoreleasepool {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.message = [NSString stringWithUTF8String:request.title.c_str()];
    panel.canCreateDirectories = YES;

    ApplyDefaultDirectory(panel, request.defaultDirectory);

    if (request.defaultName.has_value() && !request.defaultName->empty()) {
      panel.nameFieldStringValue = [NSString stringWithUTF8String:request.defaultName->c_str()];
    }

    ApplyContentTypeFilters(panel, request.filters);

    if ([panel runModal] != NSModalResponseOK) {
      return std::nullopt;
    }
    NSURL* url = panel.URL;
    if (url == nil || url.path == nil) {
      return std::nullopt;
    }
    return std::string(url.path.UTF8String);
  }
}

void NoteNativeRecentDocument(const std::string& path) {
  if (path.empty()) {
    return;
  }
  @autoreleasepool {
    NSString* pathString = [NSString stringWithUTF8String:path.c_str()];
    NSURL* url = [NSURL fileURLWithPath:pathString];
    if (url != nil) {
      [[NSDocumentController sharedDocumentController] noteNewRecentDocumentURL:url];
    }
  }
}

}  // namespace donner::editor
