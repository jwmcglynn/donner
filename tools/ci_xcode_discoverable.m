// Lists the Xcode installations that Bazel's native Xcode locator resolves for a version alias.
//
// Usage: ci_xcode_discoverable <version> [--register-installed <build>]
//
// Prints the resolved Contents/Developer directory of every LaunchServices-registered
// com.apple.dt.Xcode bundle that can claim <version> (the same version or a longer one, as
// Bazel's aliases do), one per line and without duplicates. Exits 0 with no output when no
// bundle matches, 2 when a registered bundle cannot be read, and 64 on a usage error.
// --register-installed refreshes Apple-signed matching installations in /Applications for the
// calling account, returns no paths, and exits 3 if inspection or registration fails.
#import <CoreServices/CoreServices.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <string.h>

static BOOL ClaimsVersion(NSString* candidate, NSString* version) {
  if (![candidate isKindOfClass:[NSString class]]) {
    return NO;
  }
  while ([[candidate componentsSeparatedByString:@"."] count] < 3) {
    candidate = [candidate stringByAppendingString:@".0"];
  }
  return [candidate isEqualToString:version] ||
         [candidate hasPrefix:[version stringByAppendingString:@"."]];
}

static BOOL HasAppleSignature(NSURL* url) {
  SecStaticCodeRef code = NULL;
  SecRequirementRef requirement = NULL;
  OSStatus status = SecStaticCodeCreateWithPath((__bridge CFURLRef)url, kSecCSDefaultFlags, &code);
  if (status == errSecSuccess) {
    status = SecStaticCodeCheckValidity(
        code, kSecCSStrictValidate | kSecCSCheckAllArchitectures | kSecCSCheckNestedCode, NULL);
  }
  if (status == errSecSuccess) {
    status = SecRequirementCreateWithString(
        CFSTR("anchor apple generic and certificate leaf[field.1.2.840.113635.100.6.1.9] exists "
              "and identifier \"com.apple.dt.Xcode\""),
        kSecCSDefaultFlags, &requirement);
  }
  if (status == errSecSuccess) {
    status = SecStaticCodeCheckValidity(code, kSecCSStrictValidate, requirement);
  }
  if (requirement) {
    CFRelease(requirement);
  }
  if (code) {
    CFRelease(code);
  }
  return status == errSecSuccess;
}

static int RegisterInstalled(NSString* version, NSString* build, NSURL* directory) {
  NSError* error = nil;
  NSArray<NSURL*>* urls =
      [NSFileManager.defaultManager contentsOfDirectoryAtURL:directory
                                  includingPropertiesForKeys:nil
                                                     options:NSDirectoryEnumerationSkipsHiddenFiles
                                                       error:&error];
  if (!urls || error) {
    return 3;
  }
  for (NSURL* url in urls) {
    if (![url.pathExtension isEqualToString:@"app"]) {
      continue;
    }
    NSBundle* bundle = [NSBundle bundleWithURL:url];
    if (![bundle.bundleIdentifier isEqualToString:@"com.apple.dt.Xcode"]) {
      continue;
    }
    NSString* candidate = bundle.infoDictionary[@"CFBundleShortVersionString"];
    NSDictionary* metadata = [NSDictionary
        dictionaryWithContentsOfURL:[url URLByAppendingPathComponent:@"Contents/version.plist"]];
    if (![candidate isKindOfClass:[NSString class]] ||
        ![metadata[@"CFBundleShortVersionString"] isKindOfClass:[NSString class]] ||
        ![metadata[@"ProductBuildVersion"] isKindOfClass:[NSString class]] ||
        ![candidate isEqualToString:version] ||
        ![metadata[@"CFBundleShortVersionString"] isEqualToString:version] ||
        ![metadata[@"ProductBuildVersion"] isEqualToString:build]) {
      continue;
    }
    if (!HasAppleSignature(url) || LSRegisterURL((__bridge CFURLRef)url, true) != noErr) {
      return 3;
    }
  }
  return 0;
}

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    if (argc != 2 && !(argc == 4 && strcmp(argv[2], "--register-installed") == 0)) {
      return 64;
    }
    NSString* version = @(argv[1]);
    if (argc == 4) {
      return RegisterInstalled(version, @(argv[3]),
                               [NSURL fileURLWithPath:@"/Applications" isDirectory:YES]);
    }
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
      // A longer version also claims Bazel's shorter alias and must retain the ambiguity check.
      if (ClaimsVersion(candidate, version)) {
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
