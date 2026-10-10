/*
 * Jellyfin Wheel - audio output probe (Stage 16). Opens the console's compressed-audio (bitstream) ports one after
 * the other in two call orders and writes every return code to klog (AUDIO lines). It sends only silence, so nothing
 * is heard, and it always puts the HDMI output back to normal PCM afterwards. Runs only when /data/jf-audio-probe
 * exists, before the picture starts; remove that file to stop it.
 *
 * The function names, argument lists and mode numbers are those of the PS5-Audio-Passthrough-Research notes and
 * sample app (GPL-3.0), which report them working on firmware 12.70; whether they exist and behave the same on 4.03
 * is exactly what this probe finds out. The functions are ordinary imports (a weak reference makes the linker and the
 * converter drop them); the link script's null-import check refuses a build whose stubs lack one.
 *
 * Variant A: open the port, then configure the output (target 1).  Variant B: configure first, then open.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "platform.h"

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int sceAudioOutInit(void);
extern int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t freq, uint32_t format);
extern int sceAudioOutClose(int32_t handle);
extern int sceAudioOutOutput(int32_t handle, const void *samples);
extern int sceAudioOutExOpen(int32_t user, int32_t mode);
extern int sceAudioOutExClose(int32_t handle);
extern int sceAudioOutExConfigureOutput(int32_t zero, uint32_t flags, int32_t mode, int32_t target, uint64_t opt);
extern int sceAudioOutSysOpen(int32_t user, int32_t mode);
extern int sceAudioOutSysClose(int32_t handle);
extern int sceAudioOutSysConfigureOutput(int32_t type, uint32_t flags, int32_t mode, int32_t target, uint64_t opt);
extern int sceAudioOutSysGetHdmiMonitorInfo(int32_t type, void *out, uint32_t size);

#define SYSTEM_USER 0xff
#define TARGET_HDMI 1
#define MODE_DEFAULT 0xff

static int16_t silence[1024 * 2] __attribute__((aligned(64))); /* one grain of 1024 stereo frames, zeros */

static void pause_ms(int ms)
{
   struct timespec ts;
   ts.tv_sec = ms / 1000;
   ts.tv_nsec = (long)(ms % 1000) * 1000000L;
   nanosleep(&ts, NULL);
}

static void restore(const char *why)
{
   const int rc = sceAudioOutExConfigureOutput(0, 0, MODE_DEFAULT, MODE_DEFAULT, 0);
   say("AUDIO: restore PCM output (%s): ExConfigureOutput(0,0,0xFF,0xFF,0) = 0x%08x", why, (unsigned)rc);
}

/* PROBE_REV 3. One mode, one call order. Returns 1 when the port opened and the output was configured. */
static int try_mode(int mode, const char *name, int grain, char variant)
{
   int handle = -1, rc, ok = 0, pcm = -1;
   say("AUDIO: --- %s (mode %d), variant %c ---", name, mode, variant);
   if (variant == 'C') {
      /* variant C: the order of A, with an ordinary stereo port open the whole time (as the app's own audio would be) */
      pcm = sceAudioOutOpen(SYSTEM_USER, 0, 0, 256, 48000, 1);
      say("AUDIO: %s/%c a PCM port opened first: sceAudioOutOpen = 0x%08x", name, variant, (unsigned)pcm);
   }
   if (variant == 'B') {
      rc = sceAudioOutExConfigureOutput(0, 0, mode, TARGET_HDMI, 0);
      say("AUDIO: %s/%c ExConfigureOutput(0,0,%d,1,0) = 0x%08x", name, variant, mode, (unsigned)rc);
      if (rc < 0) {
         restore("configure refused");
         return 0;
      }
   }
   handle = sceAudioOutExOpen(SYSTEM_USER, mode);
   say("AUDIO: %s/%c ExOpen(0xFF,%d) = 0x%08x", name, variant, mode, (unsigned)handle);
   if (handle < 0) {
      if (pcm >= 0) {
         sceAudioOutClose(pcm);
      }
      restore("open refused");
      return 0;
   }
   if (variant == 'A' || variant == 'C') {
      rc = sceAudioOutExConfigureOutput(0, 0, mode, TARGET_HDMI, 0);
      say("AUDIO: %s/%c ExConfigureOutput(0,0,%d,1,0) = 0x%08x", name, variant, mode, (unsigned)rc);
      if (rc < 0) {
         sceAudioOutExClose(handle);
         if (pcm >= 0) {
            sceAudioOutClose(pcm);
         }
         restore("configure refused");
         return 0;
      }
   }
   ok = 1;
   if (grain > 0) {
      int i, bad = 0;
      for (i = 0; i < 4; i++) {
         rc = sceAudioOutOutput(handle, silence);
         if (rc < 0) {
            bad++;
         }
         if (i == 0) {
            say("AUDIO: %s/%c Output(silence, grain %d) = 0x%08x", name, variant, grain, (unsigned)rc);
         }
      }
      rc = sceAudioOutOutput(handle, NULL);
      say("AUDIO: %s/%c 4 grains sent, %d refused, drain = 0x%08x", name, variant, bad, (unsigned)rc);
   } else {
      say("AUDIO: %s/%c opened and configured; no silence sent (grain size not known)", name, variant);
   }
   rc = sceAudioOutExClose(handle);
   say("AUDIO: %s/%c ExClose = 0x%08x", name, variant, (unsigned)rc);
   if (pcm >= 0) {
      rc = sceAudioOutClose(pcm);
      say("AUDIO: %s/%c the PCM port closed = 0x%08x", name, variant, (unsigned)rc);
   }
   pause_ms(150);
   restore("done");
   pause_ms(150);
   return ok;
}

