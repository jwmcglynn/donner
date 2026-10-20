// Lists the Xcode installations that Bazel's native Xcode locator resolves for a version alias.
//
// Usage: ci_xcode_discoverable <version>
//
// Prints the resolved Contents/Developer directory of every LaunchServices-registered
// com.apple.dt.Xcode bundle that can claim <version> (the same version or a longer one, as
// Bazel's aliases do), one per line and without duplicates. Exits 0 with no output when no
// bundle matches, 2 when a registered bundle cannot be read, and 64 on a usage error.
#import <CoreServices/CoreServices.h>
#import <Foundation/Foundation.h>

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    if (argc != 2) {
      return 64;
    }
    NSString* version = @(argv[1]);
    // NULL means LaunchServices has no Xcode registered at all.
    NSArray<NSURL*>* urls = CFBridgingRelease(
        LSCopyApplicationURLsForBundleIdentifier(CFSTR("com.apple.dt.Xcode"), NULL));
    NSMutableOrderedSet<NSString*>* developers = [NSMutableOrderedSet orderedSet];
    for (NSURL* url in urls) {
      NSBundle* bundle = [NSBundle bundleWithURL:url];
      if (!bundle) {
        return 2;
      }
      if (![bundle.bundleIdentifier isEqualToString:@"com.apple.dt.Xcode"]) {
        continue;
      }
      NSString* candidate = bundle.infoDictionary[@"CFBundleShortVersionString"];
      if (![candidate isKindOfClass:[NSString class]]) {
        return 2;
      }
      while ([[candidate componentsSeparatedByString:@"."] count] < 3) {
        candidate = [candidate stringByAppendingString:@".0"];
      }
      // A longer version also claims Bazel's shorter alias, so it is listed too and the caller
      // rejects the ambiguity instead of letting either installation into the cache partition.
      if ([candidate isEqualToString:version] ||
          [candidate hasPrefix:[version stringByAppendingString:@"."]]) {
        [developers addObject:[[url.path stringByAppendingPathComponent:@"Contents/Developer"]
                                  stringByResolvingSymlinksInPath]];
      }
    }
    for (NSString* developer in developers) {
      printf("%s\n", developer.fileSystemRepresentation);
    }
    return 0;
  }
}
