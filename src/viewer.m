/*
 * HauppaugeRecorder for macOS: live preview and recording for the Hauppauge HD PVR.
 *
 * The control bar under the preview selects the video and audio inputs, the
 * encoder bitrate and the folder, and starts/stops recording the box's own
 * H.264 transport stream (.ts). The engine is app_core.c, shared with the
 * Windows and Linux apps.
 */
#import <AVFoundation/AVFoundation.h>
#import <AudioUnit/AudioUnit.h>
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>

#include "app_core.h"
#include "hdpvr.h"

/* ---- AudioComponent / AudioUnit calls, looked up at run time ----
 * Current SDKs bind these functions to AudioToolbox, but older macOS (10.9 and 10.10) exports
 * them from AudioUnit.framework instead, so a direct call cannot be bound there and dyld kills
 * the app the first time one runs. Look them up by name, wherever this macOS keeps them. */
#include <dlfcn.h>

static void *au_sym(const char *name)
{
    static void *au_lib;
    static int tried;
    if (!tried) {
        tried = 1;
        au_lib = dlopen("/System/Library/Frameworks/AudioUnit.framework/AudioUnit", RTLD_LAZY | RTLD_GLOBAL);
    }
    void *f = au_lib ? dlsym(au_lib, name) : NULL;
    return f ? f : dlsym(RTLD_DEFAULT, name);
}

typedef AudioComponent (*au_find_next_t)(AudioComponent, const AudioComponentDescription *);
typedef OSStatus (*au_instance_new_t)(AudioComponent, AudioComponentInstance *);
typedef OSStatus (*au_unit_fn_t)(AudioUnit); /* Dispose, Initialize, OutputUnitStart/Stop */
typedef OSStatus (*au_set_property_t)(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void *,
                                      UInt32);

static AudioComponent au_find_next(AudioComponent c, const AudioComponentDescription *d)
{
    static au_find_next_t f;
    if (!f)
        f = (au_find_next_t)au_sym("AudioComponentFindNext");
    return f ? f(c, d) : NULL;
}
static OSStatus au_instance_new(AudioComponent c, AudioComponentInstance *out)
{
    static au_instance_new_t f;
    if (!f)
        f = (au_instance_new_t)au_sym("AudioComponentInstanceNew");
    return f ? f(c, out) : kAudio_UnimplementedError;
}
static OSStatus au_set_property(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el,
                                const void *data, UInt32 size)
{
    static au_set_property_t f;
    if (!f)
        f = (au_set_property_t)au_sym("AudioUnitSetProperty");
    return f ? f(u, id, sc, el, data, size) : kAudio_UnimplementedError;
}
static OSStatus au_unit_call(const char *name, au_unit_fn_t *cache, AudioUnit u)
{
    if (!*cache)
        *cache = (au_unit_fn_t)au_sym(name);
    return *cache ? (*cache)(u) : kAudio_UnimplementedError;
}
static OSStatus au_instance_dispose(AudioComponentInstance u)
{
    static au_unit_fn_t f;
    return au_unit_call("AudioComponentInstanceDispose", &f, u);
}
static OSStatus au_initialize(AudioUnit u)
{
    static au_unit_fn_t f;
    return au_unit_call("AudioUnitInitialize", &f, u);
}
static OSStatus au_output_start(AudioUnit u)
{
    static au_unit_fn_t f;
    return au_unit_call("AudioOutputUnitStart", &f, u);
}
static OSStatus au_output_stop(AudioUnit u)
{
    static au_unit_fn_t f;
    return au_unit_call("AudioOutputUnitStop", &f, u);
}


/* ---- AppKit helpers: the modern convenience constructors need 10.12+, SF Symbols 11+ ---- */

/* ---- 10.9 compatibility: the API added in 10.10 and 10.11 is used only where it exists ---- */

static NSColor *label_color(BOOL secondary)
{
    if (@available(macOS 10.10, *))
        return secondary ? [NSColor secondaryLabelColor] : [NSColor labelColor];
    return secondary ? NSColor.disabledControlTextColor : NSColor.controlTextColor;
}

static NSFont *mono_font(CGFloat size, BOOL semibold)
{
    if (@available(macOS 10.11, *))
        return [NSFont monospacedDigitSystemFontOfSize:size
                                                weight:semibold ? NSFontWeightSemibold : NSFontWeightMedium];
    return [NSFont userFixedPitchFontOfSize:size];
}

/* layout anchors and -setActive: need 10.11 / 10.10, so constraints are built the 10.7 way */
static NSLayoutConstraint *mk(id a, NSLayoutAttribute aa, NSLayoutRelation rel, id b, NSLayoutAttribute ba,
                              CGFloat mult, CGFloat c)
{
    return [NSLayoutConstraint constraintWithItem:a attribute:aa relatedBy:rel toItem:b
                                        attribute:b ? ba : NSLayoutAttributeNotAnAttribute
                                       multiplier:mult constant:c];
}

static void set_active(NSLayoutConstraint *c, BOOL on)
{
    if (!c)
        return;
    if (@available(macOS 10.10, *)) {
        c.active = on;
        return;
    }
    id a = c.firstItem, b = c.secondItem;
    NSView *owner = b ? [a ancestorSharedWithView:b] : a;
    if (on)
        [owner addConstraint:c];
    else
        [owner removeConstraint:c];
}

