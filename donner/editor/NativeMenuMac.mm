#include "donner/editor/NativeMenuMac.h"

#import <AppKit/AppKit.h>

#include <string_view>
#include <utility>
#include <vector>

#include "donner/editor/EditorMenuModel.h"

extern "C" {
#include "GLFW/glfw3.h"
}

@interface DonnerNativeMenuTarget : NSObject <NSMenuDelegate>
@property(nonatomic, assign) donner::editor::NativeMenuMac* owner;
- (void)activate:(NSMenuItem*)item;
@end

@implementation DonnerNativeMenuTarget
- (void)activate:(NSMenuItem*)item {
  if (self.owner == nullptr) {
    return;
  }
  const bool keyEquivalent = NSApp.currentEvent.type == NSEventTypeKeyDown;
  self.owner->enqueueFromNative(static_cast<donner::editor::MenuBarCommand>(item.tag),
                                keyEquivalent);
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
  (void)menu;
  if (self.owner != nullptr) {
    self.owner->refreshCurrentState();
  }
}
@end

namespace donner::editor {
namespace {

NSString* ToNSString(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data()
                                  length:value.size()
                                encoding:NSUTF8StringEncoding];
}

struct KeyEquivalent {
  NSString* key = @"";
  NSEventModifierFlags modifiers = 0;
};

KeyEquivalent ParseShortcut(std::string_view shortcut) {
  KeyEquivalent result;
  if (shortcut.starts_with("Cmd+")) {
    result.modifiers |= NSEventModifierFlagCommand;
    shortcut.remove_prefix(4);
  }
  if (shortcut.starts_with("Shift+")) {
    result.modifiers |= NSEventModifierFlagShift;
    shortcut.remove_prefix(6);
  }
  if (shortcut == "Enter") {
    result.key = @"\r";
  } else if (!shortcut.empty()) {
    result.key = [ToNSString(shortcut) lowercaseString];
  }
  return result;
}

using ItemBinding = std::pair<NSMenuItem*, const EditorMenuNode*>;

void AddMenuNodes(NSMenu* menu, std::span<const EditorMenuNode> nodes,
                  DonnerNativeMenuTarget* target, std::vector<ItemBinding>* bindings) {
  menu.autoenablesItems = NO;
  menu.delegate = target;
  for (const EditorMenuNode& node : nodes) {
    if (node.kind == EditorMenuNode::Kind::Separator) {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    if (node.kind == EditorMenuNode::Kind::Submenu) {
      NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:ToNSString(node.label)
                                                    action:nil
                                             keyEquivalent:@""];
      NSMenu* submenu = [[NSMenu alloc] initWithTitle:ToNSString(node.label)];
      AddMenuNodes(submenu, node.children, target, bindings);
      item.submenu = submenu;
      [menu addItem:item];
      continue;
    }
    const KeyEquivalent shortcut = ParseShortcut(node.shortcut);
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:ToNSString(node.label)
                                                  action:@selector(activate:)
                                           keyEquivalent:shortcut.key];
    item.target = target;
    item.tag = static_cast<NSInteger>(node.command);
    item.keyEquivalentModifierMask = shortcut.modifiers;
    [menu addItem:item];
    bindings->emplace_back(item, &node);
  }
}

}  // namespace

struct NativeMenuMac::Impl {
  NSMenu* previousMenu = nil;
  NSMenu* installedMenu = nil;
  DonnerNativeMenuTarget* target = nil;
  std::vector<ItemBinding> items;
};

NativeMenuMac::NativeMenuMac() : impl_(std::make_unique<Impl>()) {}

NativeMenuMac::~NativeMenuMac() {
  if (impl_->target != nil) {
    impl_->target.owner = nullptr;
  }
  if ([NSThread isMainThread] && NSApp.mainMenu == impl_->installedMenu) {
    NSApp.mainMenu = impl_->previousMenu;
  }
}

bool NativeMenuMac::install() {
  if (![NSThread isMainThread]) {
    return false;
  }
  [NSApplication sharedApplication];
  if (impl_->installedMenu != nil) {
    return NSApp.mainMenu == impl_->installedMenu;
  }

  impl_->previousMenu = NSApp.mainMenu;
  impl_->target = [[DonnerNativeMenuTarget alloc] init];
  impl_->target.owner = this;
  NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@""];
  for (const EditorMenu& menu : EditorMenuModel()) {
    NSMenuItem* topItem = [[NSMenuItem alloc] initWithTitle:ToNSString(menu.title)
                                                     action:nil
                                              keyEquivalent:@""];
    NSMenu* submenu = [[NSMenu alloc] initWithTitle:ToNSString(menu.title)];
    AddMenuNodes(submenu, menu.items, impl_->target, &impl_->items);
    topItem.submenu = submenu;
    [mainMenu addItem:topItem];
  }
  impl_->installedMenu = mainMenu;
  NSApp.mainMenu = mainMenu;
  update(currentState_);
  return NSApp.mainMenu == mainMenu;
}

void NativeMenuMac::update(const MenuBarState& state) {
  currentState_ = state;
  if (![NSThread isMainThread] || impl_->installedMenu == nil) {
    return;
  }
  for (const auto& [item, definition] : impl_->items) {
    const EditorMenuItemPresentation presentation =
        PresentEditorMenuItem(*definition, state, EditorMenuSurface::Native);
    // Disabled commands must not consume the focused editor's key event.
    NSString* shortcut = presentation.enabled && !SuppressNativeMenuShortcuts(state)
                             ? ParseShortcut(definition->shortcut).key
                             : @"";
    if (![item.keyEquivalent isEqualToString:shortcut]) {
      item.keyEquivalent = shortcut;
    }
    item.enabled = presentation.enabled;
    item.state = presentation.checked ? NSControlStateValueOn : NSControlStateValueOff;
  }
}

void NativeMenuMac::refreshCurrentState() {
  update(currentState_);
}

void NativeMenuMac::enqueueFromNative(MenuBarCommand command, bool keyEquivalent) {
  {
    std::lock_guard lock(pendingMutex_);
    pending_.push_back(PendingCommand{command, keyEquivalent});
  }
  glfwPostEmptyEvent();
}

NativeMenuMac::DrainResult NativeMenuMac::drain(const MenuBarState& state) {
  std::deque<PendingCommand> pending;
  {
    std::lock_guard lock(pendingMutex_);
    pending.swap(pending_);
  }
  DrainResult result;
  for (const PendingCommand& activation : pending) {
    for (const auto& [item, definition] : impl_->items) {
      (void)item;
      if (definition->command != activation.command ||
          !NativeMenuActivationAllowed(*definition, state, activation.keyEquivalent)) {
        continue;
      }
      ApplyMenuBarCommand(true, activation.command, state, &result.actions);
      result.hadKeyEquivalent |= activation.keyEquivalent;
      break;
    }
  }
  return result;
}

}  // namespace donner::editor
