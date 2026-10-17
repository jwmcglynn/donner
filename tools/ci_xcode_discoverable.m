// Checks the LaunchServices discovery contract used by Bazel's native Xcode locator.
#import <CoreServices/CoreServices.h>
#import <Foundation/Foundation.h>

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    if (argc != 3) {
      return 64;
    }
    NSString* version = @(argv[1]);
    NSString* expected = [@(argv[2]) stringByResolvingSymlinksInPath];
    NSArray<NSURL*>* urls = CFBridgingRelease(
        LSCopyApplicationURLsForBundleIdentifier(CFSTR("com.apple.dt.Xcode"), NULL));
    if (!urls) {
      return 2;
    }
    BOOL found = NO;
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
      // A longer version can also claim Bazel's shorter alias. Reject ambiguous
      // aliases instead of allowing a different installation into the cache partition.
      if ([candidate isEqualToString:version] ||
          [candidate hasPrefix:[version stringByAppendingString:@"."]]) {
        NSString* developer = [[url.path stringByAppendingPathComponent:@"Contents/Developer"]
            stringByResolvingSymlinksInPath];
        if (![developer isEqualToString:expected]) {
          return 1;
        }
        found = YES;
      }
    }
    return found ? 0 : 1;
  }
}