static void activate_all(NSArray<NSLayoutConstraint *> *cs)
{
    for (NSLayoutConstraint *c in cs)
        set_active(c, YES);
}

static NSTextField *make_label(NSString *s)
{
    if (@available(macOS 10.12, *))
        return [NSTextField labelWithString:s];
    NSTextField *l = [NSTextField new];
    l.stringValue = s;
    l.bezeled = NO;
    l.drawsBackground = NO;
    l.editable = NO;
    l.selectable = NO;
    l.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    l.textColor = label_color(NO);
    [l.cell setLineBreakMode:NSLineBreakByClipping];
    return l;
}

static char kCheckbox;

static NSButton *make_button(NSString *title, id target, SEL action, BOOL checkbox)
{
    if (@available(macOS 10.12, *)) {
        NSButton *b = checkbox ? [NSButton checkboxWithTitle:title target:target action:action]
                               : [NSButton buttonWithTitle:title target:target action:action];
        if (checkbox)
            objc_setAssociatedObject(b, &kCheckbox, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return b;
    }
    NSButton *b = [NSButton new];
    b.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    b.title = title;
    b.target = target;
    b.action = action;
    if (checkbox) {
        objc_setAssociatedObject(b, &kCheckbox, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [b setButtonType:NSButtonTypeSwitch];
    } else {
        [b setButtonType:NSButtonTypeMomentaryPushIn];
        b.bezelStyle = NSBezelStyleRounded;
    }
    return b;
}

static void set_symbol(NSButton *b, NSString *name, NSString *desc)
{
    if (@available(macOS 11.0, *)) {
        b.image = [NSImage imageWithSystemSymbolName:name accessibilityDescription:desc];
        b.imagePosition = NSImageLeading;
    }
}

/*
 * The control bar is laid out by this small row/column view rather than NSStackView: 10.9's NSStackView
 * sizes and clips its views differently from 10.10 and later, and there it left the bar with no room
 * under the preview. A Box measures its views (their intrinsic sizes, or the limits set with
 * limit_size) and places them by frame, which works the same on every macOS since 10.7.
 */
static char kSizeLimits;

/* min and max width (0: none) and a fixed height (0: its own) for a view in a Box */
static void limit_size(NSView *v, CGFloat minw, CGFloat maxw, CGFloat h)
{
    objc_setAssociatedObject(v, &kSizeLimits, @[ @(minw), @(maxw), @(h) ], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

@interface Box : NSView
@property(nonatomic) NSUserInterfaceLayoutOrientation orientation; /* rows center their views, columns align them left */
@property(nonatomic) CGFloat spacing;
@property(nonatomic) NSEdgeInsets edgeInsets;
@property(nonatomic, weak) NSView *flexible; /* in a row: gets the width the others leave */
- (void)setGap:(CGFloat)gap afterView:(NSView *)v;
- (NSSize)naturalSize;
- (void)place;
- (void)refit; /* after a view's title changes */
@end

static NSSize natural_size(NSView *v)
{
    if ([v isKindOfClass:Box.class])
        return [(Box *)v naturalSize];
    NSSize s = v.intrinsicContentSize; /* -1 is NSViewNoIntrinsicMetric, a variable only since 10.11 */
    /* a label's cell adds the few points of padding its intrinsic width leaves out */
    if ([v isKindOfClass:NSTextField.class])
        s.width = MAX(s.width, [[(NSTextField *)v cell] cellSize].width);
    /* 10.9 to 10.11 measure checkbox and button titles a few points short, which cut off their last
     * letter ("Widescree"): measure the title itself and add the box or bezel around it */
    if ([v isKindOfClass:NSPopUpButton.class]) {
        s.width = MAX(s.width, [[(NSPopUpButton *)v cell] cellSize].width);
    } else if ([v isKindOfClass:NSButton.class]) {
        NSButton *b = (NSButton *)v;
        BOOL box = objc_getAssociatedObject(b, &kCheckbox) != nil;
        CGFloat title = ceil([b.attributedTitle size].width);
        CGFloat image = b.image && b.imagePosition != NSNoImage ? b.image.size.width + 4 : 0;
        s.width = MAX(s.width, MAX([b.cell cellSize].width, title + image + (box ? 24 : 28)));
    }
    if (s.width < 0)
        s.width = v.frame.size.width;
    if (s.height < 0)
        s.height = v.frame.size.height;
    NSArray<NSNumber *> *lim = objc_getAssociatedObject(v, &kSizeLimits);
    if (lim) {
        s.width = MAX(s.width, lim[0].doubleValue);
        if (lim[1].doubleValue > 0)
            s.width = MIN(s.width, lim[1].doubleValue);
        if (lim[2].doubleValue > 0)
            s.height = lim[2].doubleValue;
    }
    return NSMakeSize(ceil(s.width), ceil(s.height));
}

@implementation Box {
    NSMutableDictionary<NSValue *, NSNumber *> *_gaps;
}
- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        _orientation = NSUserInterfaceLayoutOrientationHorizontal;
        _spacing = 8;
        _gaps = [NSMutableDictionary new];
    }
    return self;
}
- (BOOL)isFlipped { return YES; }
- (void)setGap:(CGFloat)gap afterView:(NSView *)v
{
    _gaps[[NSValue valueWithNonretainedObject:v]] = @(gap);
}
- (CGFloat)gapAfter:(NSView *)v
{
    NSNumber *g = _gaps[[NSValue valueWithNonretainedObject:v]];
    return g ? g.doubleValue : _spacing;
}
- (BOOL)horizontal { return _orientation == NSUserInterfaceLayoutOrientationHorizontal; }
- (NSSize)naturalSize
{
    CGFloat along = 0, across = 0;
    NSArray<NSView *> *views = self.subviews;
    for (NSView *v in views) {
        NSSize s = natural_size(v);
        if (v == _flexible)
            s.width = 0;
        along += self.horizontal ? s.width : s.height;
        across = MAX(across, self.horizontal ? s.height : s.width);
        if (v != views.lastObject)
            along += [self gapAfter:v];
    }
    NSEdgeInsets in = _edgeInsets;
    return self.horizontal ? NSMakeSize(along + in.left + in.right, across + in.top + in.bottom)
                           : NSMakeSize(across + in.left + in.right, along + in.top + in.bottom);
}
/* the bar keeps the window at least as wide as its rows (the flexible label aside) */
- (NSSize)intrinsicContentSize
{
    return self.naturalSize;
}
- (void)place
{
    NSEdgeInsets in = _edgeInsets;
    NSRect r = NSMakeRect(in.left, in.top, MAX(0, self.bounds.size.width - in.left - in.right),
                          MAX(0, self.bounds.size.height - in.top - in.bottom));
    NSArray<NSView *> *views = self.subviews;
    CGFloat extra = r.size.width;
    if (self.horizontal) {
        for (NSView *v in views) {
            if (v != _flexible)
                extra -= natural_size(v).width;
            if (v != views.lastObject)
                extra -= [self gapAfter:v];
        }
    }
    CGFloat pos = self.horizontal ? NSMinX(r) : NSMinY(r);
    for (NSView *v in views) {
        NSSize s = natural_size(v);
        NSRect f;
        if (self.horizontal) {
            if (v == _flexible)
                s.width = MAX(0, extra);
            f = NSMakeRect(pos, NSMinY(r) + round((r.size.height - s.height) / 2), s.width, s.height);
            pos += s.width;
        } else {
            if ([v isKindOfClass:Box.class])
                s.width = r.size.width; /* a row in a column gets the column's width */
            f = NSMakeRect(NSMinX(r), pos, s.width, s.height);
            pos += s.height;
        }
        pos += [self gapAfter:v];
        v.frame = f;
        if ([v isKindOfClass:Box.class])
            [(Box *)v place];
    }
}
- (void)layout
{
    [super layout];
    [self place];
}
- (void)refit
{
    [self invalidateIntrinsicContentSize];
    self.needsLayout = YES;
}
- (void)resizeSubviewsWithOldSize:(NSSize)old
{
    (void)old;
    [self place];
}
@end

static Box *stack(NSArray<NSView *> *views)
{
    Box *b = [[Box alloc] initWithFrame:NSZeroRect];
    for (NSView *v in views) {
        v.translatesAutoresizingMaskIntoConstraints = YES;
        v.autoresizingMask = NSViewNotSizable;
        [b addSubview:v];
    }
    return b;
}

static NSPopUpButton *make_popup(const char *const *names, int n, id target, SEL action)
{
    NSPopUpButton *p = [NSPopUpButton new];
    for (int i = 0; i < n; i++)
        [p addItemWithTitle:[NSString stringWithUTF8String:names[i]]];
    p.target = target;
    p.action = action;
    return p;
}

/* ---- preview view: keeps the picture at its aspect inside whatever space it has ---- */

@interface HDPreview : NSView
@property(nonatomic, strong) CALayer *picture;
@property(nonatomic, strong) NSTextField *badge, *message;
@property(nonatomic) double aspect;
@end

@implementation HDPreview
- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        _aspect = 16.0 / 9;
        self.wantsLayer = YES;
        self.layer.backgroundColor = NSColor.blackColor.CGColor;
        _picture = [CALayer layer];
        _picture.contentsGravity = kCAGravityResize;
        _picture.backgroundColor = NSColor.blackColor.CGColor;
        [self.layer addSublayer:_picture];

        _message = make_label(@"");
        _message.textColor = [NSColor colorWithWhite:0.75 alpha:1];
        _message.font = [NSFont systemFontOfSize:15];
        _message.alignment = NSTextAlignmentCenter;
        [self addSubview:_message];

        _badge = make_label(@"");
        _badge.font = mono_font(13, YES);
        _badge.textColor = NSColor.whiteColor;
        _badge.backgroundColor = [NSColor colorWithRed:0.8 green:0.05 blue:0.05 alpha:0.85];
        _badge.drawsBackground = YES;
        _badge.hidden = YES;
        [self addSubview:_badge];
    }
    return self;
}
- (BOOL)isFlipped { return YES; }
- (void)setAspect:(double)aspect
{
    if (aspect == _aspect)
        return;
    _aspect = aspect;
    [self setNeedsLayout:YES];
}
- (void)layout
{
    [super layout];
    NSSize s = self.bounds.size;
    CGFloat w = s.width, h = w / _aspect;
    if (h > s.height) {
        h = s.height;
        w = h * _aspect;
    }
    NSRect r = NSMakeRect((s.width - w) / 2, (s.height - h) / 2, w, h);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _picture.frame = r;
    [CATransaction commit];
    [_badge sizeToFit];
    _badge.frame = NSMakeRect(NSMinX(r) + 10, NSMinY(r) + 10, _badge.frame.size.width + 8, 20);
    [_message sizeToFit];
    NSSize m = _message.frame.size;
    _message.frame = NSMakeRect((s.width - m.width) / 2, (s.height - m.height) / 2, m.width, m.height);
}
- (void)mouseDown:(NSEvent *)e
{
    if (e.clickCount == 2)
        [self.window toggleFullScreen:nil];
}
@end

