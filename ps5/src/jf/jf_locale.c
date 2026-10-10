#pragma GCC visibility push(hidden)
/* Locale stand-ins for json-c. json-c parses a number by switching the thread to
 * the "C" locale (uselocale + duplocale + newlocale), calling strtod, and
 * switching back. A title has one locale, and the platform layer already binds
 * newlocale/freelocale/localeconv to a fixed C locale (ps5_newlocale and
 * friends), so the switching has nothing to do. These two functions are the ones
 * the platform leaves out; they hand back the platform's own locale object so
 * json-c's later freelocale() always receives something the platform made. */
#include <locale.h>

static locale_t c_locale(void)
{
	return newlocale(LC_ALL_MASK, "C", (locale_t)0);
}

locale_t duplocale(locale_t base)
{
	(void)base;
	return c_locale();
}

locale_t uselocale(locale_t loc)
{
	/* Whatever is asked for, the thread's locale is the C one: report it, and
	 * report the same object each time so restoring "the old locale" is a no-op. */
	static locale_t current;
	if (!current) {
		current = c_locale();
	}
	(void)loc;
	return current;
}

/* libc symbols FFmpeg and its dependencies reference that the platform lacks. */
#include <errno.h>
__asm__(".text\n"
        ".globl _setjmp\n.hidden _setjmp\n.type _setjmp,@function\n_setjmp:\n\tjmp setjmp@PLT\n"
        ".globl _longjmp\n.hidden _longjmp\n.type _longjmp,@function\n_longjmp:\n\tjmp longjmp@PLT\n");
struct mmsghdr;
int sendmmsg(int fd, struct mmsghdr *v, unsigned vl, unsigned flags)
{ (void)fd; (void)v; (void)vl; (void)flags; errno = ENOSYS; return -1; }
int recvmmsg(int fd, struct mmsghdr *v, unsigned vl, unsigned flags, const void *ts)
{ (void)fd; (void)v; (void)vl; (void)flags; (void)ts; errno = ENOSYS; return -1; }
int ___mb_cur_max(void) { return 4; }

/* name lookups and tty checks FFmpeg references; the title talks to an IP address only */
const char *gai_strerror(int code) { (void)code; return "name resolution is not available"; }
void *gethostbyname(const char *name) { (void)name; return 0; }
int getnameinfo(const void *sa, unsigned salen, char *host, unsigned hostlen, char *serv, unsigned servlen, int flags)
{ (void)sa; (void)salen; (void)host; (void)hostlen; (void)serv; (void)servlen; (void)flags; return 4; }
int isatty(int fd) { (void)fd; errno = 25; return 0; }
