/*
 * libjackpotlte-fix.dylib: small userland fixes for iOS 16 on the Galaxy A8
 * 2018, inserted into every app by the kernel (boot-arg a8fix=1, kern_exec.c).
 *
 * Typing lag: while a keyboard is up, AssistantServices keeps telling UIKit
 * that dictation availability changed (AFDictationConnection ->
 * -[UIDictationConnection dictationConnnectionDidChangeAvailability:] ->
 * -[UIDictationController postUIKeyboardDictationAvailabilityDidChangeNotification]
 * -> UISystemKeyboardDockController -> +[UIDictationController
 * dictationIsFunctional] -> another availability query), which keeps the main
 * thread busy so taps are handled seconds late. This phone has no working
 * dictation, so: dictation is never functional and availability changes are
 * dropped.
 *
 * Crash reasons: ReportCrash is disabled on this phone (its Mach service
 * would never answer), so an uncaught Objective-C exception is also written
 * to stderr (name, reason, return addresses), which the console's "tee NAME"
 * shows.
 *
 * Brightness (kernel 0199, sysctl kern.a8_brightness 0-255): nothing on this
 * phone takes the level the user sets to the panel (CoreBrightness has no
 * backlight it knows). The level is caught where it is set -- Control
 * Center's slider (-[CCUIDisplayModuleViewController _setBacklightLevel:]),
 * SpringBoard's -[SBBrightnessController setBrightnessLevel:] and
 * _setBrightnessLevel:showHUD:, and CoreBrightness's -[BrightnessSystemClient
 * setProperty:forKey:] for DisplayBrightness (Settings) -- and written to the
 * sysctl, which moves the panel's AOR. Control Center's slider was not even
 * interactive (-[CCUIDisplayModuleViewController _canAdjustBrightness] said
 * NO, from BKSDisplayBrightnessIsBrightnessLevelControlAvailable): it says
 * YES. Control Center asks _backlightLevel
 * for where to draw the slider, and gets the level last set. The approach is
 * Scion's (cooblg/scion-kernel userland/src/inject_sb.c, brightness_bridge).
 *
 * os_log to stderr (v5): a process named with the console's "tee" starts
 * with OS_ACTIVITY_DT_MODE in its environment (kernel 0192), which iOS's
 * libtrace does not honor on this build. Such a process gets an os_log hook
 * (libsystem_trace's os_log_set_hook, the one Xcode's console uses) that
 * writes each of its log messages to stderr as one line, "A8LOG <type>
 * <message>", which the tee shows. This phone has no "log stream" and
 * ReportCrash is off, so this is how a daemon that fails quietly (assetsd not
 * making the Photos library) says why.
 *
 * Layout (v7): this screen is 360 pt wide, a width no iPhone has. In app
 * processes MobileGestalt has no ArtworkDeviceSubType (the /product node is
 * only read in SpringBoard), and UIKit sizes the keyboard for a 320 pt phone
 * (UIKeyboardAutomatic 320 wide at x 20) while its keys are laid out for 360:
 * the keyboard sat 20 pt right with its right column cut off. The keyboard's
 * default size gets the screen's width. In SpringBoard (subtype 2556, the
 * 393 pt iPhone 14 Pro the Dynamic Island needs) Control Center's module grid
 * (320 wide) was placed at x 0 instead of centered: CCUIScrollView centers it.
 *
 * Auto-Lock (v8): with no CommCenter (no cellular here) SpringBoard's
 * -[SBTelephonyManager _handleTelephonyDaemonRestart] retries once a second,
 * and each retry ends in -[SBIdleTimerGlobalCoordinator resetIdleTimer]
 * ("updateIdleTimerForReason: IdleTimerReset" ~once a second), so the
 * Auto-Lock timer never ran out. A reset made inside that retry is dropped;
 * the retries go on as before. The approach is Scion's (inject_sb.c,
 * step 105).
 *
 * Status bar (v10): DynamicSplit lays the bar out for the 393 pt iPhone 14
 * Pro; on this 360 pt screen the clock sat 22 pt from the left edge and the
 * battery 49 pt from the right one, too far toward the island.  After each
 * layout of _UIStatusBarForegroundView the items left of its middle move so
 * the leftmost starts a margin from the left edge, and the ones right of it
 * so the rightmost ends the same margin from the right edge.  The margin is
 * 5 pt (picked on the phone 11-24: 4 sat too close to the rounded corners, 6
 * too far in), or the number in /private/var/mobile/Library/a8fix-sbmargin
 * (points, 2 to 60, read once when the process starts).
 *
 * Status bar (v14, user report 11-25): a real Dynamic Island iPhone centres
 * the clock in the left "ear" beside the island and the battery group in the
 * right one, on the island's middle line; v10 pushed them to the screen's
 * edges and they sat high (the clock's middle 13 pt down, the island's 30).
 * Now the items left of the island are centred between the left edge and the
 * island, the ones right of it between the island and the right edge (at
 * least 6 pt from the edges, 4 pt from the island), each on the island's
 * middle line.  The island is the status bar's avoidance frame when it has a
 * plausible one (it follows a compact Live Activity), else the idle pill
 * (117.5-242.5 pt, 11.3-48 pt).  a8fix-sbmargin holding a number keeps v10's
 * edge layout; a8fix-sbvcenter "0" keeps the items' heights.
 *
 * Status bar (v16, user reports 11-26): the battery jumped to the top edge and
 * then into place whenever the bar came back (closing Control Center,
 * unlocking, leaving an app).  v14 left items that were still fading in
 * (alpha 0) where UIKit had put them, high up, until the next layout.  Now
 * every item that is not hidden is moved; the group is measured from the
 * visible items, or from the fading ones when none is visible.  The items also
 * sit a little toward the screen's edges: 5 pt out from the middle of each
 * ear, or the number in a8fix-sbout (0 to 30 pt; "0" = v14's centred items).
 * a8fix-sblog "1" logs each layout pass that moves or skips something, and
 * the bar's visual provider's methods once.
 *
 * Backdrop scale (v17b, P3a): every CABackdropLayer captures at most 25%
 * (they all captured at 100% here); a8fix-bdscale N or "1"; see bd_apply.
 *
 * CoreImage (v17d, Bold Text crash): no CIContext can work here (no Metal,
 * no CPU renderer in iOS 16, and its GL path needs Apple's private GLES
 * context), so its GL context makers answer nil instead of crashing at
 * pc 0; a8fix-cigl "1" keeps Apple's; see ci_apply.
 *
 * Dim lock screen (v18e, P14 AOD): the cover sheet's darkening effect view is
 * made clear when SpringBoard blanks the screen, so the frame the kernel keeps
 * has the wallpaper (a8wall-aodfx = percent kept, "100" = off); with the GL
 * shim's live dim (a8gl-aodlive "1") the lock screen clock keeps running
 * (v18g); see aod_wall_apply.
 *
 * Sensor curtain (v14, user report 11-25): the expanded island's X button had
 * a pill-shaped bite: the sensor cover (_SBSystemApertureMagiciansCurtainView
 * in the aperture window that takes no touches) is drawn above the content,
 * and on a 360 pt screen the button reaches under it.  That curtain is hidden
 * in SpringBoard (no camera there on this phone); a8fix-curtain "1" keeps it.
 *
 * View dump (v6): SIGUSR1 (console "kill NAME 30") makes the process write
 * every visible window's views to stderr, one line each: depth, class, frame
 * in window points, and a label's text ("A8VIEW ..."), seen with tee. It is
 * how an off-center layout (keyboard, Control Center) is found.  v15 adds a
 * backdrop layer's class, group name and capture scale.
 *
 * Wallpaper extensions (v11): kernel 0216 lets lsd read the extension point
 * table, but LaunchServices' database was made while every extension was
 * refused, so no poster provider is in it and the wallpaper stays black.
 * Moving the database aside for a full rebuild hung lsd on this image (11-25:
 * the first-boot data migration never ran here).  As the Galaxy S7 port does
 * (wardevsa/ios-a5xelte, hero-run.c tell_ls_of_posters), SpringBoard
 * registers just the poster bundles with LSRegisterURL, once: when
 * /private/var/mobile/Library/a8fix-posters holds "1", 30 s after SpringBoard
 * starts, off the main thread; the results replace the file's content.
 *
 * Plain C on the Objective-C runtime; no SDK headers.
 */
typedef struct objc_class *Class;
typedef struct objc_selector *SEL;
typedef struct objc_object *id;
typedef struct objc_method *Method;
typedef void (*IMP)(void);

extern Class objc_getClass(const char *name);
extern SEL sel_registerName(const char *name);
extern Method class_getInstanceMethod(Class cls, SEL sel);
extern Method class_getClassMethod(Class cls, SEL sel);
extern IMP method_setImplementation(Method m, IMP imp);
extern void _dyld_register_func_for_add_image(void (*func)(const void *mh, long slide));
extern id objc_msgSend(id self, SEL op, ...);
extern long write(int fd, const void *buf, unsigned long n);
extern void NSSetUncaughtExceptionHandler(void (*handler)(id exception));
extern int sysctlbyname(const char *name, void *oldp, unsigned long *oldlenp, void *newp, unsigned long newlen);
extern const char *getprogname(void);
extern const char *method_getTypeEncoding(Method m);
extern void *MGCopyAnswer(void *key);
typedef unsigned long long dispatch_time_t;
extern dispatch_time_t dispatch_time(dispatch_time_t when, long long delta);
extern void dispatch_after_f(dispatch_time_t when, void *queue, void *context, void (*work)(void *));
extern char _dispatch_main_q;
typedef struct { double x, y, w, h; } fix_rect;
extern char *getenv(const char *name);
extern const char *class_getName(Class cls);
extern Class class_getSuperclass(Class cls);
extern Method *class_copyMethodList(Class cls, unsigned int *count);
extern SEL method_getName(Method m);
extern const char *sel_getName(SEL sel);
extern IMP class_getMethodImplementation(Class cls, SEL sel);
extern signed char class_addMethod(Class cls, SEL sel, IMP imp, const char *types);
extern Class object_getClass(id obj);
extern void *dispatch_source_create(const void *type, unsigned long handle, unsigned long mask, void *queue);
extern void dispatch_source_set_event_handler_f(void *source, void (*handler)(void *));
extern void dispatch_resume(void *object);
extern const char _dispatch_source_type_signal;
extern void (*signal(int sig, void (*func)(int)))(int);
extern void free(void *p);
typedef struct os_log_message_s *os_log_message_t;
typedef void (^os_log_hook_t)(unsigned char type, os_log_message_t message);
extern os_log_hook_t os_log_set_hook(unsigned char level, os_log_hook_t hook);
extern char *os_log_copy_message_string(os_log_message_t message);
/* v9: the symbol test (CoreGraphics and CoreFoundation, from their .tbd) */
extern unsigned long CGImageGetWidth(void *image);
extern unsigned long CGImageGetHeight(void *image);
extern unsigned long CGImageGetBytesPerRow(void *image);
extern unsigned long CGImageGetBitsPerPixel(void *image);
extern unsigned int CGImageGetBitmapInfo(void *image);
extern void *CGImageGetDataProvider(void *image);
extern void *CGDataProviderCopyData(void *provider);
extern const unsigned char *CFDataGetBytePtr(void *data);
extern long CFDataGetLength(void *data);
extern void CFRelease(void *object);

static void
fix_put(const char *s)
{
	unsigned long n = 0;

	if (s == 0)
		s = "(null)";
	while (s[n] != 0)
		n++;
	write(2, s, n);
}

static const char *
fix_utf8(id obj)
{
	const char *(*str)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
	id (*desc)(id, SEL) = (id (*)(id, SEL))objc_msgSend;

	if (obj == 0)
		return "(nil)";
	return str(desc(obj, sel_registerName("description")), sel_registerName("UTF8String"));
}

static void
fix_exception(id e)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	unsigned long (*count)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
	unsigned long long (*ull)(id, SEL) = (unsigned long long (*)(id, SEL))objc_msgSend;
	id addrs;
	unsigned long n;
	char hex[20];

	fix_put("A8FIX exception ");
	fix_put(fix_utf8(get(e, sel_registerName("name"))));
	fix_put(": ");
	fix_put(fix_utf8(get(e, sel_registerName("reason"))));
	fix_put("\nA8FIX bt");
	addrs = get(e, sel_registerName("callStackReturnAddresses"));
	n = addrs ? count(addrs, sel_registerName("count")) : 0;
	for (unsigned long i = 0; i < n && i < 40; i++) {
		unsigned long long a = ull(at(addrs, sel_registerName("objectAtIndex:"), i),
		    sel_registerName("unsignedLongLongValue"));
		hex[0] = ' ';
		hex[1] = '0';
		hex[2] = 'x';
		for (int k = 0; k < 16; k++) {
			int d = (int)((a >> (60 - 4 * k)) & 15);
			hex[3 + k] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		}
		hex[19] = 0;
		fix_put(hex);
	}
	fix_put("\n");
}

static signed char
fix_no(id self, SEL cmd)
{
	(void)self;
	(void)cmd;
	return 0;
}

static void
fix_nop(id self, SEL cmd)
{
	(void)self;
	(void)cmd;
}

static void
fix_nop1(id self, SEL cmd, id arg)
{
	(void)self;
	(void)cmd;
	(void)arg;
}

static int fix_done;

static void
fix_apply(void)
{
	Class dc, conn;
	Method m;

	if (fix_done)
		return;
	dc = objc_getClass("UIDictationController");
	if (dc == 0)
		return;
	fix_done = 1;
	m = class_getClassMethod(dc, sel_registerName("dictationIsFunctional"));
	if (m)
		method_setImplementation(m, (IMP)fix_no);
	m = class_getInstanceMethod(dc,
	    sel_registerName("postUIKeyboardDictationAvailabilityDidChangeNotification"));
	if (m)
		method_setImplementation(m, (IMP)fix_nop);
	conn = objc_getClass("UIDictationConnection");
	if (conn) {
		m = class_getInstanceMethod(conn,
		    sel_registerName("dictationConnnectionDidChangeAvailability:"));
		if (m)
			method_setImplementation(m, (IMP)fix_nop1);
	}
}

/* ---- brightness ------------------------------------------------------ */

static float bl_user = -1.0f;           /* the level last set, 0..1 */
static int bl_written = -1;

static void
bl_set(float value)
{
	int level;

	if (!(value >= 0.0f))
		value = 0.0f;
	if (value > 1.0f)
		value = 1.0f;
	bl_user = value;
	level = (int)(value * 255.0f + 0.5f);
	if (level != bl_written && sysctlbyname("kern.a8_brightness", 0, 0, &level, sizeof(level)) == 0)
		bl_written = level;
}

/* a method whose type encoding has a float argument where `want` is, or 0 */
static Method
bl_method(const char *class_name, const char *selector, int class_method, const char *want)
{
	Class c = objc_getClass(class_name);
	Method m;
	const char *types;

	if (c == 0)
		return 0;
	m = class_method ? class_getClassMethod(c, sel_registerName(selector)) :
	    class_getInstanceMethod(c, sel_registerName(selector));
	if (m == 0)
		return 0;
	types = method_getTypeEncoding(m);
	if (types == 0 || types[0] != want[0])
		return 0;
	/* the arguments after self and _cmd: each of want[1..] must be in order */
	for (const char *w = want + 1, *t = types + 1; *w; w++) {
		while (*t && *t != *w)
			t++;
		if (*t == 0)
			return 0;
		t++;
	}
	return m;
}

