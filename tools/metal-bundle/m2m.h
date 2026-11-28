/* SPDX-License-Identifier: MIT */
/*
 * m2m.h for the jackpotlte port: what RedwoodMetal's sources need of Jesvi-iOS's
 * mali2metal channel (/dev/m2m, a kernel log of theirs that this port does not have).
 *
 * A record is one write(2) of one line to stderr: "A8METAL <process>[<pid>] <sev> <cat>:
 * <text>". On this phone a process's stderr reaches the serial console (as the fix
 * library's lines do), and the console's "tee NAME" shows a named one. Severities: I
 * information, W declined or degraded, E error, F fatal. The kernel stamped the time and
 * the process on Jesvi's; here the process is in the line and the console adds the time.
 */
#ifndef JACKPOTLTE_M2M_H
#define JACKPOTLTE_M2M_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define M2M_RECORD_MAX 512

extern const char *getprogname(void);

static const char *m2m_client_name = "";

static inline void
m2m_client(const char *name)
{
	m2m_client_name = name != NULL ? name : "";
}

/* Jesvi's records how a Metal client ends; this port's kernel prints signals itself. */
static inline void
m2m_watch_crashes(void)
{
}

__attribute__((format(printf, 3, 4), unused)) static void
m2m_log(char sev, const char *cat, const char *fmt, ...)
{
	char text[M2M_RECORD_MAX], line[M2M_RECORD_MAX + 96];
	va_list ap;
	size_t n;
	int len;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	n = strlen(text);
	while (n > 0 && text[n - 1] == '\n') {
		text[--n] = '\0';
	}
	len = snprintf(line, sizeof(line), "A8METAL %s[%d]%s%s %c %s: %s\n", getprogname(),
	    (int)getpid(), m2m_client_name[0] ? " " : "", m2m_client_name, sev, cat, text);
	if (len > 0) {
		write(2, line, (size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1);
	}
}

#endif
