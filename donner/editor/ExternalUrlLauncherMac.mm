#include "donner/editor/ExternalUrlLauncher.h"

#import <AppKit/AppKit.h>

#include <string>

namespace donner::editor {

bool LaunchExternalUrl(ExternalUrlTarget target) {
  const std::string url(ExternalUrlValue(target));
  if (url.empty()) {
    return false;
  }

  @autoreleasepool {
    NSString* value = [NSString stringWithUTF8String:url.c_str()];
    if (value == nil) {
      return false;
    }

    NSURL* externalUrl = [NSURL URLWithString:value];
    return externalUrl != nil && [[NSWorkspace sharedWorkspace] openURL:externalUrl];
  }
}

}  // namespace donner::editor