/* ---- app ---- */

static app_t *g_app;

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate {
    NSWindow *_window;
    HDPreview *_preview;
    NSLayoutConstraint *_aspectConstraint;
    double _aspect;
    id _engine; /* AVAudioEngine, 10.15 and later only */
    AudioUnit _outputUnit; /* monitor output before 10.15 (no AVAudioSourceNode) */

    int _fw, _fh;   /* display link thread only */
    uint64_t _seq;
    CVDisplayLinkRef _link;
    volatile int _showing; /* a frame is on its way to the main thread */

    NSPopUpButton *_inputPopup, *_audioPopup, *_qualityPopup;
    NSButton *_palCheck, *_wideCheck, *_monitorCheck, *_recordButton, *_folderButton;
    Box *_bar;
    NSTextField *_timeLabel, *_statusLabel;
    NSLevelIndicator *_meterL, *_meterR;
    NSTimer *_frameTimer, *_uiTimer;
    NSURL *_folder;
    BOOL _finishing;
    double _level[2];
    NSString *_savedMessage; /* "Saved ..." shown in the status line for a few seconds */
    NSDate *_savedUntil;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    NSUserDefaults *ud = NSUserDefaults.standardUserDefaults;
    [ud registerDefaults:@{ @"input" : @0, @"audio" : @0, @"pal" : @NO, @"widescreen" : @NO, @"quality" : @0,
                            @"monitor" : @YES }];
    NSString *folder = [ud stringForKey:@"folder"];
    _folder = folder ? [NSURL fileURLWithPath:folder]
                     : [NSFileManager.defaultManager URLsForDirectory:NSMoviesDirectory
                                                            inDomains:NSUserDomainMask].firstObject;
    for (NSString *a in NSProcessInfo.processInfo.arguments) {
        if ([a isEqualToString:@"-v"]) hdpvr_set_verbose(1);
        if ([a isEqualToString:@"-vv"]) hdpvr_set_verbose(2);
    }

    [self buildMenu];
    [self buildWindow];
    g_app = app_start((int)[ud integerForKey:@"input"], (int)[ud integerForKey:@"audio"],
                      (int)[ud integerForKey:@"quality"], [ud boolForKey:@"pal"]);
    app_set_widescreen(g_app, [ud boolForKey:@"widescreen"]);
    app_set_monitor(g_app, [ud boolForKey:@"monitor"]);
    [self startAudio];
    [self tick];

    /* The display link asks for a frame on every screen refresh, so a new picture lands on the refresh it
     * is due for. A timer drifts against the refresh and every so often shows a frame twice. */
    if (CVDisplayLinkCreateWithActiveCGDisplays(&_link) == kCVReturnSuccess &&
        CVDisplayLinkSetOutputCallback(_link, display_link_cb, (__bridge void *)self) == kCVReturnSuccess &&
        CVDisplayLinkStart(_link) == kCVReturnSuccess) {
        [self followScreen];
    } else {
        if (_link)
            CVDisplayLinkRelease(_link);
        _link = NULL;
        _frameTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 target:self selector:@selector(pollFrame)
                                                     userInfo:nil repeats:YES];
        [NSRunLoop.mainRunLoop addTimer:_frameTimer forMode:NSRunLoopCommonModes];
    }
    _uiTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 20 target:self selector:@selector(tick)
                                              userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:_uiTimer forMode:NSRunLoopCommonModes];
}