/* TrueHD uses the system open and its own mode table (a 768 kHz port). */
static int try_truehd(void)
{
   int rc, handle, ok = 0;
   say("AUDIO: --- TrueHD (Sys mode 5) ---");
   handle = sceAudioOutSysOpen(SYSTEM_USER, 5);
   say("AUDIO: TrueHD SysOpen(0xFF,5) = 0x%08x", (unsigned)handle);
   if (handle >= 0) {
      rc = sceAudioOutSysConfigureOutput(1, 0, 5, TARGET_HDMI, 0);
      say("AUDIO: TrueHD SysConfigureOutput(1,0,5,1,0) = 0x%08x", (unsigned)rc);
      ok = rc >= 0;
      rc = sceAudioOutSysClose(handle);
      say("AUDIO: TrueHD SysClose = 0x%08x", (unsigned)rc);
      pause_ms(150);
   }
   restore("TrueHD done");
   pause_ms(150);
   return ok;
}

static void monitor_info(void)
{
   unsigned char info[0x180];
   unsigned i;
   int rc;
   memset(info, 0, sizeof info);
   rc = sceAudioOutSysGetHdmiMonitorInfo(1, info, sizeof info);
   say("AUDIO: monitor info (what the HDMI sink says it can decode) = 0x%08x", (unsigned)rc);
   if (rc < 0) {
      return;
   }
   for (i = 0; i < sizeof info; i += 32) {
      unsigned j;
      char line[32 * 3 + 1];
      for (j = 0; j < 32; j++) {
         static const char hex[] = "0123456789abcdef";
         line[j * 3] = hex[info[i + j] >> 4];
         line[j * 3 + 1] = hex[info[i + j] & 15];
         line[j * 3 + 2] = ' ';
      }
      line[sizeof line - 1] = 0;
      say("AUDIO: monitor[%03x] %s", i, line);
   }
}

int jf_audio_probe(void)
{
   static const struct {
      int mode;
      const char *name;
      int grain;
   } modes[] = {
      { 0, "AC-3", 256 },
      { 2, "DTS", 256 },
      { 3, "E-AC-3", 1024 },
      { 10, "E-AC-3 JOC", 0 },
      { 4, "DTS-HD", 0 },
   };
   unsigned i;
   int ok = 0, total = 0;
   if (access("/data/jf-audio-probe", F_OK) != 0) {
      return -1;
   }
   say("AUDIO: probe starts (silence only; delete /data/jf-audio-probe to stop it)");
   i = (unsigned)sceAudioOutInit();
   say("AUDIO: sceAudioOutInit = 0x%08x (the app calls it later, when its own sound starts; 0x8026000e means it was already done)", i);
   monitor_info();
   for (i = 0; i < sizeof modes / sizeof modes[0]; i++) {
      char v;
      for (v = 'A'; v <= 'C'; v++) {
         ok += try_mode(modes[i].mode, modes[i].name, modes[i].grain, v);
         total++;
      }
   }
   ok += try_truehd();
   total++;
   say("AUDIO: probe ends, %d of %d tries opened and configured; output is back on PCM", ok, total);
   return ok;
}