static void (*cc_set_orig)(id, SEL, float);
static float (*cc_level_orig)(id, SEL);
static void (*sb_set_orig)(id, SEL, float);
static void (*sb_set_hud_orig)(id, SEL, float, signed char);
static signed char (*bsc_set_orig)(id, SEL, id, id);

static void
cc_set(id self, SEL cmd, float level)
{
	bl_set(level);
	cc_set_orig(self, cmd, level);
}

static float
cc_level(id self, SEL cmd)
{
	return bl_user >= 0.0f ? bl_user : cc_level_orig(self, cmd);
}

static void
sb_set(id self, SEL cmd, float level)
{
	bl_set(level);
	sb_set_orig(self, cmd, level);
}

static void
sb_set_hud(id self, SEL cmd, float level, signed char hud)
{
	bl_set(level);
	sb_set_hud_orig(self, cmd, level, hud);
}

static signed char
bsc_set(id self, SEL cmd, id property, id key)
{
	const char *(*str)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
	signed char (*is_kind)(id, SEL, Class) = (signed char (*)(id, SEL, Class))objc_msgSend;
	float (*fv)(id, SEL) = (float (*)(id, SEL))objc_msgSend;
	id (*obj_for)(id, SEL, id) = (id (*)(id, SEL, id))objc_msgSend;
	id (*make)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;
	Class string = objc_getClass("NSString"), number = objc_getClass("NSNumber"),
	    dictionary = objc_getClass("NSDictionary");

	if (key != 0 && string != 0 && is_kind(key, sel_registerName("isKindOfClass:"), string)) {
		const char *k = str(key, sel_registerName("UTF8String"));
		const char *want = "DisplayBrightness";
		int same = k != 0;

		for (int i = 0; same && (k[i] != 0 || want[i] != 0); i++)
			same = k[i] == want[i];
		if (same && property != 0) {
			id n = property;

			if (dictionary != 0 && is_kind(n, sel_registerName("isKindOfClass:"), dictionary))
				n = obj_for(n, sel_registerName("objectForKey:"),
				    make(string, sel_registerName("stringWithUTF8String:"), "Brightness"));
			if (n != 0 && number != 0 && is_kind(n, sel_registerName("isKindOfClass:"), number))
				bl_set(fv(n, sel_registerName("floatValue")));
		}
	}
	return bsc_set_orig(self, cmd, property, key);
}

static signed char
cc_can_adjust(id self, SEL cmd)
{
	(void)self;
	(void)cmd;
	return 1;
}

static int bl_cc_done, bl_sb_done, bl_bsc_done;

static void
bl_apply(void)
{
	Method m;

	if (!bl_cc_done && objc_getClass("CCUIDisplayModuleViewController") != 0) {
		bl_cc_done = 1;
		m = bl_method("CCUIDisplayModuleViewController", "_setBacklightLevel:", 0, "vf");
		if (m)
			cc_set_orig = (void (*)(id, SEL, float))method_setImplementation(m, (IMP)cc_set);
		m = bl_method("CCUIDisplayModuleViewController", "_backlightLevel", 0, "f");
		if (m)
			cc_level_orig = (float (*)(id, SEL))method_setImplementation(m, (IMP)cc_level);
		/* the slider is made non-interactive when backboardd says the
		 * brightness level cannot be controlled (no backlight it knows) */
		m = bl_method("CCUIDisplayModuleViewController", "_canAdjustBrightness", 0, "B");
		if (m == 0)
			m = bl_method("CCUIDisplayModuleViewController", "_canAdjustBrightness", 0, "c");
		if (m)
			method_setImplementation(m, (IMP)cc_can_adjust);
		fix_put("A8FIX brightness: Control Center's slider hooked\n");
	}
	if (!bl_sb_done && objc_getClass("SBBrightnessController") != 0) {
		bl_sb_done = 1;
		m = bl_method("SBBrightnessController", "setBrightnessLevel:", 0, "vf");
		if (m)
			sb_set_orig = (void (*)(id, SEL, float))method_setImplementation(m, (IMP)sb_set);
		m = bl_method("SBBrightnessController", "_setBrightnessLevel:showHUD:", 0, "vfB");
		if (m)
			sb_set_hud_orig = (void (*)(id, SEL, float, signed char))method_setImplementation(m, (IMP)sb_set_hud);
		fix_put("A8FIX brightness: SpringBoard's controller hooked\n");
	}
	if (!bl_bsc_done && objc_getClass("BrightnessSystemClient") != 0) {
		bl_bsc_done = 1;
		m = bl_method("BrightnessSystemClient", "setProperty:forKey:", 0, "c@@");
		if (m == 0)
			m = bl_method("BrightnessSystemClient", "setProperty:forKey:", 0, "B@@");
		if (m)
			bsc_set_orig = (signed char (*)(id, SEL, id, id))method_setImplementation(m, (IMP)bsc_set);
	}
}

/* ---- layout on a 360 pt screen (v7) ----------------------------------- */

struct vline {
	char b[400];
	unsigned long n;
};

static void
vl_str(struct vline *l, const char *s)
{
	for (; s && *s && l->n < sizeof(l->b) - 2; s++)
		l->b[l->n++] = (*s == '\n') ? ' ' : *s;
}

static void
vl_num(struct vline *l, double v)
{
	char t[24];
	int k = 0;
	long long whole;
	int tenth;

	if (v < 0) {
		vl_str(l, "-");
		v = -v;
	}
	whole = (long long)v;
	tenth = (int)((v - (double)whole) * 10.0 + 0.5);
	if (tenth >= 10) {
		whole++;
		tenth = 0;
	}
	do {
		t[k++] = (char)('0' + whole % 10);
		whole /= 10;
	} while (whole > 0 && k < 20);
	while (k > 0 && l->n < sizeof(l->b) - 2)
		l->b[l->n++] = t[--k];
	if (tenth != 0 && l->n < sizeof(l->b) - 3) {
		l->b[l->n++] = '.';
		l->b[l->n++] = (char)('0' + tenth);
	}
}


typedef struct { double w, h; } fix_size;

static double
screen_width(void)
{
	Class ui = objc_getClass("UIScreen");
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	id screen;

	if (ui == 0 || (screen = get((id)ui, sel_registerName("mainScreen"))) == 0)
		return 0;
	return rect(screen, sel_registerName("bounds")).w;
}

static fix_size (*kb_size_orig)(id, SEL, long);
static fix_size (*kbi_size_orig)(id, SEL, long);
static int kb_said;

static fix_size
kb_widen(fix_size size, long orientation, const char *who)
{
	double w = screen_width();

	/* portrait only (1, 2): a keyboard narrower than the screen but more
	 * than 80% of it is the 320 pt fallback; it gets the screen's width */
	if ((orientation == 1 || orientation == 2) && w > 0 && size.w < w && size.w > w * 0.8) {
		if (kb_said < 2) {
			char line[96] = "A8FIX keyboard: ";
			unsigned long n = 16;

			for (const char *t = who; *t && n < 60; t++)
				line[n++] = *t;
			line[n++] = '\n';
			write(2, line, n);
			kb_said++;
		}
		size.w = w;
	}
	return size;
}

static fix_size
kb_size(id self, SEL cmd, long orientation)
{
	return kb_widen(kb_size_orig(self, cmd, orientation), orientation, "UIKeyboard default width set to the screen's");
}

static fix_size
kbi_size(id self, SEL cmd, long orientation)
{
	return kb_widen(kbi_size_orig(self, cmd, orientation), orientation, "UIKeyboardImpl default width set to the screen's");
}

/* Control Center: the view that holds the module grid, centered */
static void (*cc_scroll_layout_orig)(id, SEL);

static void
cc_scroll_layout(id self, SEL cmd)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
	unsigned long (*count)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	void (*set_rect)(id, SEL, fix_rect) = (void (*)(id, SEL, fix_rect))objc_msgSend;
	Class grid = objc_getClass("CCUIModuleCollectionView");
	fix_rect bounds, f;
	id views;
	unsigned long n;

	cc_scroll_layout_orig(self, cmd);
	if (grid == 0)
		return;
	bounds = rect(self, sel_registerName("bounds"));
	views = get(self, sel_registerName("subviews"));
	n = views ? count(views, sel_registerName("count")) : 0;
	for (unsigned long i = 0; i < n; i++) {
		id v = at(views, sel_registerName("objectAtIndex:"), i);
		id inner = get(v, sel_registerName("subviews"));
		unsigned long m = inner ? count(inner, sel_registerName("count")) : 0;
		int holds = 0;

		for (unsigned long j = 0; j < m && !holds; j++)
			holds = object_getClass(at(inner, sel_registerName("objectAtIndex:"), j)) == grid;
		if (!holds)
			continue;
		f = rect(v, sel_registerName("frame"));
		if (f.w > 0 && f.w < bounds.w && f.x < 0.5 && f.x > -0.5) {
			f.x = (bounds.w - f.w) / 2;
			set_rect(v, sel_registerName("setFrame:"), f);
		}
	}
}

/* Control Center on notch and island phones: the frame its status bar
 * presentation gives the module grid's container, centered */
static fix_rect (*cc_presented_orig)(id, SEL);
static int cc_presented_said;

static fix_rect
cc_presented(id self, SEL cmd)
{
	fix_rect f = cc_presented_orig(self, cmd);
	double w = screen_width();

	if (cc_presented_said < 1) {
		struct vline l;

		l.n = 0;
		vl_str(&l, "A8FIX control center: presented frame ");
		vl_num(&l, f.x);
		vl_str(&l, ",");
		vl_num(&l, f.y);
		vl_str(&l, " ");
		vl_num(&l, f.w);
		vl_str(&l, "x");
		vl_num(&l, f.h);
		vl_str(&l, " on a screen ");
		vl_num(&l, w);
		vl_str(&l, " wide\n");
		write(2, l.b, l.n);
		cc_presented_said++;
	}
	if (w > 0 && f.w > 0 && f.w < w && f.x > -0.5 && f.x < 0.5)
		f.x = (w - f.w) / 2;
	return f;
}

/* the overlay controller: after its layout, the grid's container centered */
static void (*cc_vwls_orig)(id, SEL);
static int cc_center_said;

static void
cc_center(id vc)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	void (*set_rect)(id, SEL, fix_rect) = (void (*)(id, SEL, fix_rect))objc_msgSend;
	id grid, container, host;
	fix_rect f, b;

	grid = get(vc, sel_registerName("overlayModuleCollectionView"));
	container = grid ? get(grid, sel_registerName("superview")) : 0;
	host = container ? get(container, sel_registerName("superview")) : 0;
	if (host == 0)
		return;
	f = rect(container, sel_registerName("frame"));
	b = rect(host, sel_registerName("bounds"));
	if (cc_center_said < 1) {
		struct vline l;

		l.n = 0;
		vl_str(&l, "A8FIX control center: grid container at ");
		vl_num(&l, f.x);
		vl_str(&l, " width ");
		vl_num(&l, f.w);
		vl_str(&l, " in ");
		vl_num(&l, b.w);
		vl_str(&l, "\n");
		write(2, l.b, l.n);
		cc_center_said++;
	}
	if (f.w > 0 && f.w < b.w && f.x > -0.5 && f.x < 0.5) {
		f.x = (b.w - f.w) / 2;
		set_rect(container, sel_registerName("setFrame:"), f);
	}
}

static void
cc_vwls(id self, SEL cmd)
{
	cc_vwls_orig(self, cmd);
	cc_center(self);
}

/* `cls`'s own method for `sel` replaced (added if it only inherits one);
 * the implementation it had (or its superclass's) returned */
static IMP
own_method(Class cls, const char *selector, IMP imp, const char *types)
{
	SEL sel = sel_registerName(selector);
	IMP old = class_getMethodImplementation(cls, sel);
	Method m = class_getInstanceMethod(cls, sel);

	if (m == 0)
		return 0;
	if (class_addMethod(cls, sel, imp, types))
		return old;                     /* (inherited: the superclass's) */
	return method_setImplementation(m, imp);
}

/* ---- status bar (v10, v14) ------------------------------------------ */

#define SB_MARGIN_SWITCH  "/private/var/mobile/Library/a8fix-sbmargin"
#define SB_VCENTER_SWITCH "/private/var/mobile/Library/a8fix-sbvcenter"
#define SB_OUT_SWITCH     "/private/var/mobile/Library/a8fix-sbout"
#define SB_LOG_SWITCH     "/private/var/mobile/Library/a8fix-sblog"
#define SB_LOG_MAX        400

static void (*sb_fg_layout_orig)(id, SEL);
static double sb_margin = -1;           /* v10's edge margin; 0: v14's ears */
static int sb_vcenter = 1;              /* v14: on the island's middle line */
static double sb_out = 5;               /* v16: points out from each ear's middle */
static int sb_log;                      /* v16: a8fix-sblog "1" */
static int sb_logged;
static int sb_said;

/* the text of a switch file (an NSString), or 0 when there is none */
static id
fix_switch_text(const char *path)
{
	Class string = objc_getClass("NSString");
	id (*make)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;
	id (*from_file)(Class, SEL, id, unsigned long, id *) =
	    (id (*)(Class, SEL, id, unsigned long, id *))objc_msgSend;

	if (string == 0)
		return 0;
	return from_file(string, sel_registerName("stringWithContentsOfFile:encoding:error:"),
	    make(string, sel_registerName("stringWithUTF8String:"), path), 4 /* UTF-8 */, 0);
}

static double
sb_margin_now(void)
{
	if (sb_margin < 0) {
		double (*dbl)(id, SEL) = (double (*)(id, SEL))objc_msgSend;
		id text = fix_switch_text(SB_MARGIN_SWITCH);
		double v = text != 0 ? dbl(text, sel_registerName("doubleValue")) : 0;

		sb_margin = (v >= 2 && v <= 60) ? v : 0;
		text = fix_switch_text(SB_VCENTER_SWITCH);
		sb_vcenter = !(text != 0 &&
		    ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String"))[0] == '0');
		text = fix_switch_text(SB_OUT_SWITCH);
		if (text != 0) {
			v = dbl(text, sel_registerName("doubleValue"));
			sb_out = (v >= 0 && v <= 30) ? v : sb_out;
		}
		text = fix_switch_text(SB_LOG_SWITCH);
		sb_log = text != 0 &&
		    ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String"))[0] == '1';
	}
	return sb_margin;
}

/* to the panel's pixel grid (3 pixels a point) */
static double
sb_px(double v)
{
	double t = v * 3.0;

	return (double)(long long)(t + (t >= 0 ? 0.5 : -0.5)) / 3.0;
}

/* v10: the leftmost item `m` from the left edge, the rightmost `m` from the right */
static void
sb_edges(id self, fix_rect b, double m)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
	unsigned long (*count)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	signed char (*flag)(id, SEL) = (signed char (*)(id, SEL))objc_msgSend;
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	void (*set_rect)(id, SEL, fix_rect) = (void (*)(id, SEL, fix_rect))objc_msgSend;
	double lmin = 1e9, rmax = -1e9, dl = 0, dr = 0;
	unsigned long n, nl = 0, nr = 0;
	id views;

	views = get(self, sel_registerName("subviews"));
	n = views ? count(views, sel_registerName("count")) : 0;
	for (unsigned long i = 0; i < n; i++) {
		id v = at(views, sel_registerName("objectAtIndex:"), i);
		fix_rect f;

		if (flag(v, sel_registerName("isHidden")))
			continue;
		f = rect(v, sel_registerName("frame"));
		if (f.w < 0.5 || f.w > b.w / 3)
			continue;       /* nothing bar-wide */
		if (f.x + f.w / 2 < b.w / 2) {
			lmin = f.x < lmin ? f.x : lmin;
			nl++;
		} else {
			rmax = f.x + f.w > rmax ? f.x + f.w : rmax;
			nr++;
		}
	}
	if (nl)
		dl = m - lmin;
	if (nr)
		dr = (b.w - m) - rmax;
	if (dl > -0.25 && dl < 0.25 && dr > -0.25 && dr < 0.25)
		return;
	for (unsigned long i = 0; i < n; i++) {
		id v = at(views, sel_registerName("objectAtIndex:"), i);
		fix_rect f;

		if (flag(v, sel_registerName("isHidden")))
			continue;
		f = rect(v, sel_registerName("frame"));
		if (f.w < 0.5 || f.w > b.w / 3)
			continue;
		f.x += (f.x + f.w / 2 < b.w / 2) ? dl : dr;
		set_rect(v, sel_registerName("setFrame:"), f);
	}
	if (sb_said < 2) {
		struct vline l;

		l.n = 0;
		vl_str(&l, "A8FIX status bar: left items moved ");
		vl_num(&l, dl);
		vl_str(&l, " pt, right items ");
		vl_num(&l, dr);
		vl_str(&l, " pt (margin ");
		vl_num(&l, m);
		vl_str(&l, ", bar ");
		vl_num(&l, b.w);
		vl_str(&l, " wide)\n");
		write(2, l.b, l.n);
		sb_said++;
	}
}

