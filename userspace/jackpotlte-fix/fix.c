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

static void
fix_image_added(const void *mh, long slide)
{
	(void)mh;
	(void)slide;
	fix_apply();
}

__attribute__((constructor)) static void
fix_init(void)
{
	NSSetUncaughtExceptionHandler(fix_exception);
	fix_apply();
	if (!fix_done)
		_dyld_register_func_for_add_image(fix_image_added);
}