- (NSTextField *)label:(NSString *)s
{
    NSTextField *l = make_label(s);
    l.textColor = label_color(YES);
    return l;
}

- (NSLevelIndicator *)meter
{
    NSLevelIndicator *m = [NSLevelIndicator new];
    [(NSLevelIndicatorCell *)m.cell setLevelIndicatorStyle:NSLevelIndicatorStyleContinuousCapacity];
    m.minValue = 0;
    m.maxValue = 60; /* dBFS + 60 */
    m.warningValue = 48;
    m.criticalValue = 57;
    limit_size(m, 90, 90, 8);
    return m;
}

- (void)buildWindow
{
    NSUserDefaults *ud = NSUserDefaults.standardUserDefaults;
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 960, 540 + 90)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.delegate = self;
    _window.title = @APP_NAME;
    _window.frameAutosaveName = @"HDPVRCaptureWindow";
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;

    _preview = [[HDPreview alloc] initWithFrame:NSZeroRect];
    _preview.translatesAutoresizingMaskIntoConstraints = NO;

    /* row 1: source and recording settings */
    _inputPopup = make_popup(app_video_names, 3, self, @selector(sourceChanged:));
    [_inputPopup selectItemAtIndex:[ud integerForKey:@"input"]];
    _audioPopup = make_popup(app_audio_names, 3, self, @selector(sourceChanged:));
    [_audioPopup selectItemAtIndex:[ud integerForKey:@"audio"]];
    _qualityPopup = make_popup(app_quality_names, APP_QUALITIES, self, @selector(sourceChanged:));
    [_qualityPopup selectItemAtIndex:[ud integerForKey:@"quality"]];

    _palCheck = make_button(@"PAL", self, @selector(sourceChanged:), YES);
    _palCheck.state = [ud boolForKey:@"pal"] ? NSControlStateValueOn : NSControlStateValueOff;
    _palCheck.toolTip = @"50 Hz on the S-Video and composite inputs.";

    _wideCheck = make_button(@"Widescreen", self, @selector(widescreenChanged:), YES);
    _wideCheck.state = [ud boolForKey:@"widescreen"] ? NSControlStateValueOn : NSControlStateValueOff;
    _wideCheck.toolTip = @"Show standard-definition pictures at 16:9 in the preview and snapshots.";

    _folderButton = make_button(@"", self, @selector(chooseFolder:), NO);
    _folderButton.bezelStyle = NSBezelStyleRounded;
    set_symbol(_folderButton, @"folder", @"Folder");
    limit_size(_folderButton, 0, 200, 0);
    [self updateFolderButton];

    Box *row1 = stack(@[
        [self label:@"Input"], _inputPopup, _palCheck, [self label:@"Audio"], _audioPopup, _wideCheck,
        [self label:@"Quality"], _qualityPopup, [self label:@"Save to"], _folderButton
    ]);
    row1.spacing = 8;
    [row1 setGap:16 afterView:_palCheck];
    [row1 setGap:16 afterView:_wideCheck];
    [row1 setGap:16 afterView:_qualityPopup];

    /* row 2: record button, time, meters, monitor, status */
    _recordButton = make_button(@"Record", self, @selector(toggleRecord:), NO);
    _recordButton.bezelStyle = NSBezelStyleRounded;
    set_symbol(_recordButton, @"record.circle", nil);
    if (@available(macOS 10.14, *))
        _recordButton.contentTintColor = NSColor.systemRedColor;
    _recordButton.keyEquivalent = @"r";
    _recordButton.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    limit_size(_recordButton, 110, 0, 0);

    _timeLabel = make_label(@"00:00:00");
    _timeLabel.font = mono_font(15, NO);

    _meterL = [self meter];
    _meterR = [self meter];
    Box *meters = stack(@[
        stack(@[ [self label:@"L"], _meterL ]),
        stack(@[ [self label:@"R"], _meterR ])
    ]);
    meters.orientation = NSUserInterfaceLayoutOrientationVertical;
    meters.spacing = 2;

    _monitorCheck = make_button(@"Play audio", self, @selector(monitorChanged:), YES);
    _monitorCheck.state = [ud boolForKey:@"monitor"] ? NSControlStateValueOn : NSControlStateValueOff;

    _statusLabel = make_label(@"");
    _statusLabel.textColor = label_color(YES);
    [_statusLabel.cell setLineBreakMode:NSLineBreakByTruncatingMiddle];

    Box *row2 =
        stack(@[ _recordButton, _timeLabel, meters, _monitorCheck, _statusLabel ]);
    row2.flexible = _statusLabel;
    row2.spacing = 16;

    Box *bar = stack(@[ row1, row2 ]);
    bar.orientation = NSUserInterfaceLayoutOrientationVertical;
    bar.spacing = 8;
    bar.edgeInsets = NSEdgeInsetsMake(10, 12, 10, 12);
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    _bar = bar;

    NSView *content = [NSView new];
    [content addSubview:_preview];
    [content addSubview:bar];
    activate_all(@[
        mk(_preview, NSLayoutAttributeTop, NSLayoutRelationEqual, content, NSLayoutAttributeTop, 1, 0),
        mk(_preview, NSLayoutAttributeLeading, NSLayoutRelationEqual, content, NSLayoutAttributeLeading, 1, 0),
        mk(_preview, NSLayoutAttributeTrailing, NSLayoutRelationEqual, content, NSLayoutAttributeTrailing, 1, 0),
        mk(bar, NSLayoutAttributeTop, NSLayoutRelationEqual, _preview, NSLayoutAttributeBottom, 1, 0),
        mk(bar, NSLayoutAttributeLeading, NSLayoutRelationEqual, content, NSLayoutAttributeLeading, 1, 0),
        mk(bar, NSLayoutAttributeTrailing, NSLayoutRelationEqual, content, NSLayoutAttributeTrailing, 1, 0),
        mk(bar, NSLayoutAttributeBottom, NSLayoutRelationEqual, content, NSLayoutAttributeBottom, 1, 0),
        mk(_preview, NSLayoutAttributeWidth, NSLayoutRelationGreaterThanOrEqual, nil, 0, 1, 720),
    ]);
    [bar setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationVertical];
    [bar setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                  forOrientation:NSLayoutConstraintOrientationHorizontal];
    [bar setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                  forOrientation:NSLayoutConstraintOrientationVertical];
    [self applyAspect:16.0 / 9];
    _window.contentView = content;
    [self updatePal];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)buildMenu
{
    NSMenu *bar = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Quit " APP_NAME action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem *fileItem = [NSMenuItem new];
    [bar addItem:fileItem];
    NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
    [file addItemWithTitle:@"Start/Stop Recording" action:@selector(toggleRecord:) keyEquivalent:@"r"];
    [file addItemWithTitle:@"Save Snapshot" action:@selector(snapshot:) keyEquivalent:@"s"];
    [file addItem:[NSMenuItem separatorItem]];
    [file addItemWithTitle:@"Choose Recording Folder…" action:@selector(chooseFolder:) keyEquivalent:@""];
    [file addItemWithTitle:@"Show Recordings in Finder" action:@selector(revealFolder:) keyEquivalent:@""];
    [file addItem:[NSMenuItem separatorItem]];
    [file addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    fileItem.submenu = file;

    NSMenuItem *viewItem = [NSMenuItem new];
    [bar addItem:viewItem];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"]
        .keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    viewItem.submenu = view;
    NSApp.mainMenu = bar;
}

- (void)updateFolderButton
{
    _folderButton.title = _folder.lastPathComponent ?: @"Movies";
    [_bar refit];
    _folderButton.toolTip = _folder.path;
}

- (void)updatePal
{
    _palCheck.enabled = _inputPopup.indexOfSelectedItem != APP_VIDEO_COMPONENT && !(g_app && app_is_recording(g_app));
}

/* the window grows and shrinks around the picture's aspect */
- (void)applyAspect:(double)aspect
{
    if (aspect == _aspect)
        return;
    BOOL first = _aspect == 0;
    _aspect = aspect;
    _preview.aspect = aspect;
    set_active(_aspectConstraint, NO);
    _aspectConstraint = mk(_preview, NSLayoutAttributeHeight, NSLayoutRelationEqual, _preview, NSLayoutAttributeWidth, 1 / aspect, 0);
    _aspectConstraint.priority = NSLayoutPriorityDefaultHigh; /* full screen may not fit it exactly */
    set_active(_aspectConstraint, YES);
    if (first || (_window.styleMask & NSWindowStyleMaskFullScreen))
        return;
    /* keep the picture's height and change its width, within the screen */
    NSRect vis = _window.screen.visibleFrame;
    CGFloat bar = _window.contentView.bounds.size.height - _preview.bounds.size.height;
    CGFloat h = _preview.bounds.size.height, w = MAX(720, h * aspect);
    NSRect fr = [_window frameRectForContentRect:NSMakeRect(0, 0, w, w / aspect + bar)];
    if (fr.size.width > vis.size.width || fr.size.height > vis.size.height) {
        CGFloat s = MIN(vis.size.width / fr.size.width, vis.size.height / fr.size.height);
        w *= s;
        fr = [_window frameRectForContentRect:NSMakeRect(0, 0, w, w / aspect + bar)];
    }
    NSRect cur = _window.frame;
    fr.origin = NSMakePoint(NSMidX(cur) - fr.size.width / 2, NSMaxY(cur) - fr.size.height);
    fr.origin.x = MAX(NSMinX(vis), MIN(fr.origin.x, NSMaxX(vis) - fr.size.width));
    fr.origin.y = MAX(NSMinY(vis), MIN(fr.origin.y, NSMaxY(vis) - fr.size.height));
    [_window setFrame:fr display:YES animate:NO];
}

/* ---- controls ---- */

- (void)sourceChanged:(id)sender
{
    NSUserDefaults *ud = NSUserDefaults.standardUserDefaults;
    [ud setInteger:_inputPopup.indexOfSelectedItem forKey:@"input"];
    [ud setInteger:_audioPopup.indexOfSelectedItem forKey:@"audio"];
    [ud setInteger:_qualityPopup.indexOfSelectedItem forKey:@"quality"];
    [ud setBool:_palCheck.state == NSControlStateValueOn forKey:@"pal"];
    app_set_source(g_app, (int)_inputPopup.indexOfSelectedItem, (int)_audioPopup.indexOfSelectedItem,
                   (int)_qualityPopup.indexOfSelectedItem, _palCheck.state == NSControlStateValueOn);
    [self updatePal];
}

- (void)widescreenChanged:(id)sender
{
    BOOL on = _wideCheck.state == NSControlStateValueOn;
    [NSUserDefaults.standardUserDefaults setBool:on forKey:@"widescreen"];
    app_set_widescreen(g_app, on);
}

- (void)monitorChanged:(id)sender
{
    BOOL on = _monitorCheck.state == NSControlStateValueOn;
    app_set_monitor(g_app, on);
    [NSUserDefaults.standardUserDefaults setBool:on forKey:@"monitor"];
}

- (void)chooseFolder:(id)sender
{
    if (app_is_recording(g_app))
        return;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = YES;
    p.canChooseFiles = NO;
    p.canCreateDirectories = YES;
    p.prompt = @"Choose";
    p.directoryURL = _folder;
    [p beginSheetModalForWindow:_window completionHandler:^(NSModalResponse res) {
        if (res != NSModalResponseOK)
            return;
        self->_folder = p.URL;
        [NSUserDefaults.standardUserDefaults setObject:p.URL.path forKey:@"folder"];
        [self updateFolderButton];
    }];
}

- (void)revealFolder:(id)sender
{
    [NSWorkspace.sharedWorkspace openURL:_folder];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item.action == @selector(toggleRecord:)) {
        item.title = app_is_recording(g_app) ? @"Stop Recording" : @"Start Recording";
        return !_finishing && (app_is_recording(g_app) || app_state(g_app) == APP_STREAMING);
    }
    if (item.action == @selector(snapshot:))
        return app_state(g_app) == APP_STREAMING;
    return YES;
}

