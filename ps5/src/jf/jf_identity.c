/* The device id a release build reports to a Jellyfin server: 32 hex digits made on the console the first
 * time they are needed and kept in /data/jellyzoom-device-id, so each install is one device of its own. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char id[40];

static int valid(const char *s)
{
	int n = 0;
	for (; s[n]; n++) {
		const char c = s[n];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return 0;
		}
	}
	return n == 32;
}

const char *jf_device_id(void)
{
	pthread_mutex_lock(&lock);
	if (!id[0]) {
		FILE *f = fopen("/data/jellyzoom-device-id", "r");
		if (f) {
			char buf[64] = { 0 };
			if (fgets(buf, sizeof(buf), f)) {
				buf[strcspn(buf, "\r\n")] = 0;
				if (valid(buf)) {
					snprintf(id, sizeof(id), "%s", buf);
				}
			}
			fclose(f);
		}
		if (!id[0]) {
			unsigned char raw[16] = { 0 };
			size_t got = 0;
			FILE *r = fopen("/dev/urandom", "rb");
			if (r) {
				got = fread(raw, 1, sizeof(raw), r);
				fclose(r);
			}
			if (got != sizeof(raw)) { /* no random source: the clock and the process are better than nothing */
				struct timespec t;
				clock_gettime(CLOCK_REALTIME, &t);
				unsigned long long x = (unsigned long long)t.tv_nsec * 6364136223846793005ULL + (unsigned long long)t.tv_sec + (unsigned long long)getpid();
				for (int i = 0; i < 16; i++) {
					x = x * 6364136223846793005ULL + 1442695040888963407ULL;
					raw[i] = (unsigned char)(x >> 56);
				}
			}
			for (int i = 0; i < 16; i++) {
				snprintf(id + i * 2, 3, "%02x", raw[i]);
			}
			FILE *w = fopen("/data/jellyzoom-device-id", "w");
			if (w) {
				fputs(id, w);
				fclose(w);
			}
		}
	}
	pthread_mutex_unlock(&lock);
	return id;
}
