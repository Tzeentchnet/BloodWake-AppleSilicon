#import <AppKit/AppKit.h>
#ifdef BW_MACOS_SETTINGS_PANEL_TESTING
#import <ApplicationServices/ApplicationServices.h>
#endif

#include <stdio.h>
#include <string.h>

#include "bw_macos_settings_panel.h"

static void copy_error(char *err, size_t errlen, const char *fallback,
                       const char *detail)
{
    if (!err || !errlen)
        return;
    if (detail && *detail)
        snprintf(err, errlen, "%s", detail);
    else
        snprintf(err, errlen, "%s", fallback);
}

bw_macos_settings_apply_kind_t
bw_macos_settings_classify(const bw_setting_info_t *info)
{
    if (!info || !info->supported || !info->apply_mode ||
        !strcmp(info->apply_mode, "unsupported"))
        return BW_MACOS_SETTINGS_UNSUPPORTED;
    if (!strcmp(info->apply_mode, "live"))
        return BW_MACOS_SETTINGS_LIVE;
    if (!strcmp(info->apply_mode, "frame"))
        return BW_MACOS_SETTINGS_FRAME;
    return BW_MACOS_SETTINGS_RESTART;
}

int bw_macos_settings_save_value(const bw_setting_info_t *info,
                                 const char *value,
                                 char *err, size_t errlen)
{
    bw_macos_settings_apply_kind_t kind;
    char detail[512] = {0};

    if (err && errlen)
        err[0] = 0;
    if (!info || !info->section || !info->key || !value) {
        copy_error(err, errlen, "The setting or value is missing.", NULL);
        return BW_MACOS_SETTINGS_SAVE_INVALID;
    }
    kind = bw_macos_settings_classify(info);
    if (kind == BW_MACOS_SETTINGS_UNSUPPORTED) {
        copy_error(err, errlen, "This setting is unavailable on this Mac.",
                   info->support_reason);
        return BW_MACOS_SETTINGS_SAVE_INVALID;
    }
    if (bw_settings_validate_value(info->section, info->key, value,
                                   detail, sizeof detail) != 0) {
        copy_error(err, errlen, "The selected value is invalid.", detail);
        return BW_MACOS_SETTINGS_SAVE_INVALID;
    }
    detail[0] = 0;
    if (bw_settings_write_value(info->section, info->key, value,
                                detail, sizeof detail) != 0) {
        copy_error(err, errlen, "The settings file could not be updated.", detail);
        return BW_MACOS_SETTINGS_SAVE_WRITE_FAILED;
    }
    if (kind == BW_MACOS_SETTINGS_LIVE || kind == BW_MACOS_SETTINGS_FRAME) {
        char apply_error[512] = {0};
        if (bw_settings_runtime_apply(info->section, info->key, value,
                                      apply_error, sizeof apply_error) != 0) {
            if (err && errlen) {
                snprintf(err, errlen,
                         "Saved for the next launch, but could not apply now: %s",
                         *apply_error ? apply_error : "the runtime rejected the value");
            }
            return BW_MACOS_SETTINGS_SAVE_APPLY_FAILED;
        }
    }
    return BW_MACOS_SETTINGS_SAVE_OK;
}

static NSString *string_from_utf8(const char *text)
{
    NSString *result;
    if (!text)
        return @"";
    result = [NSString stringWithUTF8String:text];
    return result ?: @"";
}

static NSString *setting_map_key(const char *section, const char *key)
{
    return [NSString stringWithFormat:@"%@.%@",
            [string_from_utf8(section) lowercaseString],
            [string_from_utf8(key) lowercaseString]];
}

