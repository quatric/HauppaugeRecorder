/*
 * HauppaugeRecorder for old Macs: Mac OS X 10.3 on PowerPC and 10.4 on Intel,
 * up to 10.8 (10.9 and later run the regular app, viewer.m).
 *
 * The same engine (app_core.c finds and runs the HD PVR, records its
 * transport stream untouched and decodes it with ffmpeg for the preview and
 * the sound) under a Cocoa window written for the oldest AppKit: manual
 * retain/release, no blocks, no Auto Layout, no layers. The picture goes to
 * an OpenGL view, the sound to Core Audio's default output (audio_mac_legacy.c).
 *
 * Recording only copies the box's own H.264 to disk, which any of these Macs
 * can do. Decoding it for the preview is heavy: on PowerPC the preview of HD
 * signals is smaller and drops frames, but the recording is unaffected.
 */
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>
#include <string.h>

#include "app_core.h"
#include "hdpvr.h"

#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

static app_t *g_app;

static int pot(int v)
{
    int p = 64;
    while (p < v)
        p *= 2;
    return p;
}

/* ---- preview ---- */

@interface PreviewView : NSOpenGLView {
    int _w, _h, _tw, _th; /* frame and texture size */
    uint32_t *_buf;
    uint64_t _seq;
    GLuint _tex;
    BOOL _glReady, _haveFrame;
    double _aspect;
}
- (void)setAspect:(double)aspect;
- (void)poll;
- (void)clear;
- (void)drawPicture;
@end

@implementation PreviewView
- (id)initWithFrame:(NSRect)frame
{
    NSOpenGLPixelFormatAttribute attrs[] = { NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, 0 };
    NSOpenGLPixelFormat *pf = [[[NSOpenGLPixelFormat alloc] initWithAttributes:attrs] autorelease];
    if ((self = [super initWithFrame:frame pixelFormat:pf]))
        _aspect = 16.0 / 9;
    return self;
}

- (void)dealloc
{
    free(_buf);
    [super dealloc];
}

- (BOOL)isOpaque { return YES; }

- (void)setAspect:(double)aspect
{
    if (aspect == _aspect)
        return;
    _aspect = aspect;
    [self setNeedsDisplay:YES];
}

- (void)clear
{
    if (!_haveFrame)
        return;
    _haveFrame = NO;
    [self setNeedsDisplay:YES];
}

- (void)setupGL
{
    [[self openGLContext] makeCurrentContext];
    GLint one = 1; /* swap on the screen's refresh, so frames are not torn */
    [[self openGLContext] setValues:&one forParameter:NSOpenGLCPSwapInterval];
    glGenTextures(1, &_tex);
    _glReady = YES;
}

- (void)poll
{
    if (!g_app || ![self window] || ![[self window] isVisible])
        return;
    if (!_glReady)
        [self setupGL];
    int w, h;
    app_frame_size(g_app, &w, &h);
    if (w != _w || h != _h || !_buf) {
        free(_buf);
        _buf = calloc((size_t)w * h, 4);
        _w = w;
        _h = h;
        _seq = 0;
    }
    if (!_buf || !app_get_frame(g_app, _buf, w, h, &_seq))
        return;
    [[self openGLContext] makeCurrentContext];
    glBindTexture(GL_TEXTURE_2D, _tex);
    if (pot(w) != _tw || pot(h) != _th) {
        _tw = pot(w);
        _th = pot(h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, _tw, _th, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    }
    /* the frames are 32-bit 0x00RRGGBB in the CPU's byte order, which is what this type reads on both CPUs */
    glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, _buf);
    _haveFrame = YES;
    [self drawPicture];
}

- (void)drawPicture
{
    NSRect b = [self bounds];
    float W = (float)b.size.width, H = (float)b.size.height;
    glViewport(0, 0, (GLsizei)W, (GLsizei)H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (_haveFrame && _tw && W > 0 && H > 0) {
        float ar = (float)_aspect, w = W, h = W / ar;
        if (h > H) {
            h = H;
            w = H * ar;
        }
        float x0 = -w / W, x1 = w / W, y0 = -h / H, y1 = h / H;
        float u = (float)_w / _tw, v = (float)_h / _th;
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, _tex);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glBegin(GL_QUADS);
        glTexCoord2f(0, v); glVertex2f(x0, y0);
        glTexCoord2f(u, v); glVertex2f(x1, y0);
        glTexCoord2f(u, 0); glVertex2f(x1, y1);
        glTexCoord2f(0, 0); glVertex2f(x0, y1);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }
    [[self openGLContext] flushBuffer];
}