/*
 * v14: where the island is, in the bar's points: the status bar's own
 * avoidance frame (-[_UIStatusBar avoidanceFrame], which SpringBoard keeps on
 * the island and on a compact Live Activity) when it is a plausible one, else
 * the idle pill (the sensor curtain: 125 pt wide, centred, 11.3 to 48 pt).
 * v16b: OX is where the bar starts on the screen (Control Center's compact
 * bar starts at 55 pt); the pill is centred on the screen, not on the bar.
 */
static fix_rect
sb_island(id fg, fix_rect b, double ox, double sw, const char **from)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	signed char (*responds)(id, SEL, SEL) = (signed char (*)(id, SEL, SEL))objc_msgSend;
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	fix_rect (*convert)(id, SEL, fix_rect, id) = (fix_rect (*)(id, SEL, fix_rect, id))objc_msgSend;
	SEL avoid = sel_registerName("avoidanceFrame");
	fix_rect f;
	id v = fg;

	for (int up = 0; up < 4 && v != 0; up++, v = get(v, sel_registerName("superview"))) {
		if (!responds(v, sel_registerName("respondsToSelector:"), avoid))
			continue;
		f = convert(fg, sel_registerName("convertRect:fromView:"), rect(v, avoid), v);
		if (f.w >= 40 && f.h >= 10 && f.h <= 60 && f.x + ox >= 12 && f.x + ox + f.w <= sw - 12 &&
		    f.y >= -2 && f.y < b.h) {
			*from = class_getName(object_getClass(v));
			return f;
		}
		*from = "default (avoidance frame empty)";
		goto pill;
	}
	*from = "default (no avoidance frame)";
pill:
	f.w = 125;
	f.x = (sw - f.w) / 2 - ox;
	f.y = 11.3;
	f.h = 36.7;
	return f;
}

/* v16 (a8fix-sblog "1"): the methods of the bar's visual provider, once, so a
 * later fix can change UIKit's own layout instead of moving its results */
static void
sb_log_provider(id fg)
{
	static int done;
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	signed char (*responds)(id, SEL, SEL) = (signed char (*)(id, SEL, SEL))objc_msgSend;
	SEL vp = sel_registerName("visualProvider");
	id v = fg, provider = 0;

	if (done || !sb_log)
		return;
	done = 1;
	for (int up = 0; up < 4 && v != 0 && provider == 0; up++, v = get(v, sel_registerName("superview")))
		if (responds(v, sel_registerName("respondsToSelector:"), vp))
			provider = get(v, vp);
	if (provider == 0) {
		fix_put("A8FIX sb: no visual provider found\n");
		return;
	}
	for (Class c = object_getClass(provider); c != 0 && sb_logged < SB_LOG_MAX; c = class_getSuperclass(c)) {
		const char *cn = class_getName(c);

		if (cn[0] == 'N' && cn[1] == 'S' && cn[2] == 'O')
			break;          /* NSObject */
		for (int meta = 0; meta < 2; meta++) {
			unsigned int count = 0;
			Method *list = class_copyMethodList(meta ? object_getClass((id)c) : c, &count);
			struct vline l;

			l.n = 0;
			vl_str(&l, meta ? "A8FIX sb provider + " : "A8FIX sb provider - ");
			vl_str(&l, cn);
			vl_str(&l, ":");
			for (unsigned int i = 0; i < count; i++) {
				const char *s = sel_getName(method_getName(list[i]));
				const char *t = method_getTypeEncoding(list[i]);
				unsigned long len = 0;

				while (s[len])
					len++;
				if (l.n + len + 24 > sizeof(l.b) - 2) {
					vl_str(&l, "\n");
					l.b[l.n - 1] = '\n';
					write(2, l.b, l.n);
					sb_logged++;
					l.n = 0;
					vl_str(&l, "A8FIX sb provider ... ");
				}
				vl_str(&l, " ");
				vl_str(&l, s);
				if (t && t[0] != 'v' && t[0] != '@') {
					/* the return type when it is a number or a struct */
					char r[2] = { t[0], 0 };

					vl_str(&l, "=");
					vl_str(&l, t[0] == '{' ? "struct" : r);
				}
			}
			l.b[l.n++] = '\n';
			write(2, l.b, l.n);
			sb_logged++;
			if (list)
				free(list);
		}
	}
}

/*
 * v14: as a real Dynamic Island iPhone lays it out: the items left of the
 * island centred in the left "ear" (between the screen's edge and the
 * island), the ones right of it in the right ear, at least 6 pt from the
 * screen's edges and 4 pt from the island, each on the island's middle line
 * (a8fix-sbvcenter "0" leaves the heights alone).  Only the bar that spans
 * the screen at its top (Control Center's own bar is inset and keeps its
 * layout).  v16: items fading in (alpha 0) move too, and each group sits
 * sb_out points out from its ear's middle (a8fix-sbout).  v16b: v14 asked for
 * the bar at the screen's top left; the bars that carry the items during a
 * transition are elsewhere (an app's card moving and shrinking, the cover
 * sheet above the screen, Control Center's compact bar at 55 pt) and kept
 * UIKit's high layout, so the battery jumped when the real bar took over.
 * Now: (A) every bar as wide as the screen, in its own points, wherever it is;
 * (B) a bar at the top flush with a screen edge, in screen points.
 */
static void
sb_ears(id self, fix_rect b)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
	unsigned long (*count)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	signed char (*flag)(id, SEL) = (signed char (*)(id, SEL))objc_msgSend;
	double (*dbl)(id, SEL) = (double (*)(id, SEL))objc_msgSend;
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	fix_rect (*convert)(id, SEL, fix_rect, id) = (fix_rect (*)(id, SEL, fix_rect, id))objc_msgSend;
	void (*set_rect)(id, SEL, fix_rect) = (void (*)(id, SEL, fix_rect))objc_msgSend;
	static double said_il = -1, said_ir = -1;
	static double main_il, main_ir, main_iy;     /* v16b: the main bar's island */
	static int main_set;
	/* [0] the visible items, [1] the ones fading in (alpha 0); per side */
	double lmin[2] = { 1e9, 1e9 }, lmax[2] = { -1e9, -1e9 }, rmin[2] = { 1e9, 1e9 }, rmax[2] = { -1e9, -1e9 };
	unsigned long nl[2] = { 0, 0 }, nr[2] = { 0, 0 };
	double l0 = 0, l1 = 0, r0 = 0, r1 = 0, dl = 0, dr = 0;
	double il, ir, ic, iy, ox, sw = screen_width();
	unsigned long n, moved = 0;
	int lv, rv;
	const char *from = "";
	fix_rect w, island;
	id views;
	struct vline dl_line;

	w = convert(self, sel_registerName("convertRect:toView:"), b, 0);
	if (sw < 250)
		sw = b.w;
	if (b.w > sw - 1 && b.w < sw + 1) {
		/* (A) a screen-wide bar, wherever it is drawn and however it is scaled
		 * (an app's card, the cover sheet, the switcher): its own points */
		ox = 0;
	} else if (w.y > -1 && w.y < 1 && w.w > b.w - 1 && w.w < b.w + 1 && b.w >= sw / 2 &&
	    ((w.x > 1 && w.x + w.w > sw - 1 && w.x + w.w < sw + 1) || (w.x > -1 && w.x < 1))) {
		/* (B) part of the top bar, flush with a screen edge (Control Center's
		 * compact bar, 55-360 pt, that hands the battery back) */
		ox = w.x;
	} else {
		/* Control Center's own expanded bar (inset), scaled parts */
		if (sb_log && sb_logged < SB_LOG_MAX) {
			struct vline l;

			l.n = 0;
			vl_str(&l, "A8FIX sb: bar skipped (bounds ");
			vl_num(&l, b.w);
			vl_str(&l, " wide; in its window ");
			vl_num(&l, w.x);
			vl_str(&l, ",");
			vl_num(&l, w.y);
			vl_str(&l, " ");
			vl_num(&l, w.w);
			vl_str(&l, "x");
			vl_num(&l, w.h);
			vl_str(&l, ")\n");
			write(2, l.b, l.n);
			sb_logged++;
		}
		return;
	}
	sb_log_provider(self);
	if (ox != 0) {
		/* (B): its own avoidance frame is in the main bar's points (it put
		 * the island 55 pt too far right), so the main bar's island is used */
		if (main_set) {
			il = main_il;
			ir = main_ir;
			iy = main_iy;
			from = "the main bar's";
		} else {
			il = (sw - 125) / 2;
			ir = il + 125;
			iy = 11.3 + 36.7 / 2;
			from = "default (no main bar yet)";
		}
	} else {
		island = sb_island(self, b, 0, sw, &from);
		il = island.x;           /* screen points from here on */
		ir = il + island.w;
		iy = island.y + island.h / 2;
		if (w.x > -1 && w.x < 1 && w.y > -1 && w.y < 1 && w.w > sw - 1 && w.w < sw + 1) {
			main_il = il;    /* the bar at the screen's top left */
			main_ir = ir;
			main_iy = iy;
			main_set = 1;
		}
	}
	ic = (il + ir) / 2;
	views = get(self, sel_registerName("subviews"));
	n = views ? count(views, sel_registerName("count")) : 0;
	for (unsigned long i = 0; i < n; i++) {
		id v = at(views, sel_registerName("objectAtIndex:"), i);
		int faded;
		fix_rect f;

		if (flag(v, sel_registerName("isHidden")))
			continue;
		f = rect(v, sel_registerName("frame"));
		f.x += ox;
		if (f.w < 0.5 || f.w > sw / 3 || f.y + f.h / 2 > iy + 4)
			continue;       /* nothing bar-wide, nothing in a lower row */
		faded = dbl(v, sel_registerName("alpha")) < 0.01;
		if (f.x + f.w / 2 < ic) {
			lmin[faded] = f.x < lmin[faded] ? f.x : lmin[faded];
			lmax[faded] = f.x + f.w > lmax[faded] ? f.x + f.w : lmax[faded];
			nl[faded]++;
		} else {
			rmin[faded] = f.x < rmin[faded] ? f.x : rmin[faded];
			rmax[faded] = f.x + f.w > rmax[faded] ? f.x + f.w : rmax[faded];
			nr[faded]++;
		}
	}
	/* a group is measured from its visible items, or from the fading ones
	 * when none is visible yet */
	lv = nl[0] ? 0 : 1;
	rv = nr[0] ? 0 : 1;
	if (nl[lv]) {
		l0 = lmin[lv];
		l1 = lmax[lv];
		dl = il / 2 - (l0 + l1) / 2 - sb_out;
		if (l1 + dl > il - 4)
			dl = il - 4 - l1;
		if (l0 + dl < 6)
			dl = 6 - l0;
	}
	if (nr[rv]) {
		r0 = rmin[rv];
		r1 = rmax[rv];
		dr = (ir + sw) / 2 - (r0 + r1) / 2 + sb_out;
		if (r0 + dr < ir + 4)
			dr = ir + 4 - r0;
		if (r1 + dr > sw - 6)
			dr = sw - 6 - r1;
	}
	dl_line.n = 0;
	for (unsigned long i = 0; i < n; i++) {
		id v = at(views, sel_registerName("objectAtIndex:"), i);
		fix_rect f, g;
		int moves;

		if (flag(v, sel_registerName("isHidden")))
			continue;
		f = rect(v, sel_registerName("frame"));
		f.x += ox;
		if (f.w < 0.5 || f.w > sw / 3 || f.y + f.h / 2 > iy + 4)
			continue;
		g = f;
		g.x = sb_px(f.x + ((f.x + f.w / 2 < ic) ? dl : dr));
		if (sb_vcenter)
			g.y = sb_px(iy - f.h / 2);
		moves = g.x - f.x > 0.1 || f.x - g.x > 0.1 || g.y - f.y > 0.1 || f.y - g.y > 0.1;
		f.x -= ox;
		g.x -= ox;
		if (moves) {
			set_rect(v, sel_registerName("setFrame:"), g);
			moved++;
		}
		if (sb_log) {
			vl_str(&dl_line, "; ");
			vl_str(&dl_line, class_getName(object_getClass(v)));
			vl_str(&dl_line, " ");
			vl_num(&dl_line, f.x);
			vl_str(&dl_line, ",");
			vl_num(&dl_line, f.y);
			vl_str(&dl_line, " ");
			vl_num(&dl_line, f.w);
			vl_str(&dl_line, "x");
			vl_num(&dl_line, f.h);
			vl_str(&dl_line, " a");
			vl_num(&dl_line, dbl(v, sel_registerName("alpha")));
			vl_str(&dl_line, moves ? " ->" : " =");
			if (moves) {
				vl_num(&dl_line, g.x);
				vl_str(&dl_line, ",");
				vl_num(&dl_line, g.y);
			}
		}
	}
	if (sb_log && sb_logged < SB_LOG_MAX && (moved || nl[1] || nr[1])) {
		struct vline l;

		l.n = 0;
		vl_str(&l, "A8FIX sb pass: bar x ");
		vl_num(&l, ox);
		vl_str(&l, (b.w > sw - 1 && b.w < sw + 1) ? " (A)" : " (B)");
		vl_str(&l, " island ");
		vl_num(&l, il);
		vl_str(&l, "-");
		vl_num(&l, ir);
		vl_str(&l, " L ");
		vl_num(&l, nl[0]);
		vl_str(&l, "+");
		vl_num(&l, nl[1]);
		vl_str(&l, " faded dl ");
		vl_num(&l, dl);
		vl_str(&l, " R ");
		vl_num(&l, nr[0]);
		vl_str(&l, "+");
		vl_num(&l, nr[1]);
		vl_str(&l, " faded dr ");
		vl_num(&l, dr);
		for (unsigned long k = 0; k < dl_line.n && l.n < sizeof(l.b) - 2; k++)
			l.b[l.n++] = dl_line.b[k];
		l.b[l.n++] = '\n';
		write(2, l.b, l.n);
		sb_logged++;
	}
	if (sb_said < 12 && (moved || il != said_il || ir != said_ir)) {
		struct vline l;

		l.n = 0;
		vl_str(&l, "A8FIX status bar ears: island ");
		vl_num(&l, il);
		vl_str(&l, "-");
		vl_num(&l, ir);
		vl_str(&l, " pt, middle line ");
		vl_num(&l, iy);
		vl_str(&l, " (");
		vl_str(&l, from);
		vl_str(&l, "); left ");
		vl_num(&l, nl[0] + nl[1]);
		vl_str(&l, " items ");
		vl_num(&l, nl[lv] ? l0 : 0);
		vl_str(&l, "-");
		vl_num(&l, nl[lv] ? l1 : 0);
		vl_str(&l, " moved ");
		vl_num(&l, dl);
		vl_str(&l, ", right ");
		vl_num(&l, nr[0] + nr[1]);
		vl_str(&l, " items ");
		vl_num(&l, nr[rv] ? r0 : 0);
		vl_str(&l, "-");
		vl_num(&l, nr[rv] ? r1 : 0);
		vl_str(&l, " moved ");
		vl_num(&l, dr);
		vl_str(&l, ", out ");
		vl_num(&l, sb_out);
		vl_str(&l, sb_vcenter ? " pt; heights centred\n" : " pt; heights kept\n");
		write(2, l.b, l.n);
		said_il = il;
		said_ir = ir;
		sb_said++;
	}
}