static NSDictionary<NSString *, NSString *> *persisted_values(void)
{
    const char *path = bw_settings_path();
    NSString *contents;
    NSMutableDictionary<NSString *, NSString *> *values;
    NSString *section = nil;

    if (!path || !*path)
        return @{};
    contents = [NSString stringWithContentsOfFile:string_from_utf8(path)
                                         encoding:NSUTF8StringEncoding
                                            error:NULL];
    if (!contents)
        return @{};
    values = [NSMutableDictionary dictionary];
    NSCharacterSet *space = [NSCharacterSet whitespaceAndNewlineCharacterSet];
    for (NSString *rawLine in
         [contents componentsSeparatedByCharactersInSet:
                   [NSCharacterSet newlineCharacterSet]]) {
        NSString *line = [rawLine stringByTrimmingCharactersInSet:space];
        if (!line.length || [line hasPrefix:@";"] || [line hasPrefix:@"#"])
            continue;
        if ([line hasPrefix:@"["] && [line hasSuffix:@"]"] && line.length > 2) {
            section = [[[line substringWithRange:NSMakeRange(1, line.length - 2)]
                        stringByTrimmingCharactersInSet:space] lowercaseString];
            continue;
        }
        NSRange equals = [line rangeOfString:@"="];
        if (!section.length || equals.location == NSNotFound)
            continue;
        NSString *key = [[[line substringToIndex:equals.location]
                          stringByTrimmingCharactersInSet:space] lowercaseString];
        NSString *value = [[line substringFromIndex:equals.location + 1]
                           stringByTrimmingCharactersInSet:space];
        if (key.length)
            values[[NSString stringWithFormat:@"%@.%@", section, key]] = value;
    }
    return values;
}

static BOOL valid_value(const bw_setting_info_t *info, NSString *value)
{
    char error[256];
    return value.length &&
           bw_settings_validate_value(info->section, info->key,
                                      value.UTF8String, error, sizeof error) == 0;
}

static NSString *current_value(const bw_setting_info_t *info,
                               NSDictionary<NSString *, NSString *> *saved)
{
    bw_macos_settings_apply_kind_t kind = bw_macos_settings_classify(info);
    char value[128] = {0};
    char error[256] = {0};
    NSString *candidate;

    if ((kind == BW_MACOS_SETTINGS_LIVE || kind == BW_MACOS_SETTINGS_FRAME) &&
        bw_settings_runtime_get(info->section, info->key, value, sizeof value,
                                error, sizeof error) == 0) {
        candidate = string_from_utf8(value);
        if (valid_value(info, candidate))
            return candidate;
    }
    candidate = saved[setting_map_key(info->section, info->key)];
    if (valid_value(info, candidate))
        return candidate;
    candidate = string_from_utf8(info->default_value);
    return valid_value(info, candidate) ? candidate : @"";
}

static NSString *base_status(const bw_setting_info_t *info)
{
    switch (bw_macos_settings_classify(info)) {
    case BW_MACOS_SETTINGS_LIVE:
        return @"Live — applies immediately";
    case BW_MACOS_SETTINGS_FRAME:
        return @"Frame-safe — applies next frame";
    case BW_MACOS_SETTINGS_RESTART:
        return @"Restart required";
    case BW_MACOS_SETTINGS_UNSUPPORTED:
        if (info->support_reason && *info->support_reason)
            return [NSString stringWithFormat:@"Unavailable — %@",
                    string_from_utf8(info->support_reason)];
        return @"Unavailable on this Mac";
    }
    return @"Restart required";
}

@interface BWFlippedView : NSView
@end

@implementation BWFlippedView
- (BOOL)isFlipped
{
    return YES;
}
@end

@interface BWSettingRow : NSObject {
@public
    bw_setting_info_t info;
}
@property(nonatomic) NSUInteger settingIndex;
@property(nonatomic, strong) NSPopUpButton *button;
@property(nonatomic, strong) NSTextField *status;
@property(nonatomic, copy) NSString *currentValue;
@property(nonatomic, copy) NSString *normalStatus;
@end

@implementation BWSettingRow
@end

