#include <assert.h>
#include <string.h>

#include "../src/platform.c"

static struct pad_sample fake_current, fake_queue[64];
static int fake_count, fake_status, fake_overlay, fake_snapshot_error;

int scePadOpen(int32_t user, int32_t port, int32_t index, const void *params)
{
   (void)index; (void)params;
   assert(user == 0xff && port == 16);
   return 42;
}

int scePadRead(int32_t handle, void *samples, int32_t capacity)
{
   assert(handle == 42 && capacity == 64);
   if (fake_count < 0)
      return fake_count;
   memcpy(samples, fake_queue, (size_t)fake_count * sizeof(fake_queue[0]));
   return fake_count;
}

int scePadReadState(int32_t handle, void *sample)
{
   assert(handle == 42);
   if (fake_snapshot_error)
      return -1;
   memcpy(sample, &fake_current, sizeof(fake_current));
   return 0;
}

int sceSystemServiceGetStatus(void *status)
{
   ((uint8_t *)status)[4] = (uint8_t)fake_overlay;
   return fake_status;
}

static struct pad_sample
sample(uint32_t buttons, uint8_t key, uint64_t timestamp)
{
   struct pad_sample result = {0};
   result.buttons = buttons;
   result.timestamp_us = timestamp;
   ((uint8_t *)&result)[0x6c] = key;
   return result;
}

int main(void)
{
   fake_current = sample(0, 13, 1);
   remote_open();
   remote_poll();
   const struct pad_reading *readings;
   assert(remote_readings(&readings) == 1 && readings[0].buttons == 0);

   fake_queue[0] = sample(0, 0, 2);
   fake_queue[1] = sample(0, 13, 3);
   fake_queue[2] = sample(0, 0, 4);
   fake_count = 3;
   fake_current = sample(0, 0, 4);
   remote_poll();
   assert(remote_readings(&readings) == 4);
   assert(readings[0].buttons == 0 && readings[1].buttons == PAD_CROSS && readings[2].buttons == 0);

   fake_count = 0;
   fake_current = sample(PAD_LEFT | PAD_INTERCEPTED, 23, 5);
   remote_poll();
   assert(remote_readings(&readings) == 1);
   assert((readings[0].buttons & PAD_LEFT) && readings[0].l2 == 153);

   fake_overlay = 1;
   remote_poll();
   assert(remote_readings(&readings) == 1 && readings[0].buttons == 0 && readings[0].l2 == 0);
   fake_overlay = 0;
   fake_current = sample(0, 0, 6);
   remote_poll();

   fake_queue[0] = sample(0, 26, 7);
   fake_queue[1] = sample(0, 0, 8);
   fake_count = 2;
   fake_current = sample(0, 0, 8);
   remote_poll();
   assert(remote_media_pressed() == REMOTE_PAUSE);
   remote_poll();
   assert(remote_media_pressed() == 0);

   fake_count = 0;
   fake_current = sample(0, 22, 9);
   remote_poll();
   assert(remote_media_pressed() == REMOTE_PLAY);
   remote_poll();
   assert(remote_media_pressed() == 0);
   fake_status = -1;
   remote_poll();
   assert(remote_readings(&readings) == 1 && readings[0].buttons == 0);
   fake_status = 0;
   fake_count = -1;
   fake_snapshot_error = 1;
   remote_poll();
   assert(remote_readings(&readings) == 1 && readings[0].buttons == 0);
   return 0;
}