static void
sb_fg_layout(id self, SEL cmd)
{
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	fix_rect b;
	double m;

	sb_fg_layout_orig(self, cmd);
	b = rect(self, sel_registerName("bounds"));
	if (b.w < 250 || b.w > 500 || b.h > 120)
		return;                 /* a portrait phone's bar only */
	m = sb_margin_now();
	if (m > 0)
		sb_edges(self, b, m);   /* a8fix-sbmargin holds a number: v10 */
	else
		sb_ears(self, b);
}

static int layout_kb_done, layout_cc_done, layout_sb_done;

static void
layout_apply(void)
{
	Method m;

	if (!layout_kb_done && objc_getClass("UIKeyboardImpl") != 0) {
		layout_kb_done = 1;
		m = bl_method("UIKeyboard", "defaultSizeForInterfaceOrientation:", 1, "{");
		if (m)
			kb_size_orig = (fix_size (*)(id, SEL, long))method_setImplementation(m, (IMP)kb_size);
		m = bl_method("UIKeyboardImpl", "defaultSizeForInterfaceOrientation:", 1, "{");
		if (m)
			kbi_size_orig = (fix_size (*)(id, SEL, long))method_setImplementation(m, (IMP)kbi_size);
	}
	if (!layout_sb_done && objc_getClass("_UIStatusBarForegroundView") != 0) {
		layout_sb_done = 1;
		sb_fg_layout_orig = (void (*)(id, SEL))own_method(objc_getClass("_UIStatusBarForegroundView"),
		    "layoutSubviews", (IMP)sb_fg_layout, "v16@0:8");
		fix_put(sb_fg_layout_orig ? "A8FIX status bar: layout hooked (v16c: ears, a8fix-sbout; edges with a8fix-sbmargin)\n" :
		    "A8FIX status bar: not hooked\n");
	}
	if (!layout_cc_done && objc_getClass("CCUIOverlayStatusBarPresentationProvider") != 0) {
		layout_cc_done = 1;
		m = bl_method("CCUIOverlayStatusBarPresentationProvider", "_presentedViewFrame", 0, "{");
		if (m)
			cc_presented_orig = (fix_rect (*)(id, SEL))method_setImplementation(m, (IMP)cc_presented);
		cc_scroll_layout_orig = (void (*)(id, SEL))own_method(objc_getClass("CCUIScrollView"),
		    "layoutSubviews", (IMP)cc_scroll_layout, "v16@0:8");
		cc_vwls_orig = (void (*)(id, SEL))own_method(objc_getClass("CCUIModularControlCenterOverlayViewController"),
		    "viewWillLayoutSubviews", (IMP)cc_vwls, "v16@0:8");
		fix_put(cc_vwls_orig ? "A8FIX control center: grid centering hooked\n" : "A8FIX control center: not hooked\n");
	}
}

/* ---- v14: views of the island hidden in SpringBoard ---------------------- */

/*
 * SpringBoard draws the Dynamic Island in two SBSystemApertureWindows.
 *
 * The sensor curtain (user report 11-25): the window that takes no touches
 * holds only _SBSystemApertureMagiciansCurtainView, the black cover over the
 * iPhone 14 Pro's camera and Face ID sensors, drawn above everything else.
 * This screen is 360 pt wide (no Dynamic Island iPhone is under 393), so the
 * expanded timer puts its X button partly under that cover: a pill-shaped
 * bite.  There is no camera there on this phone, so that curtain is hidden;
 * the other window's curtain still draws the idle pill under the island's
 * content.  /private/var/mobile/Library/a8fix-curtain "1" keeps it.
 *
 * The adaptive key line (v14g, P3): _SBAdaptiveKeyLineBackdropView draws a
 * faint ring around the island over dark content, fed by a
 * _UILumaTrackingBackdropView that copies and filters what is behind the
 * island.  Together they are 11 of the island's ~40 draws and two of its
 * render-target switches on every frame the island is redrawn (a running
 * Live Activity, or anything moving under the status bar).  Both are hidden
 * in the aperture windows; /private/var/mobile/Library/a8fix-keyline "1"
 * keeps them.  (Switches are read when SpringBoard starts.)
 */
#define CURTAIN_SWITCH "/private/var/mobile/Library/a8fix-curtain"
#define KEYLINE_SWITCH "/private/var/mobile/Library/a8fix-keyline"

enum { HIDE_CURTAIN, HIDE_KEYLINE, HIDE_LUMA, HIDE_N };
static const char *const hide_class[HIDE_N] = {
	"_SBSystemApertureMagiciansCurtainView",
	"_SBAdaptiveKeyLineBackdropView",
	"_UILumaTrackingBackdropView",
};
static const char *const hide_what[HIDE_N] = {
	"the sensor curtain in the touchless aperture window",
	"the adaptive key line",
	"the key line's luma tracking backdrop",
};
static void (*hide_layout_orig[HIDE_N])(id, SEL);
static void (*hide_moved_orig[HIDE_N])(id, SEL);
static void (*hide_hidden_orig[HIDE_N])(id, SEL, signed char);
static int hide_done[HIDE_N], hide_said[HIDE_N];
static struct {
	id view;
	signed char want;       /* what SpringBoard last asked for */
	signed char forced;     /* hidden here */
} hide_seen[16];
static unsigned hide_next;

/* one console line from three pieces (pieces written apart arrive as
 * separate lines) */
static void
fix_line3(const char *a, const char *b, const char *c)
{
	struct vline l;

	l.n = 0;
	vl_str(&l, a);
	vl_str(&l, b);
	vl_str(&l, c);
	l.b[l.n++] = '\n';
	write(2, l.b, l.n);
}

static unsigned
hide_slot(id view)
{
	signed char (*flag)(id, SEL) = (signed char (*)(id, SEL))objc_msgSend;
	unsigned k;

	for (k = 0; k < 16; k++)
		if (hide_seen[k].view == view)
			return k;
	k = hide_next++ % 16;
	hide_seen[k].view = view;
	hide_seen[k].want = flag(view, sel_registerName("isHidden"));
	hide_seen[k].forced = 0;
	return k;
}

/* in an aperture window (for the curtain: the one that takes no touches)? */
static int
hide_wanted(int which, id view)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	signed char (*flag)(id, SEL) = (signed char (*)(id, SEL))objc_msgSend;
	signed char (*is_kind)(id, SEL, Class) = (signed char (*)(id, SEL, Class))objc_msgSend;
	Class aperture = objc_getClass("SBSystemApertureWindow");
	id w = get(view, sel_registerName("window"));

	if (w == 0 || aperture == 0 || !is_kind(w, sel_registerName("isKindOfClass:"), aperture))
		return 0;
	return which != HIDE_CURTAIN || !flag(w, sel_registerName("isUserInteractionEnabled"));
}

static void
hide_check(int which, id view)
{
	signed char (*flag)(id, SEL) = (signed char (*)(id, SEL))objc_msgSend;
	SEL set = sel_registerName("setHidden:");
	unsigned k = hide_slot(view);

	if (hide_wanted(which, view)) {
		hide_seen[k].forced = 1;
		if (!flag(view, sel_registerName("isHidden"))) {
			hide_hidden_orig[which](view, set, 1);
			if (hide_said[which] < 2) {
				fix_line3("A8FIX island: hidden: ", hide_what[which], "");
				hide_said[which]++;
			}
		}
	} else if (hide_seen[k].forced) {
		hide_seen[k].forced = 0;        /* moved elsewhere: as SpringBoard wants */
		if (flag(view, sel_registerName("isHidden")) != hide_seen[k].want)
			hide_hidden_orig[which](view, set, hide_seen[k].want);
	}
}

static void
hide_set(int which, id view, SEL cmd, signed char hidden)
{
	unsigned k = hide_slot(view);

	hide_seen[k].want = hidden;
	if (!hidden && hide_wanted(which, view)) {
		hide_seen[k].forced = 1;
		hidden = 1;
	}
	hide_hidden_orig[which](view, cmd, hidden);
}

#define HIDE_HOOKS(K) \
static void hide_layout_##K(id s, SEL c) { hide_layout_orig[K](s, c); hide_check(K, s); } \
static void hide_moved_##K(id s, SEL c) { hide_moved_orig[K](s, c); hide_check(K, s); } \
static void hide_hidden_##K(id s, SEL c, signed char h) { hide_set(K, s, c, h); }
HIDE_HOOKS(0)
HIDE_HOOKS(1)
HIDE_HOOKS(2)
static const IMP hide_layout_imp[HIDE_N] = { (IMP)hide_layout_0, (IMP)hide_layout_1, (IMP)hide_layout_2 };
static const IMP hide_moved_imp[HIDE_N] = { (IMP)hide_moved_0, (IMP)hide_moved_1, (IMP)hide_moved_2 };
static const IMP hide_hidden_imp[HIDE_N] = { (IMP)hide_hidden_0, (IMP)hide_hidden_1, (IMP)hide_hidden_2 };

static int
hide_keep(const char *path)
{
	id text = fix_switch_text(path);

	return text != 0 && ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String"))[0] == '1';
}

static void
curtain_apply(void)
{
	for (int k = 0; k < HIDE_N; k++) {
		Class c;

		if (hide_done[k] || (c = objc_getClass(hide_class[k])) == 0)
			continue;
		hide_done[k] = 1;
		if (hide_keep(k == HIDE_CURTAIN ? CURTAIN_SWITCH : KEYLINE_SWITCH)) {
			fix_line3("A8FIX island: kept (switch \"1\"): ", hide_what[k], "");
			continue;
		}
		hide_hidden_orig[k] = (void (*)(id, SEL, signed char))own_method(c, "setHidden:",
		    hide_hidden_imp[k], "v20@0:8B16");
		if (hide_hidden_orig[k] == 0) {
			fix_line3("A8FIX island: not hooked: ", hide_class[k], "");
			continue;
		}
		hide_layout_orig[k] = (void (*)(id, SEL))own_method(c, "layoutSubviews", hide_layout_imp[k], "v16@0:8");
		hide_moved_orig[k] = (void (*)(id, SEL))own_method(c, "didMoveToWindow", hide_moved_imp[k], "v16@0:8");
		fix_line3("A8FIX island: hooks in for ", hide_class[k], "");
	}
}

/*
 * v14g: the island's content draws its countdown with a black shadow at 33%
 * (ClockAngel's layers): on the island's black platter it cannot be seen,
 * and it costs 6 draws and 4 render-target switches each time the island is
 * redrawn.  In a process whose name ends in "Angel", -[CALayer
 * setShadowOpacity:] sets 0; /private/var/mobile/Library/a8fix-angelshadow
 * "1" keeps the shadows.
 */
#define ANGEL_SHADOW_SWITCH "/private/var/mobile/Library/a8fix-angelshadow"

static void (*shadow_set_orig)(id, SEL, float);
static int shadow_said;

static void
shadow_set(id self, SEL cmd, float opacity)
{
	if (opacity > 0 && shadow_said < 3) {
		fix_put("A8FIX island content: a layer shadow dropped (a8fix-angelshadow \"1\" keeps it)\n");
		shadow_said++;
	}
	shadow_set_orig(self, cmd, 0);
}

/* ---- v17: backdrop capture scale (P3a, Control Center's GPU time) ------ */

/*
 * v17: every material backdrop here captures at 100% (the view dump's "scale
 * 100%"), so Control Center's full-screen material copies the whole home
 * screen (1082x2222) every frame before blurring it.  -[CABackdropLayer
 * setScale:] keeps a backdrop's scale at most 25% and every new backdrop
 * layer starts there (11-26: Control Center 353-362 -> 446-450 frames a 10 s,
 * build 19.6 -> 15.5 ms, the picture the same to the eye: a blur hides the
 * capture's resolution).  /private/var/mobile/Library/a8fix-bdscale: another
 * cap (0.05 to 0.99), or "1" for Apple's scales.
 */
#define BD_SCALE_SWITCH "/private/var/mobile/Library/a8fix-bdscale"

static double bd_cap;
static void (*bd_scale_orig)(id, SEL, double);
static id (*bd_init_orig)(id, SEL);
static int bd_done;

static void
bd_scale(id self, SEL cmd, double scale)
{
	if (bd_cap > 0 && scale > bd_cap)
		scale = bd_cap;
	bd_scale_orig(self, cmd, scale);
}

static id
bd_init(id self, SEL cmd)
{
	self = bd_init_orig(self, cmd);
	if (self != 0 && bd_cap > 0)
		bd_scale_orig(self, sel_registerName("setScale:"), bd_cap);
	return self;
}

static void
bd_apply(void)
{
	Class c;
	id text;
	struct vline l;

	if (bd_done || (c = objc_getClass("CABackdropLayer")) == 0)
		return;
	bd_done = 1;
	text = fix_switch_text(BD_SCALE_SWITCH);
	bd_cap = text != 0 ? ((double (*)(id, SEL))objc_msgSend)(text, sel_registerName("doubleValue")) : 0.25;
	if (!(bd_cap >= 0.05 && bd_cap < 1)) {
		bd_cap = 0;
		fix_put("A8FIX backdrop scale: Apple's scales (a8fix-bdscale)\n");
		return;
	}
	bd_scale_orig = (void (*)(id, SEL, double))own_method(c, "setScale:", (IMP)bd_scale, "v24@0:8d16");
	if (bd_scale_orig == 0) {
		bd_cap = 0;
		fix_put("A8FIX backdrop scale: -[CABackdropLayer setScale:] not found\n");
		return;
	}
	bd_init_orig = (id (*)(id, SEL))own_method(c, "init", (IMP)bd_init, "@16@0:8");
	l.n = 0;
	vl_str(&l, "A8FIX backdrop scale: at most ");
	vl_num(&l, bd_cap * 100.0);
	vl_str(&l, bd_init_orig ? "% (setScale: and new layers)\n" : "% (setScale: only)\n");
	write(2, l.b, l.n);
}

/* ---- v17d: CoreImage without a GL path (Bold Text crash) --------------- */
/*
 * iOS 16's -[CIContext initWithOptions:] has no CPU renderer (it turns
 * "software_renderer" into "priority_request_low") and, with no MTLDevice,
 * makes a GL context: +[CIContext internalGLContextWithOptions:] ->
 * +internalContextWithEAGLContext:options: -> CI::GLContext, which takes the
 * GL entry points from -[EAGLContext getMacroContextPrivate] (Apple's private
 * GLES context).  Our EAGL has none, CoreImage's table stays all zeros and its
 * first call (glDisable(GL_DEPTH_TEST), CoreImage +0x19c588) jumps to 0: every
 * CIContext crashed its process, e.g. SpringBoard with Bold Text on (CoreUI's
 * shared context; CoreImage +0x6f10/+0x7114/+0x45884, read from the phone's
 * dyld cache 11-26).  Both makers answer nil: initWithOptions: then returns nil
 * (it checks), so the caller gets no context instead of a crash.
 * /private/var/mobile/Library/a8fix-cigl "1" keeps Apple's path.
 */
