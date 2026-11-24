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
 * View dump (v6): SIGUSR1 (console "kill NAME 30") makes the process write
 * every visible window's views to stderr, one line each: depth, class, frame
 * in window points, and a label's text ("A8VIEW ..."), seen with tee. It is
 * how an off-center layout (keyboard, Control Center) is found.
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

static int layout_kb_done, layout_cc_done;

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

static void
fix_image_added(const void *mh, long slide)
{
	(void)mh;
	(void)slide;
	fix_apply();
	bl_apply();
	layout_apply();
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

__attribute__((constructor)) static void
fix_init(void)
{
	log_apply();
	dump_arm();
	NSSetUncaughtExceptionHandler(fix_exception);
	fix_apply();
	bl_apply();
	layout_apply();
	/* (Control Center's module is a bundle loaded later: keep looking) */
	_dyld_register_func_for_add_image(fix_image_added);
	/* what the screen is, once UIKit is up (seen with the console's tee) */
	dispatch_after_f(dispatch_time(0, 8000000000LL), &_dispatch_main_q, 0, fix_screen_report);
}