@interface BWSettingsPanelController : NSObject <NSWindowDelegate>
@property(nonatomic, strong) NSPanel *panel;
@property(nonatomic, strong) NSMutableArray<BWSettingRow *> *rows;
@property(nonatomic, strong) id keyMonitor;
#ifdef BW_MACOS_SETTINGS_PANEL_TESTING
@property(nonatomic, strong) NSAlert *testAlert;
#endif
- (BOOL)openPanel;
- (void)settingChanged:(NSPopUpButton *)sender;
@end

@implementation BWSettingsPanelController

- (instancetype)init
{
    self = [super init];
    if (self)
        _rows = [NSMutableArray array];
    return self;
}

- (NSTextField *)textLabel:(NSString *)text
{
    NSTextField *label = [NSTextField labelWithString:text];
    label.selectable = NO;
    return label;
}

- (void)addTabHeading:(NSString *)title toStack:(NSStackView *)stack
{
    NSTextField *heading = [self textLabel:title];
    heading.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    [stack addArrangedSubview:heading];
}

- (void)addSettingAtIndex:(NSUInteger)index
                    info:(const bw_setting_info_t *)settingInfo
                  values:(NSDictionary<NSString *, NSString *> *)values
                 toStack:(NSStackView *)stack
{
    BWSettingRow *row = [[BWSettingRow alloc] init];
    row->info = *settingInfo;
    row.settingIndex = index;
    row.currentValue = current_value(settingInfo, values);
    row.normalStatus = base_status(settingInfo);

    NSTextField *name = [self textLabel:string_from_utf8(settingInfo->label)];
    name.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    name.toolTip = string_from_utf8(settingInfo->tip);
    [name.widthAnchor constraintEqualToConstant:185].active = YES;

    NSPopUpButton *button = [[NSPopUpButton alloc] initWithFrame:NSZeroRect
                                                       pullsDown:NO];
    for (NSUInteger optionIndex = 0;
         optionIndex < settingInfo->option_count; optionIndex++) {
        bw_setting_option_info_t option;
        if (bw_settings_get_option(index, optionIndex, &option) != 0)
            continue;
        [button addItemWithTitle:string_from_utf8(option.label)];
        button.lastItem.representedObject = string_from_utf8(option.value);
        if ([row.currentValue isEqualToString:button.lastItem.representedObject])
            [button selectItem:button.lastItem];
    }
    button.enabled =
        bw_macos_settings_classify(settingInfo) != BW_MACOS_SETTINGS_UNSUPPORTED;
    button.toolTip = button.enabled ? string_from_utf8(settingInfo->tip)
                                    : row.normalStatus;
    button.target = self;
    button.action = @selector(settingChanged:);
    button.tag = (NSInteger)self.rows.count;
    [button.widthAnchor constraintEqualToConstant:220].active = YES;
    row.button = button;

    NSTextField *status = [self textLabel:row.normalStatus];
    status.textColor =
        bw_macos_settings_classify(settingInfo) == BW_MACOS_SETTINGS_UNSUPPORTED
            ? NSColor.disabledControlTextColor
            : NSColor.secondaryLabelColor;
    status.lineBreakMode = NSLineBreakByWordWrapping;
    status.maximumNumberOfLines = 3;
    [status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                     forOrientation:NSLayoutConstraintOrientationHorizontal];
    row.status = status;

    NSStackView *line = [NSStackView stackViewWithViews:@[name, button, status]];
    line.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    line.alignment = NSLayoutAttributeCenterY;
    line.spacing = 12;
    line.toolTip = string_from_utf8(settingInfo->tip);
    [stack addArrangedSubview:line];
    [line.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;

    NSBox *separator = [[NSBox alloc] initWithFrame:NSZeroRect];
    separator.boxType = NSBoxSeparator;
    [stack addArrangedSubview:separator];
    [separator.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    [self.rows addObject:row];
}

- (void)showError:(NSString *)message
{
    NSBeep();
    if (!self.panel || self.panel.attachedSheet)
        return;
    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = @"The setting was not applied";
    alert.informativeText = message.length ? message : @"An unknown settings error occurred.";
#ifdef BW_MACOS_SETTINGS_PANEL_TESTING
    self.testAlert = alert;
#endif
    [alert beginSheetModalForWindow:self.panel completionHandler:nil];
}

- (void)selectValue:(NSString *)value inButton:(NSPopUpButton *)button
{
    for (NSMenuItem *item in button.itemArray)
        if ([item.representedObject isEqualToString:value]) {
            [button selectItem:item];
            return;
        }
}

- (void)settingChanged:(NSPopUpButton *)sender
{
    if (sender.tag < 0 || (NSUInteger)sender.tag >= self.rows.count)
        return;
    BWSettingRow *row = self.rows[(NSUInteger)sender.tag];
    NSString *value = sender.selectedItem.representedObject;
    char error[768] = {0};
    int result = bw_macos_settings_save_value(&row->info, value.UTF8String,
                                              error, sizeof error);

    if (result == BW_MACOS_SETTINGS_SAVE_OK) {
        row.currentValue = value;
        switch (bw_macos_settings_classify(&row->info)) {
        case BW_MACOS_SETTINGS_LIVE:
            row.status.stringValue = @"Saved and applied immediately";
            break;
        case BW_MACOS_SETTINGS_FRAME:
            row.status.stringValue = @"Saved — queued for the next frame";
            break;
        case BW_MACOS_SETTINGS_RESTART:
            row.status.stringValue = @"Saved — restart required";
            break;
        case BW_MACOS_SETTINGS_UNSUPPORTED:
            row.status.stringValue = row.normalStatus;
            break;
        }
        row.status.textColor = NSColor.systemGreenColor;
        return;
    }

    if (result == BW_MACOS_SETTINGS_SAVE_APPLY_FAILED) {
        row.currentValue = value;
        row.status.stringValue = @"Saved, but live apply failed";
    } else {
        [self selectValue:row.currentValue inButton:sender];
        row.status.stringValue = row.normalStatus;
    }
    row.status.textColor = NSColor.systemRedColor;
    [self showError:string_from_utf8(error)];
}

- (BOOL)openPanel
{
    if (self.panel) {
        [self.panel makeKeyAndOrderFront:nil];
        return YES;
    }

    [NSApplication sharedApplication];
    [self.rows removeAllObjects];

    NSPanel *panel =
        [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 760, 620)
                                  styleMask:(NSWindowStyleMaskTitled |
                                             NSWindowStyleMaskClosable |
                                             NSWindowStyleMaskResizable |
                                             NSWindowStyleMaskUtilityWindow)
                                    backing:NSBackingStoreBuffered
                                      defer:NO];
    panel.title = @"Blood Wake Settings";
    panel.delegate = self;
    panel.floatingPanel = YES;
    panel.level = NSFloatingWindowLevel;
    panel.hidesOnDeactivate = NO;
    panel.minSize = NSMakeSize(700, 420);
    panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    self.panel = panel;

    __weak BWSettingsPanelController *weakSelf = self;
    self.keyMonitor =
        [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown |
                                                       NSEventMaskKeyUp)
                                               handler:^NSEvent *(NSEvent *event) {
        if (event.keyCode != 122)
            return event;
        if (event.type == NSEventTypeKeyDown && !event.isARepeat)
            [weakSelf.panel close];
        return nil;
    }];

    NSView *content = panel.contentView;
    NSTextField *notice =
        [self textLabel:@"The game continues while Settings is open. Guest keyboard and "
                         "mouse input are released; the game is not paused."];
    notice.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    notice.textColor = NSColor.secondaryLabelColor;
    notice.lineBreakMode = NSLineBreakByWordWrapping;
    notice.maximumNumberOfLines = 2;
    notice.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:notice];

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.borderType = NSBezelBorder;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:scroll];

    BWFlippedView *document = [[BWFlippedView alloc] initWithFrame:NSZeroRect];
    document.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.documentView = document;

    NSStackView *stack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 8;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [document addSubview:stack];

    NSDictionary<NSString *, NSString *> *values = persisted_values();
    NSString *lastTab = nil;
    size_t count = bw_settings_count();
    for (size_t index = 0; index < count; index++) {
        bw_setting_info_t info;
        if (bw_settings_get_info(index, &info) != 0)
            continue;
        NSString *tab = string_from_utf8(info.tab);
        if (![tab isEqualToString:lastTab]) {
            [self addTabHeading:tab toStack:stack];
            lastTab = tab;
        }
        [self addSettingAtIndex:index info:&info values:values toStack:stack];
    }
    if (!self.rows.count) {
        NSTextField *empty = [self textLabel:@"No settings are available."];
        [stack addArrangedSubview:empty];
    }

    [NSLayoutConstraint activateConstraints:@[
        [notice.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:18],
        [notice.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-18],
        [notice.topAnchor constraintEqualToAnchor:content.topAnchor constant:16],
        [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:16],
        [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16],
        [scroll.topAnchor constraintEqualToAnchor:notice.bottomAnchor constant:12],
        [scroll.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-16],
        [document.leadingAnchor constraintEqualToAnchor:scroll.contentView.leadingAnchor],
        [document.trailingAnchor constraintEqualToAnchor:scroll.contentView.trailingAnchor],
        [document.topAnchor constraintEqualToAnchor:scroll.contentView.topAnchor],
        [document.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:document.leadingAnchor constant:12],
        [stack.trailingAnchor constraintEqualToAnchor:document.trailingAnchor constant:-12],
        [stack.topAnchor constraintEqualToAnchor:document.topAnchor constant:12],
        [stack.bottomAnchor constraintEqualToAnchor:document.bottomAnchor constant:-12],
    ]];

    [panel center];
    [NSApp activateIgnoringOtherApps:YES];
    [panel makeKeyAndOrderFront:nil];
    return panel.visible;
}