#define CI_GL_SWITCH "/private/var/mobile/Library/a8fix-cigl"
static int ci_done;
static unsigned ci_refused;

static void
ci_say(const char *what)
{
	struct vline l;

	if (ci_refused++ >= 3)
		return;
	l.n = 0;
	vl_str(&l, "A8FIX CoreImage: no GL context (");
	vl_str(&l, what);
	vl_str(&l, ") in ");
	vl_str(&l, getprogname());
	l.b[l.n++] = '\n';
	write(2, l.b, l.n);
}

static id
ci_no_gl(id self, SEL cmd, id options)
{
	(void)self;
	(void)cmd;
	(void)options;
	ci_say("internalGLContextWithOptions:");
	return 0;
}

static id
ci_no_eagl(id self, SEL cmd, id eagl, id options)
{
	(void)self;
	(void)cmd;
	(void)eagl;
	(void)options;
	ci_say("internalContextWithEAGLContext:options:");
	return 0;
}

static void
ci_apply(void)
{
	Class c;
	Method gl, eagl;
	id text;

	if (ci_done || (c = objc_getClass("CIContext")) == 0)
		return;
	ci_done = 1;
	text = fix_switch_text(CI_GL_SWITCH);
	if (text != 0 && ((int (*)(id, SEL))objc_msgSend)(text, sel_registerName("intValue")) == 1) {
		fix_put("A8FIX CoreImage: Apple's GL path kept (a8fix-cigl)\n");
		return;
	}
	gl = class_getClassMethod(c, sel_registerName("internalGLContextWithOptions:"));
	eagl = class_getClassMethod(c, sel_registerName("internalContextWithEAGLContext:options:"));
	if (gl != 0)
		method_setImplementation(gl, (IMP)ci_no_gl);
	if (eagl != 0)
		method_setImplementation(eagl, (IMP)ci_no_eagl);
	if (gl == 0 || eagl == 0)
		fix_put("A8FIX CoreImage: a GL context maker was not found\n");
}

/* ---- v14b: long CoreAnimation animations (P3, Live Activity cost) ------- */

/*
 * A timer Live Activity keeps backboardd drawing the island 60 times a
 * second (11-25: 598 frames in 10 s on an idle home screen, 52 draws each):
 * the end of the timer's progress ring moves by a thousandth of a pixel a
 * frame.  When /private/var/mobile/Library/a8fix-animlog exists (read when
 * the process starts), -[CALayer addAnimation:forKey:] is watched: an
 * animation longer than 2 s is logged ("A8FIX anim: ..."), and if the file
 * holds a number N from 1 to 30 its preferred frame rate range becomes
 * 1..N (CAAnimation.preferredFrameRateRange, iOS 15) before it is added.
 */
#define ANIM_SWITCH "/private/var/mobile/Library/a8fix-animlog"

typedef struct { float minimum, maximum, preferred; } fix_fps_range;

static void (*anim_add_orig)(id, SEL, id, id);
static int anim_fps;                    /* 0: log only */
static int anim_all;                    /* in an "Angel": every animation */
static int anim_said;

static void
anim_add(id self, SEL cmd, id anim, id key)
{
	signed char (*responds)(id, SEL, SEL) = (signed char (*)(id, SEL, SEL))objc_msgSend;
	double (*dbl)(id, SEL) = (double (*)(id, SEL))objc_msgSend;
	float (*flt)(id, SEL) = (float (*)(id, SEL))objc_msgSend;
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	SEL sel_fps = sel_registerName("preferredFrameRateRange");
	SEL sel_set_fps = sel_registerName("setPreferredFrameRateRange:");
	double d = 0;
	float rep = 0;

	if (anim != 0 && responds(anim, sel_registerName("respondsToSelector:"), sel_registerName("duration"))) {
		d = dbl(anim, sel_registerName("duration"));
		rep = flt(anim, sel_registerName("repeatCount"));
	}
	if (d > 2.0 || rep > 100 || anim_all) {
		fix_fps_range before = { 0, 0, 0 }, now;
		int has = responds(anim, sel_registerName("respondsToSelector:"), sel_fps);

		if (has)
			before = ((fix_fps_range (*)(id, SEL))objc_msgSend)(anim, sel_fps);
		if (anim_fps > 0 && has && d < 1e9 && rep < 1e9 &&
		    responds(anim, sel_registerName("respondsToSelector:"), sel_set_fps)) {
			now.minimum = 1;
			now.maximum = (float)anim_fps;
			now.preferred = (float)anim_fps;
			((void (*)(id, SEL, fix_fps_range))objc_msgSend)(anim, sel_set_fps, now);
		}
		if (anim_said < 60) {
			struct vline l;
			id path = responds(anim, sel_registerName("respondsToSelector:"), sel_registerName("keyPath")) ?
			    get(anim, sel_registerName("keyPath")) : 0;

			l.n = 0;
			vl_str(&l, "A8FIX anim: layer ");
			vl_str(&l, class_getName(object_getClass(self)));
			vl_str(&l, " key ");
			vl_str(&l, key ? fix_utf8(key) : "-");
			vl_str(&l, " ");
			vl_str(&l, class_getName(object_getClass(anim)));
			vl_str(&l, " path ");
			vl_str(&l, path ? fix_utf8(path) : "-");
			vl_str(&l, " duration ");
			if (d > 1e9)
				vl_str(&l, "inf");
			else
				vl_num(&l, d);
			vl_str(&l, " s repeat ");
			if (rep > 1e9)
				vl_str(&l, "inf");
			else
				vl_num(&l, rep);
			vl_str(&l, " fps ");
			vl_num(&l, before.minimum);
			vl_str(&l, "-");
			vl_num(&l, before.maximum);
			vl_str(&l, "/");
			vl_num(&l, before.preferred);
			if (anim_fps > 0)
				vl_str(&l, has ? " -> 1-N" : " (no range)");
			vl_str(&l, "\n");
			write(2, l.b, l.n);
			anim_said++;
		}
	}
	anim_add_orig(self, cmd, anim, key);
}

/*
 * The timer's island content is drawn by ClockAngel (SwiftUI), which
 * updates the progress ring on a CADisplayLink every frame (11-25: ClockAngel
 * 10% CPU and backboardd 60% on an idle home screen): its
 * ClockAngel.TimerAnimationCountdown objects each run a CADisplayLink at 30
 * frames a second for a ring that moves a thousandth of a pixel a frame.  In
 * a process whose name ends in "Angel", display links made for a target
 * whose class name has "Countdown" in it are capped at 10 frames a second
 * (the digits still turn over within 0.1 s of each second), and each tick's
 * changes are made without implicit animations (below); other links are
 * only logged (40 lines).  /private/var/mobile/Library/a8fix-angelfps holding
 * N (1-60) caps at N instead; "0" leaves Apple's rate and animations.
 */
#define ANGEL_FPS_SWITCH "/private/var/mobile/Library/a8fix-angelfps"

static int fix_streq(const char *a, const char *b);

static id (*dl_make_orig)(id, SEL, id, SEL);
static void (*dl_range_orig)(id, SEL, fix_fps_range);
static void (*dl_fps_orig)(id, SEL, long);
static int dl_fps = 10, dl_said;
static id dl_capped[16];                /* the countdown's links */
static unsigned dl_next;

static int
dl_is_capped(id link)
{
	for (unsigned k = 0; k < 16; k++)
		if (dl_capped[k] == link)
			return 1;
	return 0;
}

static int
fix_contains_word(const char *s, const char *word)
{
	for (; s && *s; s++) {
		const char *a = s, *b = word;

		while (*a && *b && *a == *b) {
			a++;
			b++;
		}
		if (*b == 0)
			return 1;
	}
	return 0;
}

static void
dl_log(const char *what, id link, double a, double b, double c)
{
	struct vline l;

	if (dl_said >= 40)
		return;
	dl_said++;
	l.n = 0;
	vl_str(&l, "A8FIX displaylink: ");
	vl_str(&l, what);
	vl_str(&l, " ");
	vl_num(&l, a);
	vl_str(&l, " ");
	vl_num(&l, b);
	vl_str(&l, " ");
	vl_num(&l, c);
	vl_str(&l, dl_is_capped(link) ? " (countdown link)\n" : "\n");
	write(2, l.b, l.n);
}

/*
 * Each tick, updateCountdownAnimation sets the ring's strokeStart and the
 * end cap's transform with Core Animation's implicit actions on: every tick
 * starts a 0.25 s animation, so with ticks 30 (or 10) times a second there is
 * always one running and backboardd redraws the island every frame.  The
 * tick's changes are made with actions off (one redraw a tick).
 */
static void (*cd_update_orig)(id, SEL);
static Class cd_class;

static void
cd_update(id self, SEL cmd)
{
	Class tx = objc_getClass("CATransaction");

	if (tx == 0) {
		cd_update_orig(self, cmd);
		return;
	}
	((void (*)(Class, SEL))objc_msgSend)(tx, sel_registerName("begin"));
	((void (*)(Class, SEL, signed char))objc_msgSend)(tx, sel_registerName("setDisableActions:"), 1);
	cd_update_orig(self, cmd);
	((void (*)(Class, SEL))objc_msgSend)(tx, sel_registerName("commit"));
}

static void
cd_hook(Class c, SEL action)
{
	Method m;

	if (cd_class != 0 || c == 0 || action == 0)
		return;
	cd_class = c;
	m = class_getInstanceMethod(c, action);
	if (m == 0 || !fix_streq(sel_getName(action), "updateCountdownAnimation"))
		return;
	cd_update_orig = (void (*)(id, SEL))method_setImplementation(m, (IMP)cd_update);
	fix_put("A8FIX displaylink: countdown ticks without implicit animations\n");
}

static id
dl_make(id self, SEL cmd, id target, SEL action)
{
	id link = dl_make_orig(self, cmd, target, action);
	const char *who = target ? class_getName(object_getClass(target)) : "-";
	int countdown = fix_contains_word(who, "Countdown");
	struct vline l;

	if (countdown && target != 0 && dl_fps > 0)
		cd_hook(object_getClass(target), action);
	if (link != 0 && countdown && dl_fps > 0) {
		dl_capped[dl_next++ % 16] = link;
		if (dl_fps_orig != 0)
			dl_fps_orig(link, sel_registerName("setPreferredFramesPerSecond:"), dl_fps);
	}
	if (dl_said < 40) {
		dl_said++;
		l.n = 0;
		vl_str(&l, "A8FIX displaylink: made for ");
		vl_str(&l, who);
		vl_str(&l, " -");
		vl_str(&l, action ? (const char *)action : "?");
		vl_str(&l, countdown && dl_fps > 0 ? " (capped)\n" : "\n");
		write(2, l.b, l.n);
	}
	return link;
}

static void
dl_range(id self, SEL cmd, fix_fps_range r)
{
	dl_log("range asked", self, r.minimum, r.maximum, r.preferred);
	if (dl_fps > 0 && dl_is_capped(self) && r.maximum > 0) {
		float n = (float)dl_fps;

		r.maximum = r.maximum > n ? n : r.maximum;
		r.preferred = (r.preferred > r.maximum || r.preferred <= 0) ? r.maximum : r.preferred;
		r.minimum = r.minimum > r.preferred ? r.preferred : r.minimum;
		if (r.minimum < 1)
			r.minimum = 1;
		if (r.preferred < r.minimum)
			r.preferred = r.minimum;
		if (r.maximum < r.preferred)
			r.maximum = r.preferred;
	}
	dl_range_orig(self, cmd, r);
}

static void
dl_set_fps(id self, SEL cmd, long fps)
{
	dl_log("fps asked", self, (double)fps, 0, 0);
	if (dl_fps > 0 && dl_is_capped(self) && (fps == 0 || fps > dl_fps))
		fps = dl_fps;
	dl_fps_orig(self, cmd, fps);
}

static int
fix_ends_with(const char *s, const char *tail)
{
	unsigned long a = 0, b = 0;

	while (s && s[a])
		a++;
	while (tail[b])
		b++;
	return s && a >= b && fix_streq(s + a - b, tail);
}

static void
dl_apply(void)
{
	Class link = objc_getClass("CADisplayLink");
	id text;
	Method m;

	if (link == 0 || !fix_ends_with(getprogname(), "Angel"))
		return;
	text = fix_switch_text(ANGEL_FPS_SWITCH);
	if (text != 0) {
		dl_fps = ((int (*)(id, SEL))objc_msgSend)(text, sel_registerName("intValue"));
		if (dl_fps < 1 || dl_fps > 60)
			dl_fps = 0;             /* "0" (or junk): Apple's own 30 */
	}
	if (!hide_keep(ANGEL_SHADOW_SWITCH) && objc_getClass("CALayer") != 0 &&
	    (m = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("setShadowOpacity:"))) != 0)
		shadow_set_orig = (void (*)(id, SEL, float))method_setImplementation(m, (IMP)shadow_set);
	m = class_getClassMethod(link, sel_registerName("displayLinkWithTarget:selector:"));
	if (m)
		dl_make_orig = (id (*)(id, SEL, id, SEL))method_setImplementation(m, (IMP)dl_make);
	m = class_getInstanceMethod(link, sel_registerName("setPreferredFrameRateRange:"));
	if (m)
		dl_range_orig = (void (*)(id, SEL, fix_fps_range))method_setImplementation(m, (IMP)dl_range);
	m = class_getInstanceMethod(link, sel_registerName("setPreferredFramesPerSecond:"));
	if (m)
		dl_fps_orig = (void (*)(id, SEL, long))method_setImplementation(m, (IMP)dl_set_fps);
	fix_put(dl_fps ? "A8FIX displaylink: countdown links capped (a8fix-angelfps)\n" :
	    "A8FIX displaylink: hooked, countdown at Apple's rate (a8fix-angelfps \"0\")\n");
}

static int anim_done;

static void
anim_apply(void)
{
	Class layer;
	Method m;
	id text;

	if (anim_done || (layer = objc_getClass("CALayer")) == 0)
		return;
	anim_done = 1;
	dl_apply();
	text = fix_switch_text(ANIM_SWITCH);
	if (text == 0)
		return;
	anim_fps = ((int (*)(id, SEL))objc_msgSend)(text, sel_registerName("intValue"));
	anim_all = fix_ends_with(getprogname(), "Angel");
	if (anim_fps < 1 || anim_fps > 30)
		anim_fps = 0;
	m = class_getInstanceMethod(layer, sel_registerName("addAnimation:forKey:"));
	if (m == 0)
		return;
	anim_add_orig = (void (*)(id, SEL, id, id))method_setImplementation(m, (IMP)anim_add);
	fix_put(anim_fps ? "A8FIX anim: long animations logged and slowed (a8fix-animlog)\n" :
	    "A8FIX anim: long animations logged (a8fix-animlog)\n");
}

/* ---- Auto-Lock (v8) --------------------------------------------------- */

static void (*tel_restart_orig)(id, SEL, id);
static void (*idle_reset_orig)(id, SEL);
static int in_tel_restart;
static unsigned long tel_dropped;

static void
tel_restart(id self, SEL cmd, id arg)
{
	in_tel_restart++;
	tel_restart_orig(self, cmd, arg);
	in_tel_restart--;
}