- (void)setStatus:(NSString *)s
{
    _statusLabel.stringValue = s ?: @"";
}

/* ---- recording ---- */

- (void)toggleRecord:(id)sender
{
    if (_finishing)
        return;
    if (app_is_recording(g_app)) {
        [self stopRecording];
        return;
    }
    char path[1100], err[300] = "";
    if (app_record_start(g_app, _folder.fileSystemRepresentation, path, sizeof(path), err, sizeof(err))) {
        NSAlert *a = [NSAlert new];
        a.messageText = @"Cannot start recording";
        a.informativeText = [NSString stringWithUTF8String:err];
        [a beginSheetModalForWindow:_window completionHandler:nil];
        return;
    }
    _recordButton.title = @"Stop";
    [_bar refit];
    set_symbol(_recordButton, @"stop.circle.fill", nil);
    _inputPopup.enabled = _audioPopup.enabled = _qualityPopup.enabled = _folderButton.enabled = NO;
    [self updatePal];
    _preview.badge.hidden = NO;
}

static void rec_done(void *user, int ok, const char *path)
{
    AppDelegate *self = (__bridge AppDelegate *)user;
    NSString *p = [NSString stringWithUTF8String:path];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self recordingFinished:ok path:p];
    });
}

- (void)stopRecording
{
    _finishing = YES;
    _recordButton.enabled = NO;
    _recordButton.title = @"Finishing…";
    [_bar refit];
    app_record_stop(g_app, rec_done, (__bridge void *)self);
}