- (void)drawRect:(NSRect)r
{
    (void)r;
    if (!_glReady)
        [self setupGL];
    [[self openGLContext] makeCurrentContext];
    [self drawPicture];
}
@end

/* ---- level meters (NSLevelIndicator needs 10.4) ---- */

@interface MeterView : NSView {
    float _v[2];
}
- (void)setLeft:(float)l right:(float)r;
@end

@implementation MeterView
- (void)setLeft:(float)l right:(float)r
{
    if (l == _v[0] && r == _v[1])
        return;
    _v[0] = l;
    _v[1] = r;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)rect
{
    (void)rect;
    NSRect b = [self bounds];
    float bar = (float)floor((b.size.height - 3) / 2);
    int c;
    for (c = 0; c < 2; c++) {
        NSRect r = NSMakeRect(0, c ? 0 : bar + 3, b.size.width, bar);
        [[NSColor colorWithCalibratedWhite:0.2f alpha:1] set];
        NSRectFill(r);
        float v = _v[c];
        NSRect f = r;
        f.size.width = (float)floor(r.size.width * (v < 0.8f ? v : 0.8f));
        [[NSColor colorWithCalibratedRed:0.2f green:0.8f blue:0.2f alpha:1] set];
        NSRectFill(f);
        if (v > 0.8f) {
            f.origin.x = r.size.width * 0.8f;
            f.size.width = (float)floor(r.size.width * ((v < 0.95f ? v : 0.95f) - 0.8f));
            [[NSColor colorWithCalibratedRed:0.95f green:0.8f blue:0.1f alpha:1] set];
            NSRectFill(f);
        }
        if (v > 0.95f) {
            f.origin.x = r.size.width * 0.95f;
            f.size.width = (float)floor(r.size.width * (v - 0.95f));
            [[NSColor colorWithCalibratedRed:0.9f green:0.15f blue:0.1f alpha:1] set];
            NSRectFill(f);
        }
    }
}
@end

/* ---- the window ---- */

@interface Controller : NSObject {
    NSWindow *_window;
    PreviewView *_preview;
    NSView *_bar;
    NSPopUpButton *_input, *_audio, *_quality;
    NSPopUpButton *_stopAfter;
    NSButton *_pal, *_wide, *_previewCheck, *_monitor, *_record, *_folderButton;
    NSTextField *_time, *_status;
    MeterView *_meters;
    NSTimer *_timer;
    NSString *_folder, *_saved;
    NSDate *_savedUntil;
    BOOL _finishing;
    int _ticks;
    double _level[2];
}
- (NSTextField *)label:(NSString *)s;
- (void)layoutBar;
@end