- (void)windowWillClose:(NSNotification *)notification
{
    if (notification.object == self.panel) {
        if (self.keyMonitor) {
            [NSEvent removeMonitor:self.keyMonitor];
            self.keyMonitor = nil;
        }
        self.panel.delegate = nil;
        self.panel = nil;
        [self.rows removeAllObjects];
#ifdef BW_MACOS_SETTINGS_PANEL_TESTING
        self.testAlert = nil;
#endif
    }
}

@end

static BWSettingsPanelController *s_controller;

#ifdef BW_MACOS_SETTINGS_PANEL_TESTING
static bw_macos_settings_panel_test_lifecycle_fn s_test_lifecycle;
static void *s_test_lifecycle_context;

static void test_copy_string(char *out, size_t out_len, NSString *value)
{
    if (!out || !out_len)
        return;
    snprintf(out, out_len, "%s", value.UTF8String ?: "");
}

static BWSettingRow *test_find_row(const char *section, const char *key)
{
    if (!section || !key)
        return nil;
    for (BWSettingRow *row in s_controller.rows)
        if (!strcmp(row->info.section, section) &&
            !strcmp(row->info.key, key))
            return row;
    return nil;
}

static BOOL test_view_contains(NSView *view, NSString *text)
{
    if ([view isKindOfClass:[NSTextField class]] &&
        [[(NSTextField *)view stringValue] containsString:text])
        return YES;
    for (NSView *child in view.subviews)
        if (test_view_contains(child, text))
            return YES;
    return NO;
}