static void
idle_reset(id self, SEL cmd)
{
	if (in_tel_restart > 0) {
		if (tel_dropped++ == 0)
			fix_put("A8FIX auto-lock: an idle-timer reset from the telephony retry dropped (no CommCenter here)\n");
		return;
	}
	idle_reset_orig(self, cmd);
}

static int idle_done;

static void
idle_apply(void)
{
	Method m, r;

	if (idle_done || objc_getClass("SBTelephonyManager") == 0 || objc_getClass("SBIdleTimerGlobalCoordinator") == 0)
		return;
	idle_done = 1;
	m = class_getInstanceMethod(objc_getClass("SBTelephonyManager"), sel_registerName("_handleTelephonyDaemonRestart"));
	r = class_getInstanceMethod(objc_getClass("SBIdleTimerGlobalCoordinator"), sel_registerName("resetIdleTimer"));
	if (m == 0 || r == 0) {
		fix_put("A8FIX auto-lock: methods not found, not hooked\n");
		return;
	}
	tel_restart_orig = (void (*)(id, SEL, id))method_setImplementation(m, (IMP)tel_restart);
	idle_reset_orig = (void (*)(id, SEL))method_setImplementation(r, (IMP)idle_reset);
	fix_put("A8FIX auto-lock: telephony retry resets dropped\n");
}

static int fix_is_springboard;           /* set in fix_init */

static void
fix_image_added(const void *mh, long slide)
{
	(void)mh;
	(void)slide;
	fix_apply();
	bl_apply();
	layout_apply();
	idle_apply();
	anim_apply();
	bd_apply();
	ci_apply();
	if (fix_is_springboard)
		curtain_apply();
}

/* ---- what UIKit and MobileGestalt think the screen is (a8fix-screen log) ---- */


static void
fix_num(double v)
{
	char b[32];
	int n = 0;
	long long whole;
	int frac;

	if (v < 0) {
		b[n++] = '-';
		v = -v;
	}
	whole = (long long)v;
	frac = (int)((v - (double)whole) * 1000.0 + 0.5);
	if (frac >= 1000) {
		whole++;
		frac -= 1000;
	}
	{
		char t[24];
		int k = 0;

		do {
			t[k++] = (char)('0' + whole % 10);
			whole /= 10;
		} while (whole > 0 && k < 20);
		while (k > 0)
			b[n++] = t[--k];
	}
	b[n++] = '.';
	b[n++] = (char)('0' + frac / 100);
	b[n++] = (char)('0' + frac / 10 % 10);
	b[n++] = (char)('0' + frac % 10);
	b[n] = 0;
	fix_put(b);
}

static void
fix_mg(const char *key)
{
	id (*make)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;
	Class string = objc_getClass("NSString");
	id k;
	void *v;

	if (string == 0)
		return;
	k = make(string, sel_registerName("stringWithUTF8String:"), key);
	v = MGCopyAnswer((void *)k);
	fix_put(" ");
	fix_put(key);
	fix_put("=");
	fix_put(v ? fix_utf8((id)v) : "(none)");
}

static void
fix_screen_report(void *context)
{
	Class ui = objc_getClass("UIScreen");
	id (*get)(Class, SEL) = (id (*)(Class, SEL))objc_msgSend;
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	double (*dbl)(id, SEL) = (double (*)(id, SEL))objc_msgSend;
	id screen;
	fix_rect r;

	(void)context;
	fix_put("A8FIX screen: ");
	fix_put(getprogname());
	if (ui != 0 && (screen = get(ui, sel_registerName("mainScreen"))) != 0 &&
	    ((signed char (*)(id, SEL, SEL))objc_msgSend)(screen, sel_registerName("respondsToSelector:"),
	    sel_registerName("_referenceBounds"))) {
		r = rect(screen, sel_registerName("bounds"));
		fix_put(" bounds ");
		fix_num(r.w);
		fix_put("x");
		fix_num(r.h);
		fix_put(" scale ");
		fix_num(dbl(screen, sel_registerName("scale")));
		fix_put(" nativeScale ");
		fix_num(dbl(screen, sel_registerName("nativeScale")));
		r = rect(screen, sel_registerName("nativeBounds"));
		fix_put(" nativeBounds ");
		fix_num(r.w);
		fix_put("x");
		fix_num(r.h);
		r = rect(screen, sel_registerName("_referenceBounds"));
		fix_put(" referenceBounds ");
		fix_num(r.w);
		fix_put("x");
		fix_num(r.h);
	}
	fix_put("\nA8FIX gestalt:");
	fix_mg("main-screen-width");
	fix_mg("main-screen-height");
	fix_mg("main-screen-scale");
	fix_mg("ArtworkDeviceSubType");
	fix_mg("ArtworkTraits");
	fix_mg("DeviceClassNumber");
	fix_mg("ProductType");
	fix_put("\n");
}

/* ---- os_log to stderr ------------------------------------------------ */

/* one line, written at once (pieces of a line written apart arrive on the
 * console as separate lines) */
static void
log_line(unsigned char type, const char *text)
{
	char line[1024];
	unsigned long n = 0;
	const char *kind = type == 0x10 ? "error" : type == 0x11 ? "fault" :
	    type == 0x01 ? "info" : type == 0x02 ? "debug" : "default";

	for (const char *k = "A8LOG "; *k; k++)
		line[n++] = *k;
	for (const char *k = kind; *k; k++)
		line[n++] = *k;
	line[n++] = ' ';
	for (const char *t = text ? text : "(null)"; *t && n < sizeof(line) - 2; t++)
		line[n++] = (*t == '\n' || *t == '\r') ? ' ' : *t;
	line[n++] = '\n';
	write(2, line, n);
}

/* is this process tee'd (the kernel put OS_ACTIVITY_DT_MODE in its environment)? */
static int
log_wanted(void)
{
	return getenv("OS_ACTIVITY_DT_MODE") != 0;
}

static void
log_apply(void)
{
	if (!log_wanted())
		return;
	os_log_set_hook(0x00, ^(unsigned char type, os_log_message_t message) {
		char *text = os_log_copy_message_string(message);

		log_line(type, text);
		free(text);
	});
	log_line(0x00, "A8FIX: os_log goes to stderr in this process");
}

/* ---- view dump (SIGUSR1) --------------------------------------------- */

static unsigned dump_lines;

static void
dump_view(id view, int depth)
{
	SEL sel_subviews = sel_registerName("subviews");
	signed char (*is)(id, SEL) = (signed char (*)(id, SEL))objc_msgSend;
	signed char (*responds)(id, SEL, SEL) = (signed char (*)(id, SEL, SEL))objc_msgSend;
	double (*dbl)(id, SEL) = (double (*)(id, SEL))objc_msgSend;
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	fix_rect (*convert)(id, SEL, fix_rect, id) = (fix_rect (*)(id, SEL, fix_rect, id))objc_msgSend;
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
	unsigned long (*count)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	struct vline l;
	fix_rect r;
	id subviews, text;
	unsigned long n;

	if (view == 0 || depth > 40 || dump_lines > 6000)
		return;
	if (is(view, sel_registerName("isHidden")) || dbl(view, sel_registerName("alpha")) <= 0.0)
		return;
	r = convert(view, sel_registerName("convertRect:toView:"), rect(view, sel_registerName("bounds")), 0);
	l.n = 0;
	vl_str(&l, "A8VIEW ");
	vl_num(&l, depth);
	vl_str(&l, " ");
	vl_str(&l, class_getName(object_getClass(view)));
	vl_str(&l, " ");
	vl_num(&l, r.x);
	vl_str(&l, ",");
	vl_num(&l, r.y);
	vl_str(&l, " ");
	vl_num(&l, r.w);
	vl_str(&l, "x");
	vl_num(&l, r.h);
	if (!is(view, sel_registerName("isUserInteractionEnabled")))
		vl_str(&l, " noui");
	/* v15: a backdrop layer's class, group and capture scale */
	{
		id layer = get(view, sel_registerName("layer"));
		SEL rts = sel_registerName("respondsToSelector:");

		if (layer != 0 && responds(layer, rts, sel_registerName("groupName"))) {
			id g = get(layer, sel_registerName("groupName"));

			vl_str(&l, " layer ");
			vl_str(&l, class_getName(object_getClass(layer)));
			vl_str(&l, " group ");
			vl_str(&l, g ? ((const char *(*)(id, SEL))objc_msgSend)(g, sel_registerName("UTF8String")) : "-");
			if (responds(layer, rts, sel_registerName("scale"))) {
				vl_str(&l, " scale ");
				vl_num(&l, dbl(layer, sel_registerName("scale")) * 100.0);
				vl_str(&l, "%");
			}
		}
	}
	if (responds(view, sel_registerName("respondsToSelector:"), sel_registerName("isEnabled")) &&
	    !is(view, sel_registerName("isEnabled")))
		vl_str(&l, " disabled");
	if (responds(view, sel_registerName("respondsToSelector:"), sel_registerName("text")) &&
	    responds(view, sel_registerName("isKindOfClass:"), (SEL)objc_getClass("UILabel")) &&
	    (text = get(view, sel_registerName("text"))) != 0) {
		vl_str(&l, " \"");
		vl_str(&l, ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")));
		vl_str(&l, "\"");
	}
	l.b[l.n++] = '\n';
	write(2, l.b, l.n);
	dump_lines++;
	subviews = get(view, sel_subviews);
	n = subviews ? count(subviews, sel_registerName("count")) : 0;
	for (unsigned long i = 0; i < n; i++)
		dump_view(at(subviews, sel_registerName("objectAtIndex:"), i), depth + 1);
}

/*
 * v9: what an SF Symbol rasterizes to in this process.  Apps' symbols reach
 * the display server as surfaces with alpha 0 everywhere (glca9 log), so the
 * question is whether UIKit/CoreUI draw anything at all here.  Run with the
 * view dump (console "kill NAME 30" on a tee'd process): for a few symbols,
 * the UIImage's size, its own CGImage if it has one, and a 3x render through
 * UIGraphicsImageRenderer, each as "bytes, nonzero bytes, largest byte".
 */
static id sym_image;

static void
sym_scan(const char *name, const char *what, void *cg)
{
	struct vline l;
	void *data;
	const unsigned char *p;
	long n, nonzero = 0;
	unsigned best = 0;

	l.n = 0;
	vl_str(&l, "A8SYM ");
	vl_str(&l, name);
	vl_str(&l, " ");
	vl_str(&l, what);
	if (cg == 0) {
		vl_str(&l, ": none\n");
		write(2, l.b, l.n);
		return;
	}
	vl_str(&l, ": ");
	vl_num(&l, (double)CGImageGetWidth(cg));
	vl_str(&l, "x");
	vl_num(&l, (double)CGImageGetHeight(cg));
	vl_str(&l, " bpp ");
	vl_num(&l, (double)CGImageGetBitsPerPixel(cg));
	vl_str(&l, " info ");
	vl_num(&l, (double)CGImageGetBitmapInfo(cg));
	data = CGImageGetDataProvider(cg) ? CGDataProviderCopyData(CGImageGetDataProvider(cg)) : 0;
	if (data == 0) {
		vl_str(&l, ", no data\n");
		write(2, l.b, l.n);
		return;
	}
	p = CFDataGetBytePtr(data);
	n = CFDataGetLength(data);
	for (long i = 0; p != 0 && i < n; i++) {
		if (p[i] != 0) {
			nonzero++;
			if (p[i] > best)
				best = p[i];
		}
	}
	CFRelease(data);
	vl_str(&l, ", bytes ");
	vl_num(&l, (double)n);
	vl_str(&l, " nonzero ");
	vl_num(&l, (double)nonzero);
	vl_str(&l, " max ");
	vl_num(&l, (double)best);
	vl_str(&l, "\n");
	write(2, l.b, l.n);
}

static void
symbol_test(void)
{
	static const char *const names[] = { "chevron.backward", "magnifyingglass", "checkmark",
	                                     "sun.max", "phone.fill" };
	Class image = objc_getClass("UIImage"), string = objc_getClass("NSString");
	Class renderer = objc_getClass("UIGraphicsImageRenderer");
	id (*make)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;
	id (*named)(Class, SEL, id) = (id (*)(Class, SEL, id))objc_msgSend;
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	void *(*cgimage)(id, SEL) = (void *(*)(id, SEL))objc_msgSend;
	id (*init_size)(id, SEL, double, double) = (id (*)(id, SEL, double, double))objc_msgSend;
	id (*draw)(id, SEL, void *) = (id (*)(id, SEL, void *))objc_msgSend;
	fix_size (*size)(id, SEL) = (fix_size (*)(id, SEL))objc_msgSend;

	if (image == 0 || string == 0)
		return;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		id img = named(image, sel_registerName("systemImageNamed:"),
		               make(string, sel_registerName("stringWithUTF8String:"), names[i]));
		struct vline l;

		l.n = 0;
		if (img == 0) {
			vl_str(&l, "A8SYM ");
			vl_str(&l, names[i]);
			vl_str(&l, ": no image\n");
			write(2, l.b, l.n);
			continue;
		}
		{
			fix_size sz = size(img, sel_registerName("size"));

			vl_str(&l, "A8SYM ");
			vl_str(&l, names[i]);
			vl_str(&l, ": size ");
			vl_num(&l, sz.w);
			vl_str(&l, "x");
			vl_num(&l, sz.h);
			vl_str(&l, " pt\n");
			write(2, l.b, l.n);
			sym_scan(names[i], "own CGImage", cgimage(img, sel_registerName("CGImage")));
			if (renderer != 0 && sz.w > 0 && sz.h > 0) {
				id r = init_size(get((id)renderer, sel_registerName("alloc")),
				                 sel_registerName("initWithSize:"), sz.w, sz.h);
				id out;

				sym_image = img;
				out = draw(r, sel_registerName("imageWithActions:"), (void *)^(id context) {
					(void)context;
					((void (*)(id, SEL, double, double))objc_msgSend)(sym_image,
					    sel_registerName("drawAtPoint:"), 0.0, 0.0);
				});
				sym_scan(names[i], "rendered", out ? cgimage(out, sel_registerName("CGImage")) : 0);
			}
		}
	}
}

static void
dump_windows(void *context)
{
	Class window = objc_getClass("UIWindow");
	Class app = objc_getClass("UIApplication");
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id (*all)(Class, SEL, signed char, signed char) = (id (*)(Class, SEL, signed char, signed char))objc_msgSend;
	id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
	unsigned long (*count)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	signed char (*responds)(id, SEL, SEL) = (signed char (*)(id, SEL, SEL))objc_msgSend;
	SEL every = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	id windows = 0;
	unsigned long n;

	(void)context;
	if (window == 0)
		return;
	if (responds((id)window, sel_registerName("respondsToSelector:"), every))
		windows = all(window, every, 1, 1);
	else if (app != 0)
		windows = get(get((id)app, sel_registerName("sharedApplication")), sel_registerName("windows"));
	dump_lines = 0;
	n = windows ? count(windows, sel_registerName("count")) : 0;
	log_line(0x00, "A8VIEW begin");
	for (unsigned long i = 0; i < n; i++)
		dump_view(at(windows, sel_registerName("objectAtIndex:"), i), 0);
	log_line(0x00, "A8VIEW end");
	symbol_test();
}

static void
dump_arm(void)
{
	void *source;

	signal(30, (void (*)(int))1);   /* SIGUSR1: SIG_IGN, the source takes it */
	source = dispatch_source_create(&_dispatch_source_type_signal, 30, 0, &_dispatch_main_q);
	if (source == 0)
		return;
	dispatch_source_set_event_handler_f(source, dump_windows);
	dispatch_resume(source);
}