- (void)recordingFinished:(int)ok path:(NSString *)path
{
    _finishing = NO;
    _recordButton.enabled = YES;
    _recordButton.title = @"Record";
    [_bar refit];
    set_symbol(_recordButton, @"record.circle", nil);
    _inputPopup.enabled = _audioPopup.enabled = _qualityPopup.enabled = _folderButton.enabled = YES;
    [self updatePal];
    _preview.badge.hidden = YES;
    if (ok) {
        _savedMessage = [@"Saved " stringByAppendingString:path.lastPathComponent];
        [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:path] ]];
    } else {
        _savedMessage = @"Recording failed: the file could not be written";
    }
    _savedUntil = [NSDate dateWithTimeIntervalSinceNow:8];
}

/* ---- frames and the UI timer ---- */

static void free_frame(void *info, const void *data, size_t size)
{
    (void)info, (void)size;
    free((void *)data);
}

static CVReturn display_link_cb(CVDisplayLinkRef link, const CVTimeStamp *now, const CVTimeStamp *out,
                                CVOptionFlags flags, CVOptionFlags *flags_out, void *ctx)
{
    (void)link, (void)now, (void)out, (void)flags, (void)flags_out;
    @autoreleasepool {
        [(__bridge AppDelegate *)ctx pollFrame];
    }
    return kCVReturnSuccess;
}