static void test_notify_lifecycle(int open)
{
    if (s_test_lifecycle)
        s_test_lifecycle(open, s_test_lifecycle_context);
}

void bw_macos_settings_panel_test_set_lifecycle_callback(
    bw_macos_settings_panel_test_lifecycle_fn callback, void *context)
{
    s_test_lifecycle = callback;
    s_test_lifecycle_context = context;
}

int bw_macos_settings_panel_test_window_server_available(void)
{
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    CGDirectDisplayID display = CGMainDisplayID();
    int available = session && display != kCGNullDirectDisplay &&
                    CGDisplayIsActive(display);
    if (session)
        CFRelease(session);
    return available;
}

int bw_macos_settings_panel_test_state(
    bw_macos_settings_panel_test_state_t *out)
{
    @autoreleasepool {
        if (!out || ![NSThread isMainThread])
            return -1;
        memset(out, 0, sizeof *out);
        out->controller_created = s_controller != nil;
        out->panel_created =
            [s_controller.panel isKindOfClass:[NSPanel class]];
        out->visible = s_controller.panel.visible;
        out->main_thread = [NSThread isMainThread];
        out->row_count = s_controller.rows.count;
        out->no_pause_notice =
            test_view_contains(s_controller.panel.contentView,
                               @"the game is not paused");
        return 0;
    }
}