/* ---- v11: the wallpaper extensions told to LaunchServices ------------------ */

extern void *dlsym(void *handle, const char *symbol);
extern void *dispatch_get_global_queue(long identifier, unsigned long flags);
#define FIX_RTLD_DEFAULT ((void *)-2)
#define FIX_POSTERS_SWITCH "/private/var/mobile/Library/a8fix-posters"

static int
fix_streq(const char *a, const char *b)
{
	while (*a != 0 && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void
fix_itoa(long v, char *out)
{
	char tmp[24];
	int n = 0, neg = v < 0;
	unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;

	do {
		tmp[n++] = (char)('0' + u % 10);
		u /= 10;
	} while (u != 0 && n < 22);
	if (neg)
		*out++ = '-';
	while (n > 0)
		*out++ = tmp[--n];
	*out = 0;
}

static void
posters_register(void *context)
{
	static const char *const appex[] = {
		"/System/Library/PrivateFrameworks/WallpaperKit.framework/PlugIns/CollectionsPoster.appex",
		"/System/Library/PrivateFrameworks/GradientPoster.framework/PlugIns/GradientPosterExtension.appex",
		"/System/Library/PrivateFrameworks/PhotosUIPrivate.framework/PlugIns/PhotosPosterProvider.appex",
		"/System/Library/PrivateFrameworks/EmojiPoster.framework/PlugIns/EmojiPosterExtension.appex",
		"/System/Library/PrivateFrameworks/UnityPoster.framework/PlugIns/UnityPosterExtension.appex",
		"/System/Library/PrivateFrameworks/PridePoster.framework/PlugIns/PridePosterExtension.appex",
		"/Applications/Weather.app/PlugIns/WeatherPoster.appex",
	};
	Class string = objc_getClass("NSString"), nsurl = objc_getClass("NSURL");
	id (*make)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;
	id (*from_file)(Class, SEL, id, unsigned long, id *) =
	    (id (*)(Class, SEL, id, unsigned long, id *))objc_msgSend;
	int (*int_value)(id, SEL) = (int (*)(id, SEL))objc_msgSend;
	id (*file_url)(Class, SEL, id) = (id (*)(Class, SEL, id))objc_msgSend;
	id (*append)(id, SEL, id) = (id (*)(id, SEL, id))objc_msgSend;
	signed char (*write_file)(id, SEL, id, signed char, unsigned long, id *) =
	    (signed char (*)(id, SEL, id, signed char, unsigned long, id *))objc_msgSend;
	int (*lsregister)(id, unsigned char);
	SEL mk = sel_registerName("stringWithUTF8String:"), cat = sel_registerName("stringByAppendingString:");
	id path, text, report;
	char num[24];

	(void)context;
	if (string == 0 || nsurl == 0)
		return;
	path = make(string, mk, FIX_POSTERS_SWITCH);
	text = from_file(string, sel_registerName("stringWithContentsOfFile:encoding:error:"), path, 4, 0);
	if (text == 0 || int_value(text, sel_registerName("intValue")) != 1)
		return;
	lsregister = (int (*)(id, unsigned char))dlsym(FIX_RTLD_DEFAULT, "LSRegisterURL");
	/* v11b: iOS has no LSRegisterURL in SpringBoard; LSApplicationWorkspace's
	 * registerPlugin: (what uicache uses for plug-ins) does the same */
	Class wsc = objc_getClass("LSApplicationWorkspace");
	id ws = wsc != 0 ? ((id (*)(Class, SEL))objc_msgSend)(wsc, sel_registerName("defaultWorkspace")) : 0;
	SEL reg = sel_registerName("registerPlugin:");
	int can_reg = ws != 0 && ((signed char (*)(id, SEL, SEL))objc_msgSend)(ws,
	    sel_registerName("respondsToSelector:"), reg);
	report = make(string, mk, lsregister != 0 ? "v11b LSRegisterURL (status path):\n" :
	    can_reg ? "v11b registerPlugin: (1 = yes, path):\n" : "v11b nothing to register with\n");
	if (lsregister != 0 || can_reg) {
		for (unsigned long i = 0; i < sizeof(appex) / sizeof(appex[0]); i++) {
			id u = file_url(nsurl, sel_registerName("fileURLWithPath:"), make(string, mk, appex[i]));
			int status = u == 0 ? -1 : lsregister != 0 ? lsregister(u, 1) :
			    (int)((signed char (*)(id, SEL, id))objc_msgSend)(ws, reg, u);

			fix_itoa(status, num);
			report = append(report, cat, make(string, mk, num));
			report = append(report, cat, make(string, mk, " "));
			report = append(report, cat, make(string, mk, appex[i]));
			report = append(report, cat, make(string, mk, "\n"));
		}
	}
	write_file(report, sel_registerName("writeToFile:atomically:encoding:error:"), path, 1, 4, 0);
	fix_put("A8FIX posters: ");
	fix_put(fix_utf8(report));
}

/* ---- v13: the wallpaper (P6), part 1: what SpringBoard shows ------------
 * PosterBoard does not run (kernel 0221 refuses it while its poster
 * extensions are not in LaunchServices), so the lock and home screens are
 * black.  To find where a still picture can go: 12 s after SpringBoard
 * starts, its windows (class, level, frame, hidden) and the views of those
 * whose class names a wallpaper or a poster, one line each, for the console's
 * "tee SpringBoard".
 */
static char wall_buf[600];
static unsigned long wall_n;

static void
wall_add(const char *s)
{
	if (s == 0)
		s = "(null)";
	while (*s != 0 && wall_n + 2 < sizeof(wall_buf))
		wall_buf[wall_n++] = *s++;
	wall_buf[wall_n] = 0;
}

static void
wall_addn(double v)
{
	char t[24];

	fix_itoa((long)v, t);
	wall_add(t);
}

static void
wall_flush(void)
{
	wall_buf[wall_n++] = '\n';
	write(2, wall_buf, wall_n);
	wall_n = 0;
}

static int
fix_contains(const char *s, const char *word)
{
	for (; s != 0 && *s != 0; s++) {
		const char *a = s, *b = word;

		while (*a != 0 && *a == *b) {
			a++;
			b++;
		}
		if (*b == 0)
			return 1;
	}
	return 0;
}

static void
wall_tree(id view, int depth, unsigned long *budget)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	unsigned long (*count)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	signed char (*flag)(id, SEL) = (signed char (*)(id, SEL))objc_msgSend;
	double (*dbl)(id, SEL) = (double (*)(id, SEL))objc_msgSend;
	unsigned long n, i;
	id subs, layer;
	fix_rect r;

	if (view == 0 || depth > 9 || *budget == 0)
		return;
	(*budget)--;
	r = rect(view, sel_registerName("frame"));
	wall_add("A8FIX wall: ");
	for (int d = 0; d < depth; d++)
		wall_add(". ");
	wall_add(class_getName(object_getClass(view)));
	wall_add(" ");
	wall_addn(r.x);
	wall_add(",");
	wall_addn(r.y);
	wall_add(" ");
	wall_addn(r.w);
	wall_add("x");
	wall_addn(r.h);
	if (flag(view, sel_registerName("isHidden")))
		wall_add(" hidden");
	wall_add(" alpha%");
	wall_addn(dbl(view, sel_registerName("alpha")) * 100.0);
	layer = get(view, sel_registerName("layer"));
	if (layer != 0) {
		wall_add(" layer ");
		wall_add(class_getName(object_getClass(layer)));
		if (get(layer, sel_registerName("contents")) != 0)
			wall_add(" with contents");
	}
	wall_flush();
	subs = get(view, sel_registerName("subviews"));
	n = subs != 0 ? count(subs, sel_registerName("count")) : 0;
	for (i = 0; i < n && i < 16; i++)
		wall_tree(at(subs, sel_registerName("objectAtIndex:"), i), depth + 1, budget);
}

/* v13 part 2: a still picture behind the lock or home screen.  Switch
 * /private/var/mobile/Library/a8wall: 1 = into the lock screen's wallpaper
 * window (_SBWallpaperSecureWindow), 2 = into the window under the home
 * screen (SBWindow at level -1), 3 = both; the picture is a8wall.png beside
 * it (a PNG: ImageIO decodes it in software, unlike the HEIC wallpapers).
 * Below every other view of that window, filling it. */
#define WALL_SWITCH     "/private/var/mobile/Library/a8wall"
#define WALL_PICTURE    "/private/var/mobile/Library/a8wall.png"

static int
wall_switch(void)
{
	Class string = objc_getClass("NSString");
	id (*make)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;
	id (*from_file)(Class, SEL, id, unsigned long, id *) =
	    (id (*)(Class, SEL, id, unsigned long, id *))objc_msgSend;
	id text;

	if (string == 0)
		return 0;
	text = from_file(string, sel_registerName("stringWithContentsOfFile:encoding:error:"),
	    make(string, sel_registerName("stringWithUTF8String:"), WALL_SWITCH), 4, 0);
	return text == 0 ? 0 : ((int (*)(id, SEL))objc_msgSend)(text, sel_registerName("intValue"));
}

static void
wall_place(id window, const char *name, double level)
{
	static id picture;
	Class string = objc_getClass("NSString"), image = objc_getClass("UIImage"),
	    image_view = objc_getClass("UIImageView");
	id (*make)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	id view;

	(void)level;
	if (string == 0 || image == 0 || image_view == 0)
		return;
	if (picture == 0)
		picture = ((id (*)(Class, SEL, id))objc_msgSend)(image,
		    sel_registerName("imageWithContentsOfFile:"),
		    make(string, sel_registerName("stringWithUTF8String:"), WALL_PICTURE));
	if (picture == 0) {
		fix_put("A8FIX wall: no picture in " WALL_PICTURE "\n");
		return;
	}
	((id (*)(id, SEL))objc_msgSend)(picture, sel_registerName("retain"));
	view = ((id (*)(id, SEL))objc_msgSend)((id)image_view, sel_registerName("alloc"));
	view = ((id (*)(id, SEL, id))objc_msgSend)(view, sel_registerName("initWithImage:"), picture);
	if (view == 0)
		return;
	((void (*)(id, SEL, fix_rect))objc_msgSend)(view, sel_registerName("setFrame:"),
	    rect(window, sel_registerName("bounds")));
	((void (*)(id, SEL, long))objc_msgSend)(view, sel_registerName("setContentMode:"), 2);
	((void (*)(id, SEL, unsigned long))objc_msgSend)(view, sel_registerName("setAutoresizingMask:"), 18);
	((void (*)(id, SEL, signed char))objc_msgSend)(view, sel_registerName("setClipsToBounds:"), 1);
	((void (*)(id, SEL, signed char))objc_msgSend)(view, sel_registerName("setUserInteractionEnabled:"), 0);
	((void (*)(id, SEL, id, long))objc_msgSend)(window, sel_registerName("insertSubview:atIndex:"), view, 0);
	wall_add("A8FIX wall: picture placed in ");
	wall_add(name);
	wall_flush();
}

/* Places the picture as soon as the windows exist: 3 s after SpringBoard
 * starts, then every 2 s for a minute until it went in. */
static void
wall_try(void *context)
{
	static id placed[2];
	static int tries;
	Class uiw = objc_getClass("UIWindow");
	unsigned long (*count)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
	double (*dbl)(id, SEL) = (double (*)(id, SEL))objc_msgSend;
	SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	int which = wall_switch(), done = 0, want = 0;
	unsigned long n, i;
	id wins;

	(void)context;
	if (which == 0 || uiw == 0 || !((signed char (*)(id, SEL, SEL))objc_msgSend)((id)uiw,
	    sel_registerName("respondsToSelector:"), all))
		return;
	want = ((which & 1) != 0) + ((which & 2) != 0);
	wins = ((id (*)(id, SEL, signed char, signed char))objc_msgSend)((id)uiw, all, 1, 0);
	n = wins != 0 ? count(wins, sel_registerName("count")) : 0;
	for (i = 0; i < n; i++) {
		id w = at(wins, sel_registerName("objectAtIndex:"), i);
		const char *name = class_getName(object_getClass(w));
		double level = dbl(w, sel_registerName("windowLevel"));
		int k = fix_streq(name, "_SBWallpaperSecureWindow") ? 0 :
		    (level == -1.0 && fix_streq(name, "SBWindow")) ? 1 : -1;

		if (k < 0 || (which & (1 << k)) == 0)
			continue;
		if (placed[k] != w) {
			wall_place(w, name, level);
			placed[k] = w;
		}
		done++;
	}
	if (done < want && ++tries < 30)
		dispatch_after_f(dispatch_time(0, 2000000000LL), &_dispatch_main_q, 0, wall_try);
}

/* v13d: copies for the console, whose fget cannot name a path with a space.
 * When /private/var/mobile/Library/a8copy holds "SOURCE|DESTINATION",
 * SpringBoard copies SOURCE to DESTINATION once (NSFileManager; DESTINATION
 * replaced) and writes "done" or the error into a8copy. */
#define FIX_COPY_SWITCH "/private/var/mobile/Library/a8copy"

static void
fix_copy(void *context)
{
	Class string = objc_getClass("NSString"), fm_class = objc_getClass("NSFileManager");
	id (*make)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;
	id (*from_file)(Class, SEL, id, unsigned long, id *) =
	    (id (*)(Class, SEL, id, unsigned long, id *))objc_msgSend;
	signed char (*write_file)(id, SEL, id, signed char, unsigned long, id *) =
	    (signed char (*)(id, SEL, id, signed char, unsigned long, id *))objc_msgSend;
	SEL mk = sel_registerName("stringWithUTF8String:");
	id path, text, parts, fm, err = 0, report;
	signed char ok;

	(void)context;
	if (string == 0 || fm_class == 0)
		return;
	path = make(string, mk, FIX_COPY_SWITCH);
	text = from_file(string, sel_registerName("stringWithContentsOfFile:encoding:error:"), path, 4, 0);
	if (text == 0)
		return;
	text = ((id (*)(id, SEL, id))objc_msgSend)(text,
	    sel_registerName("stringByTrimmingCharactersInSet:"),
	    ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSCharacterSet"),
	    sel_registerName("whitespaceAndNewlineCharacterSet")));
	parts = ((id (*)(id, SEL, id))objc_msgSend)(text, sel_registerName("componentsSeparatedByString:"),
	    make(string, mk, "|"));
	if (parts == 0 || ((unsigned long (*)(id, SEL))objc_msgSend)(parts, sel_registerName("count")) != 2)
		return;
	fm = ((id (*)(Class, SEL))objc_msgSend)(fm_class, sel_registerName("defaultManager"));
	((signed char (*)(id, SEL, id, id *))objc_msgSend)(fm, sel_registerName("removeItemAtPath:error:"),
	    ((id (*)(id, SEL, unsigned long))objc_msgSend)(parts, sel_registerName("objectAtIndex:"), 1), 0);
	ok = ((signed char (*)(id, SEL, id, id, id *))objc_msgSend)(fm,
	    sel_registerName("copyItemAtPath:toPath:error:"),
	    ((id (*)(id, SEL, unsigned long))objc_msgSend)(parts, sel_registerName("objectAtIndex:"), 0),
	    ((id (*)(id, SEL, unsigned long))objc_msgSend)(parts, sel_registerName("objectAtIndex:"), 1), &err);
	report = ok ? make(string, mk, "done\n") : make(string, mk, fix_utf8(err));
	write_file(report, sel_registerName("writeToFile:atomically:encoding:error:"), path, 1, 4, 0);
	fix_put("A8FIX copy: ");
	fix_put(ok ? "done" : fix_utf8(err));
	fix_put("\n");
}