- (void)windowDidChangeScreen:(NSNotification *)note
{
    (void)note;
    [self followScreen];
}

/* the display link follows the refresh of the screen the window is on */
- (void)followScreen
{
    NSNumber *screen = _window.screen.deviceDescription[@"NSScreenNumber"];
    if (_link && screen)
        CVDisplayLinkSetCurrentCGDisplay(_link, (CGDirectDisplayID)screen.unsignedIntValue);
}

/* On the display link's thread (or the fallback timer): takes the frame that is due, if it is new, and
 * hands it to the main thread as an image. */
- (void)pollFrame
{
    if (!g_app || __atomic_load_n(&_showing, __ATOMIC_ACQUIRE))
        return; /* the main thread is still busy with the last one */
    int w, h;
    app_frame_size(g_app, &w, &h);
    if (w != _fw || h != _fh) {
        _fw = w;
        _fh = h;
        _seq = 0;
    }
    size_t bytes = (size_t)w * h * 4;
    uint32_t *buf = malloc(bytes);
    if (!buf)
        return;
    if (!app_get_frame(g_app, buf, w, h, &_seq)) {
        free(buf);
        return;
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, buf, bytes, free_frame);
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                                   kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst, dp, NULL,
                                   false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    if (!img)
        return;
    __atomic_store_n(&_showing, 1, __ATOMIC_RELEASE);
    dispatch_async(dispatch_get_main_queue(), ^{
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        self->_preview.picture.contents = (__bridge id)img;
        [CATransaction commit];
        CGImageRelease(img);
        __atomic_store_n(&self->_showing, 0, __ATOMIC_RELEASE);
    });
}