static NSTextField *make_label(NSString *s)
{
    NSTextField *l = [[[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 10, 17)] autorelease];
    [l setStringValue:s];
    [l setBezeled:NO];
    [l setDrawsBackground:NO];
    [l setEditable:NO];
    [l setSelectable:NO];
    [l setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
    [l sizeToFit];
    return l;
}

static NSButton *make_button(NSString *title, id target, SEL action, BOOL checkbox)
{
    NSButton *b = [[[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 10, 24)] autorelease];
    [b setButtonType:checkbox ? NSSwitchButton : NSMomentaryPushInButton];
    if (!checkbox)
        [b setBezelStyle:NSRoundedBezelStyle];
    [b setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
    [b setTitle:title];
    [b setTarget:target];
    [b setAction:action];
    [b sizeToFit];
    return b;
}

static NSPopUpButton *make_popup(const char *const *names, int n, id target, SEL action)
{
    NSPopUpButton *p = [[[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 10, 26) pullsDown:NO] autorelease];
    int i;
    [p setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
    for (i = 0; i < n; i++)
        [p addItemWithTitle:[NSString stringWithUTF8String:names[i]]];
    [p setTarget:target];
    [p setAction:action];
    [p sizeToFit];
    return p;
}

/* sizes a control to its content, with a few points spare (text measured exactly can lose its last letter) */
static void fit(NSView *v, float minw, float maxw)
{
    if ([v respondsToSelector:@selector(sizeToFit)])
        [(NSControl *)v sizeToFit];
    NSRect f = [v frame];
    f.size.width = (float)ceil(f.size.width) + 4;
    if (f.size.width < minw)
        f.size.width = minw;
    if (maxw > 0 && f.size.width > maxw)
        f.size.width = maxw;
    [v setFrame:f];
}

/* places views left to right, vertically centred on a row, from x; returns where it ended */
static float place_row(NSArray *views, float x, float y, float h, float gap)
{
    unsigned i;
    for (i = 0; i < [views count]; i++) {
        NSView *v = [views objectAtIndex:i];
        if ([v isKindOfClass:[NSNumber class]]) {
            x += [(NSNumber *)v floatValue];
            continue;
        }
        NSRect f = [v frame];
        f.origin.x = x;
        f.origin.y = (float)floor(y + (h - f.size.height) / 2);
        [v setFrame:f];
        x += f.size.width + gap;
    }
    return x;
}

static void rec_done(void *user, int ok, const char *path);

@implementation Controller

- (NSUserDefaults *)ud { return [NSUserDefaults standardUserDefaults]; }

- (NSTextField *)label:(NSString *)s
{
    NSArray *subs = [_bar subviews];
    unsigned i;
    for (i = 0; i < [subs count]; i++) {
        id v = [subs objectAtIndex:i];
        if ([v isKindOfClass:[NSTextField class]] && v != _time && v != _status && [[v stringValue] isEqualToString:s])
            return v;
    }
    NSTextField *l = make_label(s);
    [_bar addSubview:l];
    return l;
}

- (void)layoutBar
{
    NSRect b = [_bar bounds];
    NSArray *row1 = [NSArray arrayWithObjects:[self label:@"Input"], _input, _pal, [NSNumber numberWithFloat:8],
                                              [self label:@"Audio"], _audio, _wide, _previewCheck,
                                              [NSNumber numberWithFloat:8],
                                              [self label:@"Quality"], _quality, [NSNumber numberWithFloat:8],
                                              [self label:@"Save to"], _folderButton, nil];
    NSArray *row2 = [NSArray arrayWithObjects:_record, _time, _meters, _monitor, [self label:@"Stop after"], _stopAfter, nil];
    unsigned i;
    for (i = 0; i < [row1 count]; i++) {
        id v = [row1 objectAtIndex:i];
        if (v != _folderButton && ![v isKindOfClass:[NSNumber class]])
            fit(v, 0, 0);
    }
    fit(_folderButton, 60, 200);
    fit(_record, 100, 0);
    fit(_time, 0, 0);
    fit(_monitor, 0, 0);
    fit([self label:@"Stop after"], 0, 0);
    fit(_stopAfter, 0, 0);
    place_row(row1, 12, 44, 26, 6);
    float x = place_row(row2, 12, 8, 28, 12);
    NSRect sf = [_status frame];
    sf.origin.x = x;
    sf.origin.y = 13;
    sf.size.width = b.size.width - x - 12;
    [_status setFrame:sf];
}

- (void)buildMenu
{
    NSMenu *bar = [[[NSMenu alloc] initWithTitle:@""] autorelease];
    NSMenuItem *appItem = [[[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""] autorelease];
    NSMenu *appMenu = [[[NSMenu alloc] initWithTitle:@APP_NAME] autorelease];
    [appMenu addItemWithTitle:@"Hide " APP_NAME action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit " APP_NAME action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem *fileItem = [[[NSMenuItem alloc] initWithTitle:@"File" action:NULL keyEquivalent:@""] autorelease];
    NSMenu *file = [[[NSMenu alloc] initWithTitle:@"File"] autorelease];
    [[file addItemWithTitle:@"Start/Stop Recording" action:@selector(toggleRecord:) keyEquivalent:@"r"] setTarget:self];
    [[file addItemWithTitle:@"Save Snapshot" action:@selector(snapshot:) keyEquivalent:@"s"] setTarget:self];
    [file addItem:[NSMenuItem separatorItem]];
    [[file addItemWithTitle:@"Choose Recording Folder..." action:@selector(chooseFolder:) keyEquivalent:@""]
        setTarget:self];
    [[file addItemWithTitle:@"Show Recordings in Finder" action:@selector(revealFolder:) keyEquivalent:@""]
        setTarget:self];
    [file addItem:[NSMenuItem separatorItem]];
    [file addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [fileItem setSubmenu:file];
    [bar addItem:fileItem];

    [NSApp setMainMenu:bar];
    if ([NSApp respondsToSelector:@selector(setAppleMenu:)]) /* 10.3 needs it to treat the first menu as the app menu */
        [NSApp performSelector:@selector(setAppleMenu:) withObject:appMenu];
}

- (void)updateFolderButton
{
    [_folderButton setTitle:[[NSFileManager defaultManager] displayNameAtPath:_folder]];
    [_folderButton setToolTip:_folder];
    [self layoutBar];
}

- (void)setStatus:(NSString *)s
{
    if (![[_status stringValue] isEqualToString:s ? s : @""])
        [_status setStringValue:s ? s : @""];
}

- (void)updateEnabled
{
    BOOL rec = app_is_recording(g_app) || _finishing;
    [_input setEnabled:!rec];
    [_audio setEnabled:!rec];
    [_quality setEnabled:!rec];
    [_folderButton setEnabled:!rec];
    [_pal setEnabled:!rec && [_input indexOfSelectedItem] != APP_VIDEO_COMPONENT];
}

- (void)buildWindow
{
    NSUserDefaults *ud = [self ud];
    int barH = 78;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 960, [[NSScreen mainScreen] visibleFrame].size.height - 60 < 540 + barH ? [[NSScreen mainScreen] visibleFrame].size.height - 60 : 540 + barH)
                                          styleMask:NSTitledWindowMask | NSClosableWindowMask |
                                                    NSMiniaturizableWindowMask | NSResizableWindowMask
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [_window setDelegate:self];
    [_window setTitle:@APP_NAME];
    [_window setMinSize:NSMakeSize(900, 300 + barH)];
    NSView *content = [_window contentView];

    _preview = [[[PreviewView alloc] initWithFrame:NSMakeRect(0, barH, 960, 540)] autorelease];
    [_preview setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [content addSubview:_preview];
    _bar = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 960, barH)] autorelease];
    [_bar setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
    [content addSubview:_bar];

    _input = make_popup(app_video_names, 3, self, @selector(sourceChanged:));
    [_input selectItemAtIndex:[ud integerForKey:@"input"]];
    _pal = make_button(@"PAL", self, @selector(sourceChanged:), YES);
    [_pal setState:[ud boolForKey:@"pal"] ? NSOnState : NSOffState];
    [_pal setToolTip:@"50 Hz on the S-Video and composite inputs."];
    _audio = make_popup(app_audio_names, 3, self, @selector(sourceChanged:));
    [_audio selectItemAtIndex:[ud integerForKey:@"audio"]];
    _wide = make_button(@"Widescreen", self, @selector(widescreenChanged:), YES);
    [_wide setState:[ud boolForKey:@"widescreen"] ? NSOnState : NSOffState];
    [_wide setToolTip:@"Show standard-definition pictures at 16:9 in the preview and snapshots."];
    _quality = make_popup(app_quality_names, APP_QUALITIES, self, @selector(sourceChanged:));
    [_quality selectItemAtIndex:[ud integerForKey:@"quality"]];
    _folderButton = make_button(@"Movies", self, @selector(chooseFolder:), NO);

    _record = make_button(@"Record", self, @selector(toggleRecord:), NO);
    [_record setKeyEquivalent:@"r"];
    [_record setKeyEquivalentModifierMask:NSCommandKeyMask];
    _time = make_label(@"00:00:00");
    [_time setFont:[NSFont userFixedPitchFontOfSize:14]];
    _meters = [[[MeterView alloc] initWithFrame:NSMakeRect(0, 0, 90, 15)] autorelease];
    _monitor = make_button(@"Play audio", self, @selector(monitorChanged:), YES);
    [_monitor setState:[ud boolForKey:@"monitor"] ? NSOnState : NSOffState];
    _stopAfter = make_popup(app_stop_after_names, APP_STOP_AFTERS, self, @selector(stopAfterChanged:));
    [_stopAfter selectItemAtIndex:[ud integerForKey:@"stopAfter"] >= 0 && [ud integerForKey:@"stopAfter"] < APP_STOP_AFTERS ? [ud integerForKey:@"stopAfter"] : 0];
    [_stopAfter setToolTip:@"Stop the recording by itself after this long."];
    app_set_stop_after(g_app, (int)[_stopAfter indexOfSelectedItem]);
    _previewCheck = make_button(@"Preview", self, @selector(previewChanged:), YES);
    [_previewCheck setState:[ud boolForKey:@"preview"] ? NSOnState : NSOffState];
    [_previewCheck setToolTip:@"Show the picture. Off saves the computer the work of decoding it; the sound and recording are not affected."];
    _status = make_label(@"");
    [_status setTextColor:[NSColor disabledControlTextColor]];
    [[_status cell] setLineBreakMode:NSLineBreakByTruncatingMiddle];
    [_status setAutoresizingMask:NSViewWidthSizable];

    NSArray *all = [NSArray arrayWithObjects:_input, _pal, _audio, _wide, _previewCheck, _quality, _folderButton, _record, _time,
                                             _meters, _monitor, _stopAfter, _status, nil];
    unsigned i;
    for (i = 0; i < [all count]; i++)
        [_bar addSubview:[all objectAtIndex:i]];
    [self updateFolderButton];
    [self updateEnabled];
    [_window setFrameAutosaveName:@"HauppaugeRecorderWindow"];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;
    NSUserDefaults *ud = [self ud];
    [ud registerDefaults:[NSDictionary dictionaryWithObjectsAndKeys:
        [NSNumber numberWithInt:0], @"input", [NSNumber numberWithInt:0], @"audio", [NSNumber numberWithBool:NO], @"pal",
        [NSNumber numberWithBool:NO], @"widescreen", [NSNumber numberWithInt:0], @"quality",
        [NSNumber numberWithBool:YES], @"monitor", [NSNumber numberWithBool:YES], @"preview", nil]];

    /* Cocoa must know threads exist before the engine's threads call back into it */
    [NSThread detachNewThreadSelector:@selector(noop:) toTarget:self withObject:nil];

    char folder[1024];
    app_default_folder(folder, sizeof(folder));
    NSString *saved = [ud stringForKey:@"folder"];
    _folder = [(saved ? saved : [NSString stringWithUTF8String:folder]) retain];
    NSArray *args = [[NSProcessInfo processInfo] arguments];
    if ([args containsObject:@"-v"]) hdpvr_set_verbose(1);
    if ([args containsObject:@"-vv"]) hdpvr_set_verbose(2);

    [self buildMenu];
    g_app = app_start((int)[ud integerForKey:@"input"], (int)[ud integerForKey:@"audio"],
                      (int)[ud integerForKey:@"quality"], [ud boolForKey:@"pal"]);
    if (!g_app) {
        NSRunAlertPanel(@APP_NAME " cannot start", @"Out of memory or threads.", @"Quit", nil, nil);
        [NSApp terminate:nil];
        return;
    }
    app_set_widescreen(g_app, [ud boolForKey:@"widescreen"]);
    app_set_preview(g_app, [ud boolForKey:@"preview"]);
    app_set_monitor(g_app, [ud boolForKey:@"monitor"]);
    [self buildWindow];
    [NSApp activateIgnoringOtherApps:YES];

    _timer = [[NSTimer scheduledTimerWithTimeInterval:1.0 / 60 target:self selector:@selector(tick:) userInfo:nil
                                              repeats:YES] retain];
    [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSModalPanelRunLoopMode];
    [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSEventTrackingRunLoopMode];
}

- (void)noop:(id)arg
{
    (void)arg;
}

- (void)windowDidResize:(NSNotification *)note
{
    (void)note;
    [self layoutBar];
}

/* ---- controls ---- */

- (void)sourceChanged:(id)sender
{
    (void)sender;
    NSUserDefaults *ud = [self ud];
    [ud setInteger:[_input indexOfSelectedItem] forKey:@"input"];
    [ud setInteger:[_audio indexOfSelectedItem] forKey:@"audio"];
    [ud setInteger:[_quality indexOfSelectedItem] forKey:@"quality"];
    [ud setBool:[_pal state] == NSOnState forKey:@"pal"];
    app_set_source(g_app, (int)[_input indexOfSelectedItem], (int)[_audio indexOfSelectedItem],
                   (int)[_quality indexOfSelectedItem], [_pal state] == NSOnState);
    [_preview clear];
    [self updateEnabled];
}

- (void)stopAfterChanged:(id)sender
{
    (void)sender;
    app_set_stop_after(g_app, (int)[_stopAfter indexOfSelectedItem]);
    [[self ud] setInteger:[_stopAfter indexOfSelectedItem] forKey:@"stopAfter"];
}

- (void)previewChanged:(id)sender
{
    (void)sender;
    BOOL on = [_previewCheck state] == NSOnState;
    app_set_preview(g_app, on);
    [[self ud] setBool:on forKey:@"preview"];
    if (!on)
        [_preview clear];
}

- (void)widescreenChanged:(id)sender
{
    (void)sender;
    BOOL on = [_wide state] == NSOnState;
    app_set_widescreen(g_app, on);
    [[self ud] setBool:on forKey:@"widescreen"];
}

- (void)monitorChanged:(id)sender
{
    (void)sender;
    BOOL on = [_monitor state] == NSOnState;
    app_set_monitor(g_app, on);
    [[self ud] setBool:on forKey:@"monitor"];
}

- (void)chooseFolder:(id)sender
{
    (void)sender;
    if (app_is_recording(g_app))
        return;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setCanChooseDirectories:YES];
    [p setCanChooseFiles:NO];
    [p setPrompt:@"Choose"];
    if ([p respondsToSelector:@selector(setCanCreateDirectories:)])
        [p setCanCreateDirectories:YES];
    if ([p runModalForDirectory:_folder file:nil types:nil] != NSOKButton || [[p filenames] count] == 0)
        return;
    [_folder release];
    _folder = [[[p filenames] objectAtIndex:0] retain];
    [[self ud] setObject:_folder forKey:@"folder"];
    [self updateFolderButton];
}

- (void)revealFolder:(id)sender
{
    (void)sender;
    [[NSWorkspace sharedWorkspace] openFile:_folder];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if ([item action] == @selector(toggleRecord:)) {
        [item setTitle:app_is_recording(g_app) ? @"Stop Recording" : @"Start Recording"];
        return g_app && !_finishing && (app_is_recording(g_app) || app_state(g_app) == APP_STREAMING);
    }
    if ([item action] == @selector(chooseFolder:))
        return g_app && !app_is_recording(g_app);
    return YES;
}

/* ---- recording ---- */

- (void)toggleRecord:(id)sender
{
    (void)sender;
    if (_finishing)
        return;
    if (app_is_recording(g_app)) {
        _finishing = YES;
        [_record setEnabled:NO];
        [_record setTitle:@"Finishing..."];
        app_record_stop(g_app, rec_done, self);
        [self layoutBar];
        return;
    }
    if (app_state(g_app) != APP_STREAMING)
        return;
    char path[1100], err[300] = "";
    if (app_record_start(g_app, [_folder fileSystemRepresentation], path, sizeof(path), err, sizeof(err))) {
        NSRunAlertPanel(@"Cannot start recording", @"%@", @"OK", nil, nil, [NSString stringWithUTF8String:err]);
        return;
    }
    [_record setTitle:@"Stop"];
    [self updateEnabled];
    [self layoutBar];
}

- (void)showSaved:(NSString *)s
{
    [_saved release];
    _saved = [s retain];
    [_savedUntil release];
    _savedUntil = [[NSDate dateWithTimeIntervalSinceNow:8] retain];
}

- (void)recordingDone:(NSDictionary *)info
{
    _finishing = NO;
    [_record setEnabled:YES];
    [_record setTitle:@"Record"];
    [self updateEnabled];
    [self layoutBar];
    NSString *path = [info objectForKey:@"path"];
    if ([[info objectForKey:@"ok"] boolValue]) {
        [self showSaved:[@"Saved " stringByAppendingString:[path lastPathComponent]]];
        [[NSWorkspace sharedWorkspace] selectFile:path inFileViewerRootedAtPath:@""];
    } else {
        [self showSaved:@"Recording failed: the file could not be written"];
    }
}

static void rec_done(void *user, int ok, const char *path)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSDictionary *info = [NSDictionary dictionaryWithObjectsAndKeys:[NSNumber numberWithBool:ok != 0], @"ok",
                                                                    [NSString stringWithUTF8String:path], @"path", nil];
    [(Controller *)user performSelectorOnMainThread:@selector(recordingDone:) withObject:info waitUntilDone:NO];
    [pool release];
}

- (void)snapshot:(id)sender
{
    (void)sender;
    char path[1100], err[300] = "";
    if (app_snapshot(g_app, [_folder fileSystemRepresentation], path, sizeof(path), err, sizeof(err)))
        [self showSaved:[NSString stringWithUTF8String:err]];
    else
        [self showSaved:[@"Saved " stringByAppendingString:[[NSString stringWithUTF8String:path] lastPathComponent]]];
}

/* ---- timer: the picture every refresh, the rest a few times a second ---- */

- (void)tick:(NSTimer *)t
{
    (void)t;
    if (!g_app)
        return;
    [_preview poll];
    if (++_ticks % 3)
        return;
    double db[2];
    app_peaks(g_app, db);
    int c;
    for (c = 0; c < 2; c++) {
        double v = db[c] + 60;
        _level[c] = v > _level[c] - 1.5 ? v : _level[c] - 1.5;
    }
    [_meters setLeft:(float)(_level[0] / 60) right:(float)(_level[1] / 60)];
    [_preview setAspect:app_aspect(g_app)];

    app_state_t st = app_state(g_app);
    BOOL rec = app_is_recording(g_app);
    [_record setEnabled:!_finishing && (rec || st == APP_STREAMING)];
    if (st != APP_STREAMING)
        [_preview clear];

    char status[600];
    app_status(g_app, status, sizeof(status));
    NSString *s = [NSString stringWithUTF8String:status];
    int w, h, fps;
    NSString *title = app_signal(g_app, &w, &h, &fps)
                          ? [NSString stringWithFormat:@APP_NAME " - %@ %dx%d", [_input titleOfSelectedItem], w, h]
                          : @APP_NAME;
    if (![[_window title] isEqualToString:title])
        [_window setTitle:title];
    if (rec) {
        int sec = (int)app_record_seconds(g_app);
        [_time setStringValue:[NSString stringWithFormat:@"%02d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60]];
        if (!_finishing && app_stop_after_due(g_app))
            [self toggleRecord:nil]; /* "Stop after" */
        s = [NSString stringWithFormat:@"Recording %.1f MB - %@", app_record_bytes(g_app) / 1e6, s];
    } else if (_finishing) {
        s = @"Finishing the file...";
    } else {
        [_time setStringValue:@"00:00:00"];
        if (_saved && [_savedUntil timeIntervalSinceNow] > 0)
            s = _saved;
    }
    [self setStatus:s];
}

/* ---- quitting ---- */

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app
{
    (void)app;
    if (!g_app || !app_is_recording(g_app))
        return NSTerminateNow;
    if (NSRunAlertPanel(@"A recording is in progress", @"Stop the recording and quit?", @"Stop and Quit", @"Cancel",
                        nil) != NSAlertDefaultReturn)
        return NSTerminateCancel;
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_timer invalidate];
    app_close(g_app); /* finishes a recording still in progress */
    g_app = NULL;
}
@end

int main(int argc, const char **argv)
{
    (void)argc, (void)argv;
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    Controller *c = [[Controller alloc] init];
    [NSApp setDelegate:c];
    [NSApp run];
    [pool release];
    return 0;
}