static void
wall_report(void *context)
{
	Class uiw = objc_getClass("UIWindow");
	unsigned long (*count)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
	double (*dbl)(id, SEL) = (double (*)(id, SEL))objc_msgSend;
	SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	unsigned long n, i, budget = 160;
	id wins;

	(void)context;
	if (uiw == 0 || !((signed char (*)(id, SEL, SEL))objc_msgSend)((id)uiw,
	    sel_registerName("respondsToSelector:"), all)) {
		fix_put("A8FIX wall: no window list\n");
		return;
	}
	wins = ((id (*)(id, SEL, signed char, signed char))objc_msgSend)((id)uiw, all, 1, 0);
	n = wins != 0 ? count(wins, sel_registerName("count")) : 0;
	wall_add("A8FIX wall: ");
	wall_addn((double)n);
	wall_add(" windows");
	wall_flush();
	for (i = 0; i < n; i++) {
		id w = at(wins, sel_registerName("objectAtIndex:"), i);
		const char *name = class_getName(object_getClass(w));

		double level = dbl(w, sel_registerName("windowLevel"));

		wall_add("A8FIX wall: window ");
		wall_add(name);
		wall_add(" level ");
		wall_addn(level);
		if (((signed char (*)(id, SEL))objc_msgSend)(w, sel_registerName("isHidden")))
			wall_add(" hidden");
		wall_flush();
		if (fix_contains(name, "Wallpaper") || fix_contains(name, "Poster") ||
		    (level == -1.0 && fix_streq(name, "SBWindow")))
			wall_tree(w, 1, &budget);
	}
}

/*
 * v18 (P14 AOD): the wallpaper on the dim lock screen.  The kernel's dim lock
 * screen (xnu 0232) keeps the last frame CoreAnimation drew before display
 * off, and the GL shim (glca30) holds the blank 700 ms so that frame is the
 * lock screen.  But right after SpringBoard blanks the screen it darkens the
 * cover sheet with a full-screen UIVisualEffectView (CSCoverSheetView >
 * SBFTouchPassThroughView > UIVisualEffectView, a UICABackdropLayer with a
 * colorBrightness filter: the wallpaper under it goes black, the clock above
 * it stays; seen 11-27 with layer dumps).  The wallpaper window itself stays
 * shown.  So on "com.apple.springboard.hasBlankedScreen" with state 1 that
 * view's alpha is set to a8wall-aodfx percent (default 0) 250 and 500 ms
 * later (it is put in after the notification), inside the shim's hold; state
 * 0 (wake) puts its alpha back.  a8wall-aodfx "100" = leave it alone.
 */
extern unsigned int notify_register_dispatch(const char *name, int *out_token, void *queue,
    void (^handler)(int));
extern unsigned int notify_get_state(int token, unsigned long long *state);
static int aod_token;
static id aod_fx[6];
static double aod_fx_alpha[6];
static int aod_fx_n;
static unsigned aod_gen;

static id
aod_cover_window(void)
{
	Class uiw = objc_getClass("UIWindow");
	unsigned long (*count)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
	SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	unsigned long n, i;
	id wins;

	if (uiw == 0 || !((signed char (*)(id, SEL, SEL))objc_msgSend)((id)uiw,
	    sel_registerName("respondsToSelector:"), all))
		return 0;
	wins = ((id (*)(id, SEL, signed char, signed char))objc_msgSend)((id)uiw, all, 1, 0);
	n = wins != 0 ? count(wins, sel_registerName("count")) : 0;
	for (i = 0; i < n; i++) {
		id w = at(wins, sel_registerName("objectAtIndex:"), i);

		if (fix_streq(class_getName(object_getClass(w)), "SBCoverSheetWindow"))
			return w;
	}
	return 0;
}

static int
aod_fx_known(id view)
{
	for (int k = 0; k < aod_fx_n; k++)
		if (aod_fx[k] == view)
			return 1;
	return 0;
}

/* the big, shown UIVisualEffectViews under `view`: alpha to `keep` */
static void
aod_fx_find(id view, int depth, double keep)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	unsigned long (*count)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	id (*at)(id, SEL, unsigned long) = (id (*)(id, SEL, unsigned long))objc_msgSend;
	fix_rect (*rect)(id, SEL) = (fix_rect (*)(id, SEL))objc_msgSend;
	double (*dbl)(id, SEL) = (double (*)(id, SEL))objc_msgSend;
	Class fx = objc_getClass("UIVisualEffectView");
	unsigned long n, i;
	id subs;
	fix_rect box;

	if (view == 0 || depth > 16 || fx == 0)
		return;
	if (((signed char (*)(id, SEL))objc_msgSend)(view, sel_registerName("isHidden")))
		return;
	box = rect(view, sel_registerName("bounds"));
	if (box.w >= 300 && box.h >= 600 && ((signed char (*)(id, SEL, Class))objc_msgSend)(view,
	    sel_registerName("isKindOfClass:"), fx)) {
		double alpha = dbl(view, sel_registerName("alpha"));

		if (!aod_fx_known(view) && aod_fx_n < 6) {
			aod_fx_alpha[aod_fx_n] = alpha;
			aod_fx[aod_fx_n++] = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("retain"));
			wall_add("A8FIX aod: ");
			wall_add(class_getName(object_getClass(get(view, sel_registerName("superview")))));
			wall_add(" > UIVisualEffectView alpha% ");
			wall_addn(alpha * 100.0);
			wall_add(" -> ");
			wall_addn(keep * 100.0);
			wall_add(" (wallpaper on the dim lock screen)");
			wall_flush();
		}
		((void (*)(id, SEL, double))objc_msgSend)(view, sel_registerName("setAlpha:"), keep);
		return;
	}
	subs = get(view, sel_registerName("subviews"));
	n = subs != 0 ? count(subs, sel_registerName("count")) : 0;
	for (i = 0; i < n && i < 64; i++)
		aod_fx_find(at(subs, sel_registerName("objectAtIndex:"), i), depth + 1, keep);
}

static void
aod_fx_later(void *context)
{
	id text;
	double keep;

	if ((unsigned)(unsigned long)context != aod_gen)
		return;                 /* woken meanwhile */
	text = fix_switch_text("/private/var/mobile/Library/a8wall-aodfx");
	keep = text != 0 ? ((double (*)(id, SEL))objc_msgSend)(text, sel_registerName("doubleValue")) / 100.0 : 0;
	if (keep >= 1.0)
		return;
	if (keep < 0)
		keep = 0;
	aod_fx_find(aod_cover_window(), 0, keep);
}

static void
aod_blank_changed(void)
{
	unsigned long long state = 0;

	notify_get_state(aod_token, &state);
	aod_gen++;
	if (state != 0) {
		dispatch_after_f(dispatch_time(0, 250000000LL), &_dispatch_main_q,
		    (void *)(unsigned long)aod_gen, aod_fx_later);
		dispatch_after_f(dispatch_time(0, 500000000LL), &_dispatch_main_q,
		    (void *)(unsigned long)aod_gen, aod_fx_later);
		return;
	}
	if (aod_fx_n > 0)
		fix_put("A8FIX aod: wake, lock screen effect views as they were\n");
	while (aod_fx_n > 0) {
		aod_fx_n--;
		((void (*)(id, SEL, double))objc_msgSend)(aod_fx[aod_fx_n], sel_registerName("setAlpha:"),
		    aod_fx_alpha[aod_fx_n]);
		((void (*)(id, SEL))objc_msgSend)(aod_fx[aod_fx_n], sel_registerName("release"));
	}
}

/* v18g (P14 AOD, with the GL shim's live dim lock screen, glca31 switch
 * a8gl-aodlive "1"): SpringBoard stops the lock screen clock at blank
 * (-[SBFLockScreenDateViewController setScreenOff:YES] -> _stopUpdateTimer;
 * seen 11-27: battery in the status bar went on updating, the clock did
 * not).  With the switch on, the clock is told the screen stays on, so its
 * minute timer keeps running and CoreAnimation (not blanked in that mode)
 * draws each new minute. */
static void (*date_screen_off_orig)(id, SEL, signed char);
static int date_said;

static int
aod_live_on(void)
{
	id text = fix_switch_text("/private/var/mobile/Library/a8gl-aodlive");

	return text != 0 && ((int (*)(id, SEL))objc_msgSend)(text, sel_registerName("intValue")) == 1;
}

static void
date_screen_off(id self, SEL cmd, signed char off)
{
	if (off && aod_live_on()) {
		if (date_said++ < 3)
			fix_put("A8FIX aod: lock screen clock kept running at blank (live dim, v18g)\n");
		off = 0;
	}
	date_screen_off_orig(self, cmd, off);
}

static void
aod_wall_apply(void)
{
	notify_register_dispatch("com.apple.springboard.hasBlankedScreen", &aod_token, &_dispatch_main_q,
	    ^(int t) {
		(void)t;
		aod_blank_changed();
	});
	fix_put("A8FIX aod: wallpaper kept on the dim lock screen (v18e)\n");
	{
		Class k = objc_getClass("SBFLockScreenDateViewController");
		Method m = k != 0 ? class_getInstanceMethod(k, sel_registerName("setScreenOff:")) : 0;

		if (m != 0)
			date_screen_off_orig = (void (*)(id, SEL, signed char))method_setImplementation(m,
			    (IMP)date_screen_off);
		else
			fix_put("A8FIX aod: no -[SBFLockScreenDateViewController setScreenOff:]\n");
	}
}

/*
 * v12: BlueTool mode.  With a8bt=1 (kernel 0221) the kernel puts this library
 * into Apple's BlueTool, and the library answers com.apple.BlueTool itself;
 * BlueTool's own code never runs.  The kernel has booted the Bluetooth chip
 * (bsd/dev/a5xelte_bt.c), and Apple's BlueTool would load an iPhone's
 * firmware into whatever answers.  bluetoothd sends {kMsgId:
 * RunBuiltinScript, kMsgArgs: {script: boot|init|deepsleep, btaddress}} and
 * goes on when the reply says {result: 0}; RunCommand gets {result: 0,
 * output: ""}.  As the Galaxy S20 port's bluetool_stub.c (jesvi-eulah/
 * Jesvi-iOS userland/src), with its manufacturer 15 (Broadcom): bluetoothd
 * restarts on a platform/chipset/manufacturer it does not know.  The main
 * thread stays in this constructor for good (as frida-gadget's "wait" does);
 * the listener runs on libdispatch's threads.  Its lines go to stderr: see
 * them with the console's "tee BlueTool".
 */
typedef void *xpc_object_t;
typedef const struct _xpc_type_s *xpc_type_t;
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
extern xpc_object_t xpc_connection_create_mach_service(const char *name, void *queue,
    unsigned long long flags);
extern void xpc_connection_set_event_handler(xpc_object_t connection, void (^handler)(xpc_object_t));
extern void xpc_connection_resume(xpc_object_t connection);
extern void xpc_connection_send_message(xpc_object_t connection, xpc_object_t message);
extern xpc_type_t xpc_get_type(xpc_object_t object);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t dictionary, const char *key);
extern const char *xpc_dictionary_get_string(xpc_object_t dictionary, const char *key);
extern xpc_object_t xpc_dictionary_get_remote_connection(xpc_object_t dictionary);
extern void xpc_dictionary_set_int64(xpc_object_t dictionary, const char *key, long long value);
extern void xpc_dictionary_set_string(xpc_object_t dictionary, const char *key, const char *string);
extern xpc_object_t xpc_retain(xpc_object_t object);
extern void xpc_release(xpc_object_t object);
extern int pause(void);
#define FIX_XPC_LISTENER 1ull   /* XPC_CONNECTION_MACH_SERVICE_LISTENER */
#define FIX_BROADCOM 15

static void
bluetool_answer(xpc_object_t message)
{
	const char *msgid = xpc_dictionary_get_string(message, "kMsgId");
	xpc_object_t args = xpc_dictionary_get_value(message, "kMsgArgs");
	int dict = args != 0 && xpc_get_type(args) == &_xpc_type_dictionary;
	const char *script = dict ? xpc_dictionary_get_string(args, "script") : 0;
	const char *command = dict ? xpc_dictionary_get_string(args, "command") : 0;
	xpc_object_t reply = xpc_dictionary_create_reply(message);

	fix_put("A8FIX BlueTool: ");
	fix_put(msgid != 0 ? msgid : "(no kMsgId)");
	if (script != 0) {
		fix_put(" script ");
		fix_put(script);
	}
	if (command != 0) {
		fix_put(" command ");
		fix_put(command);
	}
	fix_put(": answered with success\n");
	if (reply == 0)
		return;
	xpc_dictionary_set_int64(reply, "result", 0);
	if (msgid != 0 && fix_streq(msgid, "RunCommand")) {
		xpc_dictionary_set_string(reply, "output", "");
	} else {
		xpc_dictionary_set_int64(reply, "manufacturer", FIX_BROADCOM);
		xpc_dictionary_set_string(reply, "firmware", "MX140 (booted by the kernel)");
	}
	xpc_connection_send_message(xpc_dictionary_get_remote_connection(message), reply);
	xpc_release(reply);
}

static void
bluetool_serve(void)
{
	xpc_object_t listener = xpc_connection_create_mach_service("com.apple.BlueTool", 0,
	    FIX_XPC_LISTENER);

	if (listener == 0) {
		fix_put("A8FIX BlueTool: com.apple.BlueTool could not be listened on\n");
		return;
	}
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != &_xpc_type_connection)
			return;
		xpc_retain(peer);       /* (kept: bluetoothd connects once or twice) */
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			if (xpc_get_type(message) == &_xpc_type_dictionary)
				bluetool_answer(message);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	fix_put("A8FIX BlueTool: com.apple.BlueTool answered here (the kernel booted the chip)\n");
	for (;;)
		pause();
}

__attribute__((constructor)) static void
fix_init(void)
{
	/* v12: in BlueTool, only the stand-in (it does not return) */
	if (fix_streq(getprogname(), "BlueTool")) {
		bluetool_serve();
		return;
	}
	log_apply();
	dump_arm();
	NSSetUncaughtExceptionHandler(fix_exception);
	fix_apply();
	bl_apply();
	layout_apply();
	idle_apply();
	anim_apply();
	bd_apply();
	ci_apply();
	fix_is_springboard = fix_streq(getprogname(), "SpringBoard");
	if (fix_is_springboard) {
		curtain_apply();
		aod_wall_apply();
	}
	/* (Control Center's module is a bundle loaded later: keep looking) */
	_dyld_register_func_for_add_image(fix_image_added);
	/* what the screen is, once UIKit is up (seen with the console's tee) */
	dispatch_after_f(dispatch_time(0, 8000000000LL), &_dispatch_main_q, 0, fix_screen_report);
	/* v11b: the poster bundles to LaunchServices, once (a8fix-posters "1") */
	if (fix_streq(getprogname(), "SpringBoard"))
		dispatch_after_f(dispatch_time(0, 30000000000LL), dispatch_get_global_queue(0, 0), 0,
		    posters_register);
	/* v13: what SpringBoard shows (for "tee SpringBoard"), and the picture */
	if (fix_streq(getprogname(), "SpringBoard")) {
		dispatch_after_f(dispatch_time(0, 12000000000LL), &_dispatch_main_q, 0, wall_report);
		dispatch_after_f(dispatch_time(0, 3000000000LL), &_dispatch_main_q, 0, wall_try);
		dispatch_after_f(dispatch_time(0, 5000000000LL), dispatch_get_global_queue(0, 0), 0, fix_copy);
	}
}