int bw_macos_settings_panel_test_row(
    const char *section, const char *key,
    bw_macos_settings_panel_test_row_t *out)
{
    @autoreleasepool {
        BWSettingRow *row;
        if (!out || ![NSThread isMainThread])
            return -1;
        row = test_find_row(section, key);
        if (!row)
            return -1;
        memset(out, 0, sizeof *out);
        out->enabled = row.button.enabled;
        out->apply_kind = bw_macos_settings_classify(&row->info);
        test_copy_string(out->value, sizeof out->value,
                         row.button.selectedItem.representedObject);
        test_copy_string(out->status, sizeof out->status,
                         row.status.stringValue);
        snprintf(out->support_reason, sizeof out->support_reason, "%s",
                 row->info.support_reason ?: "");
        return 0;
    }
}

int bw_macos_settings_panel_test_select(const char *section, const char *key,
                                        const char *value)
{
    @autoreleasepool {
        BWSettingRow *row;
        NSString *selected;
        if (!value || ![NSThread isMainThread])
            return -1;
        row = test_find_row(section, key);
        if (!row || !row.button.enabled)
            return -1;
        selected = string_from_utf8(value);
        [s_controller selectValue:selected inButton:row.button];
        if (![row.button.selectedItem.representedObject
                isEqualToString:selected])
            return -1;
        [s_controller settingChanged:row.button];
        return 0;
    }
}

int bw_macos_settings_panel_test_visible_error(char *message,
                                               size_t message_len)
{
    @autoreleasepool {
        NSAlert *alert;
        if (![NSThread isMainThread])
            return 0;
        alert = s_controller.testAlert;
        if (!alert || s_controller.panel.attachedSheet != alert.window)
            return 0;
        test_copy_string(message, message_len, alert.informativeText);
        return 1;
    }
}

void bw_macos_settings_panel_test_dismiss_error(void)
{
    @autoreleasepool {
        NSWindow *sheet;
        if (![NSThread isMainThread])
            return;
        sheet = s_controller.panel.attachedSheet;
        if (sheet) {
            [s_controller.panel endSheet:sheet
                              returnCode:NSModalResponseCancel];
            [sheet orderOut:nil];
        }
        s_controller.testAlert = nil;
    }
}
#endif

int bw_macos_settings_panel_toggle(void)
{
    @autoreleasepool {
        int result;
        if (![NSThread isMainThread])
            return -1;
        if (!s_controller)
            s_controller = [[BWSettingsPanelController alloc] init];
        if (s_controller.panel.visible) {
            [s_controller.panel close];
#ifdef BW_MACOS_SETTINGS_PANEL_TESTING
            test_notify_lifecycle(0);
#endif
            return 0;
        }
        result = [s_controller openPanel] ? 1 : -1;
#ifdef BW_MACOS_SETTINGS_PANEL_TESTING
        if (result > 0)
            test_notify_lifecycle(1);
#endif
        return result;
    }
}

int bw_macos_settings_panel_is_open(void)
{
    @autoreleasepool {
        if (![NSThread isMainThread])
            return 0;
        return s_controller.panel.visible ? 1 : 0;
    }
}

void bw_macos_settings_panel_close(void)
{
    @autoreleasepool {
        if (![NSThread isMainThread])
            return;
        if (s_controller.panel)
            [s_controller.panel close];
    }
}