- (void)tick
{
    double db[2];
    app_peaks(g_app, db);
    for (int c = 0; c < 2; c++)
        _level[c] = MAX(db[c] + 60, _level[c] - 1.5); /* a little decay so peaks stay visible */
    _meterL.doubleValue = _level[0];
    _meterR.doubleValue = _level[1];

    [self applyAspect:app_aspect(g_app)];
    app_state_t st = app_state(g_app);
    BOOL rec = app_is_recording(g_app);
    _recordButton.enabled = !_finishing && (rec || st == APP_STREAMING);

    char status[600];
    app_status(g_app, status, sizeof(status));
    NSString *s = [NSString stringWithUTF8String:status];
    _preview.message.stringValue = st == APP_STREAMING ? @"" : s;
    _preview.message.hidden = st == APP_STREAMING;
    int w, h, fps;
    _window.title = app_signal(g_app, &w, &h, &fps)
                        ? [NSString stringWithFormat:@APP_NAME " — %@ %d×%d", _inputPopup.titleOfSelectedItem, w, h]
                        : @APP_NAME;

    if (rec) {
        int t = (int)app_record_seconds(g_app);
        _timeLabel.stringValue = [NSString stringWithFormat:@"%02d:%02d:%02d", t / 3600, t / 60 % 60, t % 60];
        _preview.badge.stringValue = [NSString stringWithFormat:@"● REC %@", _timeLabel.stringValue];
        [_preview setNeedsLayout:YES];
        s = [NSString stringWithFormat:@"Recording %.1f MB — %@", app_record_bytes(g_app) / 1e6, s];
    } else if (!_finishing) {
        _timeLabel.stringValue = @"00:00:00";
        if (_savedMessage && _savedUntil.timeIntervalSinceNow > 0)
            s = _savedMessage;
    } else {
        s = @"Finishing the file…";
    }
    [self setStatus:s];
    [_preview setNeedsLayout:YES];
}

/* ---- snapshot ---- */

- (void)snapshot:(id)sender
{
    char path[1100], err[300];
    if (app_snapshot(g_app, _folder.fileSystemRepresentation, path, sizeof(path), err, sizeof(err)))
        _savedMessage = [NSString stringWithUTF8String:err];
    else
        _savedMessage = [@"Saved " stringByAppendingString:[NSString stringWithUTF8String:path].lastPathComponent];
    _savedUntil = [NSDate dateWithTimeIntervalSinceNow:5];
}

/* ---- audio monitoring ---- */

static OSStatus output_unit_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                                   UInt32 bus, UInt32 n, AudioBufferList *abl)
{
    (void)ref, (void)ts, (void)bus;
    if (!app_audio_render_float(g_app, abl->mBuffers[0].mData, abl->mBuffers[1].mData, (int)n))
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
}

- (void)startAudio
{
    if (@available(macOS 10.15, *)) {
        _engine = [AVAudioEngine new];
        AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
        AVAudioSourceNode *src = [[AVAudioSourceNode alloc]
            initWithFormat:fmt
               renderBlock:^OSStatus(BOOL *silence, const AudioTimeStamp *ts, AVAudioFrameCount n,
                                     AudioBufferList *abl) {
                   (void)ts;
                   *silence = !app_audio_render_float(g_app, abl->mBuffers[0].mData, abl->mBuffers[1].mData, (int)n);
                   return noErr;
               }];
        [_engine attachNode:src];
        NSError *err = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" /* the error: variant needs macOS 27 */
        [_engine connect:src to:((AVAudioEngine *)_engine).mainMixerNode format:fmt];
#pragma clang diagnostic pop
        if (![_engine startAndReturnError:&err])
            NSLog(@"audio output: %@", err.localizedDescription);
        return;
    }

    /* macOS 10.11-10.14: the default output unit, which converts the rate itself */
    AudioComponentDescription desc = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                       kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = au_find_next(NULL, &desc);
    OSStatus st = comp ? au_instance_new(comp, &_outputUnit) : -1;
    if (st == noErr) {
        AudioStreamBasicDescription f = {
            .mSampleRate = 48000,
            .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved,
            .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4,
            .mChannelsPerFrame = 2, .mBitsPerChannel = 32,
        };
        AURenderCallbackStruct cb = { output_unit_render, NULL };
        st = au_set_property(_outputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f,
                                  sizeof(f));
        if (st == noErr)
            st = au_set_property(_outputUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                                      0, &cb, sizeof(cb));
        if (st == noErr)
            st = au_initialize(_outputUnit);
        if (st == noErr)
            st = au_output_start(_outputUnit);
    }
    if (st != noErr) {
        if (_outputUnit)
            au_instance_dispose(_outputUnit);
        _outputUnit = NULL;
        NSLog(@"audio output failed (error %d)", (int)st);
    }
}

/* ---- shutdown ---- */

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app
{
    if (!g_app || !app_is_recording(g_app))
        return NSTerminateNow;
    NSAlert *a = [NSAlert new];
    a.messageText = @"A recording is in progress";
    a.informativeText = @"Stop the recording and quit?";
    [a addButtonWithTitle:@"Stop and Quit"];
    [a addButtonWithTitle:@"Cancel"];
    return [a runModal] == NSAlertFirstButtonReturn ? NSTerminateNow : NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }

- (void)applicationWillTerminate:(NSNotification *)note
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
    [_frameTimer invalidate];
    [_uiTimer invalidate];
    if (@available(macOS 10.15, *))
        [(AVAudioEngine *)_engine stop];
    if (_outputUnit) {
        au_output_stop(_outputUnit);
        au_instance_dispose(_outputUnit);
        _outputUnit = NULL;
    }
    app_close(g_app); /* finishes a recording still in progress */
    g_app = NULL;
}
@end

int main(void)
{
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *delegate = [AppDelegate new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
