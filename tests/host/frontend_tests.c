/* PC tests for frontend parts that don't need the emulator running:
 * turning frames for the portrait LCD (nspire_turn.c), key bindings
 * (nspire_keys.c), the settings files (nspire_config.c), screen brightness
 * (nspire_brightness.c) and the file list's order (nspire_game_list.c).
 *
 *   frontend_tests DIR   files are written in DIR */

#include "common.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_gui_video.h"
#include "nspire_keys.h"
#include "input.h"
#include "nspire_brightness.h"
#include "nspire_game_list.h"
#include "nspire_clock.h"
#include "nspire_paths.h"
#include "nspire_selfstore.h"
#include "gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern char unit_documents_dir[600];

static int failures, checks;
static char dir[512];

#define CHECK(cond, ...)                                      \
  do                                                          \
  {                                                           \
    checks++;                                                 \
    if (!(cond))                                              \
    {                                                         \
      failures++;                                             \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
      printf(__VA_ARGS__);                                    \
      printf("\n");                                           \
    }                                                         \
  } while (0)

/* host_stubs.c */
extern uint32_t unit_backlight;
extern long unit_backlight_writes;
extern char unit_osd_message[64];
extern unsigned unit_touchpad_scan_calls;
extern unsigned unit_touchpad_info_calls;
extern unsigned unit_touchpad_availability_checks;

#define PORTRAIT (240 * 320)
#define SENTINEL 0xBEEF

static u16 src[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];
static u16 got[PORTRAIT], want[PORTRAIT];

static void fill_random(u16 *p, u32 n, u32 seed)
{
  while (n--)
  {
    seed = seed * 1103515245u + 12345u;
    *p++ = (u16)(seed >> 8);
  }
}

static void fill(u16 *p, u32 n, u16 value)
{
  while (n--)
    *p++ = value;
}

/* Landscape (x, y) of the 320x240 picture in a portrait buffer. */
#define AT(buf, x, y) (buf)[(x) * 240 + 239 - (y)]

/* nspire_turn_raw as it was before the strip version (2026-10-05), which
 * matched gpSP's upscale_aspect_raw pixel for pixel. */
static const u16 old_zero_row[GBA_SCREEN_WIDTH];
static const u16 *old_raw_row(const u16 *s, u32 src_pitch, u32 y)
{
  u32 r, k, phase, src_y;
  if (y < 13 || y > 226)
    return old_zero_row;
  r = y - 13;
  k = r >> 2;
  phase = r & 3;
  src_y = 3 * k + (phase == 3 ? 2 : phase);
  if (src_y > GBA_SCREEN_HEIGHT - 1)
    src_y = GBA_SCREEN_HEIGHT - 1;
  return s + src_y * src_pitch;
}

static void old_turn_raw(u16 *dst, const u16 *s, u32 src_pitch)
{
  u32 y;
  for (y = 12; y < 228; y += 2)
  {
    const u32 *a = (const u32 *)old_raw_row(s, src_pitch, y);
    const u32 *b = (const u32 *)old_raw_row(s, src_pitch, y + 1);
    u32 *d = (u32 *)dst + (238 - y) / 2;
    u32 g;
    for (g = 0; g < GBA_SCREEN_WIDTH / 6; g++)
    {
      u32 a0 = a[0], a1 = a[1], a2 = a[2];
      u32 b0 = b[0], b1 = b[1], b2 = b[2];
      u32 w1 = (b0 >> 16) | (a0 & 0xFFFF0000u);
      u32 w4 = (b2 & 0xFFFFu) | (a2 << 16);
      d[0 * 120] = (b0 & 0xFFFFu) | (a0 << 16);
      d[1 * 120] = w1;
      d[2 * 120] = w1;
      d[3 * 120] = (b1 & 0xFFFFu) | (a1 << 16);
      d[4 * 120] = (b1 >> 16) | (a1 & 0xFFFF0000u);
      d[5 * 120] = w4;
      d[6 * 120] = w4;
      d[7 * 120] = (b2 >> 16) | (a2 & 0xFFFF0000u);
      a += 3;
      b += 3;
      d += 8 * 120;
    }
  }
}

static u32 count_differences(const u16 *a, const u16 *b, u32 n)
{
  u32 i, count = 0;
  for (i = 0; i < n; i++)
    count += a[i] != b[i];
  return count;
}

static void test_turn_raw(void)
{
  u32 x, y, pitch;

  for (pitch = GBA_SCREEN_WIDTH; pitch <= GBA_SCREEN_WIDTH + 16; pitch += 16)
  {
    static u16 frame[(GBA_SCREEN_WIDTH + 16) * GBA_SCREEN_HEIGHT];
    fill_random(frame, sizeof(frame) / 2, pitch);
    fill(got, PORTRAIT, SENTINEL);
    fill(want, PORTRAIT, SENTINEL);
    nspire_turn_raw(got, frame, pitch);
    old_turn_raw(want, frame, pitch);
    CHECK(count_differences(got, want, PORTRAIT) == 0, "raw (pitch %u): %u pixels differ from the old turn",
          pitch, count_differences(got, want, PORTRAIT));

    /* And from first principles: 4 columns from 3, 4 lines from 3. */
    fill(want, PORTRAIT, SENTINEL);
    for (y = 12; y < 228; y++)
      for (x = 0; x < 320; x++)
      {
        s32 r = (s32)y - 13;
        static const u32 col[4] = { 0, 1, 1, 2 }, row[4] = { 0, 1, 2, 2 };
        u16 p = 0;
        if (r >= 0 && r < 214)
        {
          u32 src_y = 3 * (r >> 2) + row[r & 3];
          if (src_y > 159)
            src_y = 159;   /* 160 rows make 213 1/3 */
          p = frame[src_y * pitch + 3 * (x >> 2) + col[x & 3]];
        }
        AT(want, x, y) = p;
      }
    CHECK(count_differences(got, want, PORTRAIT) == 0, "raw (pitch %u): %u pixels wrong", pitch,
          count_differences(got, want, PORTRAIT));
  }
}

static void test_turn_1x(void)
{
  u32 x, y;
  fill_random(src, sizeof(src) / 2, 99);
  fill(got, PORTRAIT, SENTINEL);
  fill(want, PORTRAIT, SENTINEL);
  nspire_turn_1x(got, src, GBA_SCREEN_WIDTH);
  for (y = 0; y < 160; y++)
    for (x = 0; x < 240; x++)
      AT(want, 40 + x, 40 + y) = src[y * GBA_SCREEN_WIDTH + x];
  CHECK(count_differences(got, want, PORTRAIT) == 0, "1x: %u pixels wrong",
        count_differences(got, want, PORTRAIT));
}

static u16 to565(u16 p)
{
  u32 r = p & 0x1F, g = (p >> 5) & 0x1F, b = (p >> 10) & 0x1F;
  return (u16)((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
}

static void test_turn_rects(void)
{
  static u16 landscape[320 * 240];
  static const u32 rects[][4] =
  {
    { 0, 0, 320, 240 }, { 0, 13, 320, 213 }, { 2, 2, 128, 10 }, { 0, 228, 320, 12 },
    { 5, 7, 33, 17 }, { 300, 1, 20, 3 }, { 1, 0, 1, 240 }, { 17, 9, 64, 1 },
  };
  u32 i, x, y;

  fill_random(landscape, 320 * 240, 5);
  for (i = 0; i < sizeof(rects) / sizeof(rects[0]); i++)
  {
    u32 x0 = rects[i][0], y0 = rects[i][1], w = rects[i][2], h = rects[i][3];
    const u16 *s = landscape + y0 * 320 + x0;

    fill(got, PORTRAIT, SENTINEL);
    fill(want, PORTRAIT, SENTINEL);
    nspire_turn_rect_565(got, s, 320, x0, y0, w, h);
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
        AT(want, x0 + x, y0 + y) = s[y * 320 + x];
    CHECK(count_differences(got, want, PORTRAIT) == 0, "rect 565 %u,%u %ux%u: %u wrong", x0, y0, w, h,
          count_differences(got, want, PORTRAIT));

    fill(got, PORTRAIT, SENTINEL);
    fill(want, PORTRAIT, SENTINEL);
    nspire_turn_rect_555(got, s, 320, x0, y0, w, h);
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
        AT(want, x0 + x, y0 + y) = to565(s[y * 320 + x]);
    CHECK(count_differences(got, want, PORTRAIT) == 0, "rect 555 %u,%u %ux%u: %u wrong", x0, y0, w, h,
          count_differences(got, want, PORTRAIT));
  }
}

extern void unit_set_key(unsigned key, int down);
extern void unit_set_touchpad(int contact, unsigned x, unsigned y);
u32 nspire_input_frame(void);
void nspire_input_release_all(void);

static void test_swipe_input_pipeline(void)
{
  u32 p1;

  keys_set_defaults(key_bindings);
  nspire_touchpad_swipe = 1;
  nspire_touchpad_deadzone = 2;      /* 8% */
  nspire_touchpad_sensitivity = 2;  /* 100% */
  unit_set_key(KEY_SHIFT, 0);
  unit_set_touchpad(0, 100, 100);
  nspire_input_release_all();
  keys_poll();
  CHECK(unit_touchpad_availability_checks == 1 && unit_touchpad_info_calls == 1,
        "touchpad availability and dimensions are checked once at initialization");

  /* Touch establishes a relative origin; it must not trigger a direction. */
  unit_set_touchpad(1, 100, 100);
  keys_poll();
  CHECK(!key_down(KEY_SWIPE_RIGHT) && !key_down(KEY_SWIPE_UP),
        "touching the pad without moving does not trigger swipe inputs");

  /* A right/up diagonal on the 4:3 pad activates separate X and Y components. */
  unit_set_touchpad(1, 160, 130);
  keys_poll();
  CHECK(key_down(KEY_SWIPE_RIGHT) && key_down(KEY_SWIPE_UP),
        "diagonal movement activates its independent X and Y swipe inputs");

  /* A swipe may bind to more than one GBA button. A separate combination
   * binds the same swipe plus SHIFT to A. */
  keys_set_defaults(key_bindings);
  key_bindings[ACTION_A][0] = binding_unpack(KEY_SWIPE_RIGHT | (KEY_SHIFT << 8));
  key_bindings[ACTION_B][0] = binding_unpack(KEY_SWIPE_RIGHT);
  key_bindings[ACTION_L][0] = binding_unpack(KEY_SWIPE_RIGHT);
  unit_set_touchpad(0, 160, 130);
  unit_set_key(KEY_SHIFT, 0);
  keys_poll();
  unit_set_touchpad(1, 100, 100);
  keys_poll();
  unit_set_touchpad(1, 160, 100);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK(!(p1 & BUTTON_B) && !(p1 & BUTTON_L) && (p1 & BUTTON_A),
        "one swipe drives two mapped GBA buttons while the key combo stays inactive");

  unit_set_key(KEY_SHIFT, 1);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK(!(p1 & BUTTON_A) && !(p1 & BUTTON_B) && !(p1 & BUTTON_L),
        "a swipe plus a physical calculator key activates the combo alongside other mapped buttons");

  unit_set_key(KEY_SHIFT, 0);
  unit_set_touchpad(0, 160, 100);
  keys_poll();
  nspire_input_release_all();
  keys_set_defaults(key_bindings);
  unit_set_touchpad(0, 100, 100);
}

static void test_glide_input_pipeline(void)
{
  u32 p1, before_scans;
  unsigned i, right_pulses, up_pulses;
  const u32 glide_combo = BUTTON_A | BUTTON_B;

  nspire_touchpad_swipe = 0;
  nspire_touchpad_glide_enabled = 0;
  nspire_touchpad_glide_aspect_mode = GLIDE_ASPECT_4_3;
  nspire_touchpad_glide_deadzone = 8;
  nspire_touchpad_glide_invert_x = 0;
  nspire_touchpad_glide_invert_y = 0;
  memset(nspire_touchpad_glide_masks, 0, sizeof(nspire_touchpad_glide_masks));
  nspire_touchpad_glide_masks[GLIDE_DIR_RIGHT] = (unsigned short)glide_combo;
  nspire_touchpad_glide_masks[GLIDE_DIR_UP] = BUTTON_L;
  nspire_touchpad_glide_masks[GLIDE_DIR_LEFT] = BUTTON_SELECT;
  nspire_touchpad_glide_masks[GLIDE_DIR_DOWN] = BUTTON_R;
  keys_set_defaults(key_bindings);
  unit_set_key(KEY_SHIFT, 0);
  unit_set_touchpad(0, 100, 100);
  nspire_input_release_all();

  /* Off bypasses the new mapping. */
  unit_set_touchpad(1, 100, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 160, 100);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & glide_combo) == glide_combo, "glide off does not press its mapped buttons");

  /* The new contact establishes an anchor. A diagonal delta independently
   * triggers right/up, and the one frame poll is shared. */
  nspire_touchpad_glide_enabled = 1;
  unit_set_touchpad(0, 160, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 100, 100);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & (glide_combo | BUTTON_L)) == (glide_combo | BUTTON_L),
        "first contact only establishes a baseline");
  unit_set_touchpad(1, 130, 130);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & (glide_combo | BUTTON_L)) == 0,
        "first contact emits nothing and diagonal movement activates both axis maps");

  /* A pause keeps the diagonal latched and emits its proportional pulses. */
  before_scans = unit_touchpad_scan_calls;
  unit_set_touchpad(1, 130, 130);
  nspire_input_frame();
  CHECK(unit_touchpad_scan_calls == before_scans + 1,
        "glide reuses the existing single touchpad poll for a game frame");
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & (glide_combo | BUTTON_L)) == 0, "latched diagonal keeps pulsing during a pause");
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & (glide_combo | BUTTON_L)) == 0, "latched components retain their pulse remainder");
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & (glide_combo | BUTTON_L)) == (glide_combo | BUTTON_L),
        "fractional angle weighting leaves occasional digital release frames");
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & (glide_combo | BUTTON_L)) == 0, "latched glide resumes its proportional pulse cycle");

  /* Sub-deadzone steps accumulate from the unchanged anchor. */
  unit_set_touchpad(0, 100, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 100, 100);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & (glide_combo | BUTTON_L)) == (glide_combo | BUTTON_L),
        "a new touch at a distant coordinate does not create a jump");
  unit_set_touchpad(1, 110, 100);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & glide_combo) == glide_combo, "a short step below the threshold is ignored");
  unit_set_touchpad(1, 120, 100);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & glide_combo) == 0,
        "sub-deadzone travel accumulates until the anchor delta crosses the threshold");

  /* Full diagonal samples distinguish physical 4:3 weighting from equal
   * percentage-of-span weighting using a 30 by 30 raw coordinate change. */
  nspire_touchpad_glide_deadzone = 1;
  nspire_touchpad_glide_aspect_mode = GLIDE_ASPECT_4_3;
  unit_set_touchpad(0, 100, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 100, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 130, 130);
  nspire_input_frame();
  right_pulses = up_pulses = 0;
  for (i = 0; i < 20; i++)
  {
    p1 = read_ioreg(REG_P1);
    if (!(p1 & glide_combo)) right_pulses++;
    if (!(p1 & BUTTON_L)) up_pulses++;
    if (i != 19) nspire_input_frame();
  }
  CHECK(right_pulses == up_pulses, "4:3 weighting gives a physical 45-degree diagonal equal pulse rates");

  nspire_touchpad_glide_aspect_mode = GLIDE_ASPECT_EQUAL;
  unit_set_touchpad(0, 100, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 100, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 130, 130);
  nspire_input_frame();
  right_pulses = up_pulses = 0;
  for (i = 0; i < 20; i++)
  {
    p1 = read_ioreg(REG_P1);
    if (!(p1 & glide_combo)) right_pulses++;
    if (!(p1 & BUTTON_L)) up_pulses++;
    if (i != 19) nspire_input_frame();
  }
  CHECK(up_pulses > right_pulses, "equal weighting follows the touchpad's unequal raw axis fractions");

  /* Inversion selects opposite direction maps. */
  unit_set_touchpad(0, 200, 150);
  nspire_input_frame();
  nspire_touchpad_glide_invert_x = 1;
  nspire_touchpad_glide_invert_y = 1;
  unit_set_touchpad(1, 200, 150);
  nspire_input_frame();
  unit_set_touchpad(1, 230, 180);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & (BUTTON_SELECT | BUTTON_R)) == 0,
        "invert X and Y reverse both components into left/down maps");

  /* Lift-off clears held mappings immediately, while physical keys merge by
   * OR with glide buttons in the same frame. */
  unit_set_touchpad(0, 230, 180);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & (glide_combo | BUTTON_L | BUTTON_SELECT | BUTTON_R)) ==
        (glide_combo | BUTTON_L | BUTTON_SELECT | BUTTON_R),
        "contact lift clears active glide outputs immediately");

  nspire_touchpad_glide_invert_x = 0;
  nspire_touchpad_glide_invert_y = 0;
  nspire_touchpad_glide_aspect_mode = GLIDE_ASPECT_4_3;
  unit_set_touchpad(1, 100, 100);
  nspire_input_frame();
  unit_set_key(KEY_SHIFT, 1); /* physical Start */
  unit_set_touchpad(1, 130, 100);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK(!(p1 & (glide_combo | BUTTON_START)), "physical and glide inputs are OR-merged");

  unit_set_key(KEY_SHIFT, 0);
  unit_set_touchpad(0, 130, 100);
  nspire_input_frame();
  nspire_input_release_all();
  nspire_touchpad_glide_enabled = 0;
  nspire_touchpad_glide_smoothing = 0;
  nspire_touchpad_glide_smooth_strength = GLIDE_SMOOTH_MEDIUM;
  nspire_input_separation_mode = INPUT_SEPARATION_OFF;
  nspire_input_slice_frames = 2;
  nspire_touchpad_glide_invert_x = 0;
  nspire_touchpad_glide_invert_y = 0;
  memset(nspire_touchpad_glide_masks, 0, sizeof(nspire_touchpad_glide_masks));
  keys_set_defaults(key_bindings);
}

static void test_glide_smoothing(void)
{
  u32 p1;
  unsigned i;

  serial_mode = SERIAL_MODE_DISABLED;
  nspire_touchpad_swipe = 0;
  nspire_touchpad_glide_enabled = 1;
  nspire_touchpad_glide_aspect_mode = GLIDE_ASPECT_4_3;
  nspire_touchpad_glide_deadzone = 8;
  nspire_touchpad_glide_invert_x = nspire_touchpad_glide_invert_y = 0;
  nspire_touchpad_glide_smoothing = 1;
  nspire_touchpad_glide_smooth_strength = GLIDE_SMOOTH_MEDIUM;
  nspire_input_separation_mode = INPUT_SEPARATION_OFF;
  nspire_input_slice_frames = 2;
  memset(nspire_touchpad_glide_masks, 0, sizeof(nspire_touchpad_glide_masks));
  nspire_touchpad_glide_masks[GLIDE_DIR_RIGHT] = BUTTON_RIGHT;
  nspire_touchpad_glide_masks[GLIDE_DIR_UP] = BUTTON_UP;
  nspire_touchpad_glide_masks[GLIDE_DIR_LEFT] = BUTTON_LEFT;
  keys_set_defaults(key_bindings);
  unit_set_key(KEY_8, 0);
  unit_set_touchpad(0, 100, 100);
  nspire_input_release_all();
  nspire_input_frame();
  unit_set_touchpad(1, 100, 100);
  nspire_input_frame();

  unit_set_touchpad(1, 130, 100);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK(!(p1 & BUTTON_RIGHT), "smoothed glide starts with its cardinal direction");

  unit_set_touchpad(1, 130, 130);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK(!(p1 & BUTTON_RIGHT) && !(p1 & BUTTON_UP),
        "EMA keeps the previous axis active while easing toward a new angle");

  unit_set_touchpad(0, 200, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 200, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 170, 100);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK(!(p1 & BUTTON_LEFT), "fixed-point smoothing preserves negative glide directions");

  /* Hysteresis requires a 0.20 component to enter but keeps an active axis
   * until it falls below 0.10. Small deltas keep the test near both limits. */
  nspire_touchpad_glide_smooth_strength = GLIDE_SMOOTH_LOW;
  nspire_touchpad_glide_deadzone = 1;
  unit_set_touchpad(0, 100, 30);
  nspire_input_frame();
  unit_set_touchpad(1, 100, 30);
  nspire_input_frame();
  unit_set_touchpad(1, 101, 37); /* x projection about 0.14: below entry */
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK(p1 & BUTTON_RIGHT, "hysteresis suppresses a grazing inactive axis");
  {
    int entered = 0;
    for (i = 0; i < 4; i++)
    {
      unit_set_touchpad(1, (unsigned short)(103 + i * 2),
                        (unsigned short)(46 + i * 9));
      nspire_input_frame();
      p1 = read_ioreg(REG_P1);
      if (!(p1 & BUTTON_RIGHT))
        entered = 1;
    }
    CHECK(entered, "the horizontal axis enters after its component reaches 0.20");
  }
  unit_set_touchpad(1, 110, 80); /* settle below entry but above the 0.10 exit */
  nspire_input_frame();
  {
    unsigned right_pulses = 0;
    for (i = 0; i < 20; i++)
    {
      p1 = read_ioreg(REG_P1);
      if (!(p1 & BUTTON_RIGHT))
        right_pulses++;
      if (i != 19)
        nspire_input_frame();
    }
    CHECK(right_pulses > 0, "the active axis remains pulsed below its entry threshold");
  }
  for (i = 0; i < 4; i++)
  {
    unit_set_touchpad(1, 110, (unsigned short)(100 + i * 20));
    nspire_input_frame();
  }
  p1 = read_ioreg(REG_P1);
  CHECK(p1 & BUTTON_RIGHT, "an axis exits only after dropping below its lower threshold");

  /* High strength uses a three-frame grouped pulse; a pure cardinal glide
   * remains continuously held through the pulse cycle. */
  nspire_touchpad_glide_smooth_strength = GLIDE_SMOOTH_HIGH;
  unit_set_touchpad(0, 100, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 100, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 130, 100);
  nspire_input_frame();
  for (i = 0; i < 6; i++)
  {
    p1 = read_ioreg(REG_P1);
    CHECK(!(p1 & BUTTON_RIGHT), "high-strength cardinal output has no frame gaps");
    if (i != 5)
      nspire_input_frame();
  }

  /* Turning smoothing off restores the raw cardinal response. */
  nspire_touchpad_glide_smoothing = 0;
  unit_set_touchpad(0, 100, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 100, 100);
  nspire_input_frame();
  unit_set_touchpad(1, 100, 130);
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & BUTTON_RIGHT) && !(p1 & BUTTON_UP),
        "smoothing off bypasses the EMA and hysteresis state");

  unit_set_touchpad(0, 100, 130);
  nspire_input_frame();
  nspire_input_release_all();
  nspire_touchpad_glide_enabled = 0;
  nspire_touchpad_glide_smoothing = 0;
  nspire_touchpad_glide_smooth_strength = GLIDE_SMOOTH_MEDIUM;
  memset(nspire_touchpad_glide_masks, 0, sizeof(nspire_touchpad_glide_masks));
  keys_set_defaults(key_bindings);
}

static void test_input_separation_scheduler(void)
{
  u32 p1;
  const u32 chord = BUTTON_UP | BUTTON_L | BUTTON_R;

  serial_mode = SERIAL_MODE_DISABLED;
  nspire_touchpad_swipe = 0;
  nspire_touchpad_glide_enabled = 1;
  nspire_touchpad_glide_aspect_mode = GLIDE_ASPECT_4_3;
  nspire_touchpad_glide_deadzone = 8;
  nspire_touchpad_glide_invert_x = nspire_touchpad_glide_invert_y = 0;
  nspire_touchpad_glide_smoothing = 0;
  nspire_touchpad_glide_smooth_strength = GLIDE_SMOOTH_MEDIUM;
  nspire_input_separation_mode = INPUT_SEPARATION_SMART;
  nspire_input_slice_frames = 2;
  memset(nspire_touchpad_glide_masks, 0, sizeof(nspire_touchpad_glide_masks));
  nspire_touchpad_glide_masks[GLIDE_DIR_UP] = (unsigned short)chord;
  keys_set_defaults(key_bindings);
  unit_set_key(KEY_8, 0);
  unit_set_key(KEY_DEL, 0);
  unit_set_touchpad(0, 100, 100);
  nspire_input_release_all();
  nspire_input_frame();

  unit_set_touchpad(1, 100, 100);
  unit_set_key(KEY_8, 1); /* D-pad up */
  nspire_input_frame();
  unit_set_touchpad(1, 100, 130); /* Glide Up -> Up + L + R */
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & chord) == (BUTTON_L | BUTTON_R),
        "Smart mode keeps the single-button base separate from a chord");
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & chord) == (BUTTON_L | BUTTON_R), "Smart base layer uses the configured two-frame slice");
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & chord) == 0, "Smart chord layer outputs its exact mask without leaking into the base");

  /* Separate All gives two single-button keys their own round-robin layers. */
  unit_set_touchpad(0, 100, 130);
  unit_set_key(KEY_8, 1);
  unit_set_key(KEY_DEL, 1);
  nspire_input_separation_mode = INPUT_SEPARATION_ALL;
  nspire_input_release_all();
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK(!(p1 & BUTTON_UP) && (p1 & BUTTON_A), "Separate All begins with the first key source");
  nspire_input_frame();
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK((p1 & BUTTON_UP) && !(p1 & BUTTON_A), "Separate All advances after two frames");

  /* Smart combines independent single buttons in its base layer. */
  nspire_input_separation_mode = INPUT_SEPARATION_SMART;
  nspire_input_release_all();
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK(!(p1 & BUTTON_UP) && !(p1 & BUTTON_A), "Smart mode keeps ordinary single buttons simultaneous");

  /* Off stays zero-latency OR mode. */
  nspire_input_separation_mode = INPUT_SEPARATION_OFF;
  nspire_input_frame();
  p1 = read_ioreg(REG_P1);
  CHECK(!(p1 & BUTTON_UP) && !(p1 & BUTTON_A), "separation off retains immediate OR behavior");

  unit_set_key(KEY_8, 0);
  unit_set_key(KEY_DEL, 0);
  unit_set_touchpad(0, 100, 130);
  nspire_input_frame();
  nspire_input_release_all();
  nspire_touchpad_glide_enabled = 0;
  nspire_input_separation_mode = INPUT_SEPARATION_OFF;
  nspire_input_slice_frames = 2;
  memset(nspire_touchpad_glide_masks, 0, sizeof(nspire_touchpad_glide_masks));
  keys_set_defaults(key_bindings);
}

static void test_keys(void)
{
  char text[32];
  key_binding b;
  int i, slot;

  keys_set_defaults(key_bindings);
  CHECK(key_bindings[ACTION_A][0].key == KEY_DEL && key_bindings[ACTION_MENU][0].key == KEY_MENU,
        "defaults");
  CHECK(key_bindings[ACTION_RESTART][0].key == 0, "new ones unbound");
  CHECK(key_bindings[ACTION_UP][1].key == KEY_SWIPE_UP &&
        key_bindings[ACTION_DOWN][1].key == KEY_SWIPE_DOWN &&
        key_bindings[ACTION_LEFT][1].key == KEY_SWIPE_LEFT &&
        key_bindings[ACTION_RIGHT][1].key == KEY_SWIPE_RIGHT,
        "swipe components are the secondary D-pad defaults");

  CHECK(!strcmp(key_name(KEY_SWIPE_UP), "SW-UP") &&
        !strcmp(key_name(KEY_SWIPE_UP_RIGHT), "SW-UPRIGHT") &&
        !key_is_bindable(KEY_SWIPE_UP_RIGHT),
        "diagonal swipe names remain recognized but only orthogonal swipes bind");
  CHECK(touchpad_swipe_direction_mask(0, 0, 320, 240, 10, 100) == 0,
        "touching without movement stays in the dead zone");
  CHECK(touchpad_swipe_direction_mask(64, 0, 320, 240, 10, 100) == SWIPE_DIR_RIGHT,
        "right swipe is relative to its start point");
  CHECK(touchpad_swipe_direction_mask(0, 48, 320, 240, 10, 100) == SWIPE_DIR_UP,
        "up swipe follows the calculator's Y-coordinate convention");
  CHECK(touchpad_swipe_direction_mask(64, 48, 320, 240, 10, 100) ==
        (SWIPE_DIR_UP | SWIPE_DIR_RIGHT), "diagonal keeps independent X/Y components");
  CHECK(touchpad_swipe_direction_mask(32, 24, 320, 240, 10, 100) ==
        (SWIPE_DIR_UP | SWIPE_DIR_RIGHT), "4:3 normalized components have equal weights");
  CHECK(touchpad_swipe_direction_mask(32, 24, 320, 240, 10, 50) == 0,
        "lower sensitivity suppresses weaker movement");
  CHECK(touchpad_swipe_direction_mask(32, 24, 320, 240, 5, 50) ==
        (SWIPE_DIR_UP | SWIPE_DIR_RIGHT), "smaller dead zone picks up the same movement");
  CHECK(touchpad_swipe_direction_mask(-64, -48, 320, 240, 10, 100) ==
        (SWIPE_DIR_DOWN | SWIPE_DIR_LEFT), "down-left swipe direction");
  CHECK(touchpad_swipe_direction_mask(20, 0, 0, 240, 10, 100) == 0,
        "zero width is safe");

  b = binding_unpack(KEY_SWIPE_RIGHT | (KEY_SHIFT << 8));
  CHECK(b.key == KEY_SWIPE_RIGHT && b.with == KEY_SHIFT,
        "an orthogonal swipe can combine with a physical key");
  CHECK(binding_pack(b) == (KEY_SWIPE_RIGHT | (KEY_SHIFT << 8)),
        "swipe plus physical key packs and unpacks");
  key_bindings[ACTION_A][0] = binding_unpack(KEY_SWIPE_RIGHT);
  key_bindings[ACTION_B][0] = binding_unpack(KEY_SWIPE_RIGHT);
  CHECK(key_bindings[ACTION_A][0].key == key_bindings[ACTION_B][0].key,
        "one orthogonal swipe can be assigned to multiple GBA actions");
  keys_set_defaults(key_bindings);

  b = binding_unpack(KEY_S | (KEY_SHIFT << 8));
  CHECK(b.key == KEY_S && b.with == KEY_SHIFT, "unpack combo");
  CHECK(binding_pack(b) == (KEY_S | (KEY_SHIFT << 8)), "pack combo");
  CHECK(!strcmp(binding_name(b, text, sizeof(text)), "shift+S"), "name %s", text);
  b = binding_unpack(KEY_UP);
  CHECK(b.key == 0, "arrows can't be bound");
  b = binding_unpack(KEY_CLICK);
  CHECK(b.key == 0, "click can't be bound");
  b = binding_unpack(3);   /* no key 3 */
  CHECK(b.key == 0, "nonexistent key");
  b = binding_unpack(KEY_S | (KEY_S << 8));
  CHECK(b.key == KEY_S && b.with == 0, "a key with itself is just the key");
  b = binding_unpack(KEY_S | (KEY_UP << 8));
  CHECK(b.key == KEY_S && b.with == 0, "can't hold an arrow");
  b.key = 0;
  b.with = 0;
  CHECK(!strcmp(binding_name(b, text, sizeof(text)), "none"), "none: %s", text);

  for (i = 0; i < NUM_ACTIONS; i++)
    for (slot = 0; slot < KEY_SLOTS; slot++)
    {
      key_binding k = key_bindings[i][slot];
      CHECK(!k.key || key_is_bindable(k.key), "default %d/%d bindable", i, slot);
    }
}

static long file_size(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* Settings and keys go through gpsp_qol.cfg.tns (in the saves folder) and
 * a game's own file. */
static void test_config(void)
{
  char path[600], old_path[600], rom[600], game_cfg[600];
  FILE *f;
  u32 old[37];

  snprintf(main_path, sizeof(main_path), "%s", dir);
  nspire_save_file(path, sizeof(path), "gpsp_qol.cfg.tns");
  snprintf(old_path, sizeof(old_path), "%s/gpsp.cfg.tns", dir);
  remove(path);
  remove(old_path);

  /* Values out of the way of the defaults. */
  keys_set_defaults(key_bindings);
  key_bindings[ACTION_A][1] = binding_unpack(KEY_7);
  key_bindings[ACTION_SAVE_STATE][1] = binding_unpack(KEY_S | (KEY_SHIFT << 8));
  key_bindings[ACTION_RESTART][0] = binding_unpack(KEY_9);
  screen_scale = fullscreen;
  current_frameskip_type = manual_frameskip;
  frameskip_value = 3;
  nspire_fps_overlay = 2;
  nspire_auto_increment = 0;
  nspire_save_on_exit = 0;
  nspire_load_on_start = 1;
  nspire_cpu_speed = 2;
  nspire_rom_buffer_size_choice = 12;
  nspire_touchpad_swipe = 0;
  nspire_touchpad_deadzone = 5;
  nspire_touchpad_sensitivity = 6;
  nspire_touchpad_glide_enabled = 1;
  nspire_touchpad_glide_aspect_mode = GLIDE_ASPECT_EQUAL;
  nspire_touchpad_glide_deadzone = 31;
  nspire_touchpad_glide_invert_x = 1;
  nspire_touchpad_glide_invert_y = 0;
  nspire_touchpad_glide_smoothing = 1;
  nspire_touchpad_glide_smooth_strength = GLIDE_SMOOTH_HIGH;
  nspire_input_separation_mode = INPUT_SEPARATION_SMART;
  nspire_input_slice_frames = 5;
  nspire_touchpad_glide_masks[GLIDE_DIR_UP] = BUTTON_A | BUTTON_R;
  nspire_touchpad_glide_masks[GLIDE_DIR_DOWN] = BUTTON_B | BUTTON_SELECT;
  nspire_touchpad_glide_masks[GLIDE_DIR_LEFT] = BUTTON_LEFT | BUTTON_START;
  nspire_touchpad_glide_masks[GLIDE_DIR_RIGHT] = BUTTON_DOWN | BUTTON_L;
  CHECK(nspire_config_save() == 0, "save");
  CHECK(file_size(path) == 388, "gpsp_qol.cfg.tns is %ld bytes", file_size(path));
  CHECK(file_size(old_path) < 0, "older builds' gpsp.cfg.tns left alone");

  keys_set_defaults(key_bindings);
  screen_scale = scaled_raw;
  current_frameskip_type = auto_frameskip;
  frameskip_value = 0;
  nspire_fps_overlay = 0;
  nspire_auto_increment = nspire_save_on_exit = nspire_load_on_start = 1;
  nspire_cpu_speed = 0;
  nspire_rom_buffer_size_choice = 8;
  nspire_touchpad_swipe = 1;
  nspire_touchpad_deadzone = 2;
  nspire_touchpad_sensitivity = 2;
  nspire_touchpad_glide_enabled = 0;
  nspire_touchpad_glide_aspect_mode = GLIDE_ASPECT_4_3;
  nspire_touchpad_glide_deadzone = 8;
  nspire_touchpad_glide_invert_x = 0;
  nspire_touchpad_glide_invert_y = 1;
  nspire_touchpad_glide_smoothing = 0;
  nspire_touchpad_glide_smooth_strength = GLIDE_SMOOTH_MEDIUM;
  nspire_input_separation_mode = INPUT_SEPARATION_OFF;
  nspire_input_slice_frames = 2;
  memset(nspire_touchpad_glide_masks, 0, sizeof(nspire_touchpad_glide_masks));
  CHECK(nspire_config_load() == 0, "load");
  CHECK(screen_scale == fullscreen && current_frameskip_type == manual_frameskip && frameskip_value == 3,
        "graphics settings %u %u %u", screen_scale, current_frameskip_type, frameskip_value);
  CHECK(nspire_fps_overlay == 2 && !nspire_auto_increment && !nspire_save_on_exit && nspire_load_on_start &&
        nspire_cpu_speed == 2 && nspire_rom_buffer_size_choice == 12, "the new settings");
  CHECK(nspire_touchpad_swipe == 0 && nspire_touchpad_deadzone == 5 &&
        nspire_touchpad_sensitivity == 6, "swipe settings survive save/load");
  CHECK(nspire_touchpad_glide_enabled == 1 && nspire_touchpad_glide_deadzone == 31 &&
        nspire_touchpad_glide_invert_x == 1 && nspire_touchpad_glide_invert_y == 0 &&
        nspire_touchpad_glide_aspect_mode == GLIDE_ASPECT_EQUAL &&
        nspire_touchpad_glide_smoothing == 1 &&
        nspire_touchpad_glide_smooth_strength == GLIDE_SMOOTH_HIGH &&
        nspire_input_separation_mode == INPUT_SEPARATION_SMART &&
        nspire_input_slice_frames == 5,
        "glide settings survive save/load");
  CHECK(nspire_touchpad_glide_masks[GLIDE_DIR_UP] == (BUTTON_A | BUTTON_R) &&
        nspire_touchpad_glide_masks[GLIDE_DIR_DOWN] == (BUTTON_B | BUTTON_SELECT) &&
        nspire_touchpad_glide_masks[GLIDE_DIR_LEFT] == (BUTTON_LEFT | BUTTON_START) &&
        nspire_touchpad_glide_masks[GLIDE_DIR_RIGHT] == (BUTTON_DOWN | BUTTON_L),
        "native glide button masks survive save/load");
  CHECK(key_bindings[ACTION_A][0].key == KEY_DEL && key_bindings[ACTION_A][1].key == KEY_7, "A's keys");
  CHECK(key_bindings[ACTION_SAVE_STATE][1].key == KEY_S && key_bindings[ACTION_SAVE_STATE][1].with == KEY_SHIFT,
        "combo kept");
  CHECK(key_bindings[ACTION_RESTART][0].key == KEY_9, "new hotkey kept");

  /* The first start: a 148-byte gpsp.cfg.tns from gpSP before, its keys
   * and settings, new defaults. */
  remove(path);
  memset(old, 0, sizeof(old));
  old[0] = scaled_aspect;
  old[7 + ACTION_UP] = KEY_8;
  old[7 + ACTION_DOWN] = KEY_2;
  old[7 + ACTION_LEFT] = KEY_4;
  old[7 + ACTION_RIGHT] = KEY_6;
  old[7 + ACTION_A] = KEY_CTRL;
  old[7 + ACTION_B] = KEY_SHIFT;
  old[7 + ACTION_L] = 0;
  old[7 + ACTION_R] = 200;   /* garbage */
  old[7 + ACTION_START] = KEY_ENTER;
  old[7 + ACTION_SELECT] = KEY_VAR;
  old[7 + ACTION_MENU] = KEY_ESC;
  old[7 + ACTION_FAST_FORWARD] = KEY_F;
  old[7 + ACTION_LOAD_STATE] = KEY_L;
  old[7 + ACTION_SAVE_STATE] = KEY_S;
  old[7 + ACTION_QUIT] = KEY_Q;
  old[23] = 1;
  old[24] = manual_frameskip;
  old[25] = 2;
  old[26] = 0xFF000000u;
  old[33] = 1;
  old[36] = 1;
  f = fopen(old_path, "wb");
  fwrite(old, 4, 37, f);
  fclose(f);
  nspire_auto_increment = 0;
  CHECK(nspire_config_load() == 0, "load an old file");
  /* Older builds showed every scaling as the 320x214 picture, so that's
   * what their settings start with. */
  CHECK(screen_scale == scaled_raw && current_frameskip_type == manual_frameskip && frameskip_value == 2 &&
        nspire_fps_overlay == 1 && nspire_video_renderer_choice == 1, "old settings");
  CHECK(nspire_touchpad_swipe == 1 && nspire_touchpad_deadzone == 2 &&
        nspire_touchpad_sensitivity == 2, "legacy config gets safe swipe defaults");
  CHECK(nspire_touchpad_glide_enabled == 0 && nspire_touchpad_glide_deadzone == 8 &&
        nspire_touchpad_glide_invert_x == 0 && nspire_touchpad_glide_invert_y == 0 &&
        nspire_touchpad_glide_aspect_mode == GLIDE_ASPECT_4_3 &&
        nspire_touchpad_glide_smoothing == 0 &&
        nspire_touchpad_glide_smooth_strength == GLIDE_SMOOTH_MEDIUM &&
        nspire_input_separation_mode == INPUT_SEPARATION_OFF &&
        nspire_input_slice_frames == 2,
        "legacy config gets safe glide defaults");
  CHECK(!nspire_touchpad_glide_masks[GLIDE_DIR_UP] && !nspire_touchpad_glide_masks[GLIDE_DIR_DOWN] &&
        !nspire_touchpad_glide_masks[GLIDE_DIR_LEFT] && !nspire_touchpad_glide_masks[GLIDE_DIR_RIGHT],
        "legacy config leaves all glide maps empty");
  CHECK(key_bindings[ACTION_UP][1].key == KEY_SWIPE_UP &&
        key_bindings[ACTION_RIGHT][1].key == KEY_SWIPE_RIGHT,
        "legacy config gains swipe defaults without replacing primary keys");
  CHECK(key_bindings[ACTION_A][0].key == KEY_CTRL && key_bindings[ACTION_MENU][0].key == KEY_ESC &&
        key_bindings[ACTION_QUIT][0].key == KEY_Q, "old keys");
  CHECK(key_bindings[ACTION_L][0].key == 0 && key_bindings[ACTION_R][0].key == 0, "unbound and garbage keys");
  CHECK(key_bindings[ACTION_A][1].key == 0 && key_bindings[ACTION_RESTART][0].key == 0, "new slots empty");
  CHECK(key_bindings[ACTION_BRIGHTER][0].key == KEY_PLUS && key_bindings[ACTION_DIMMER][0].key == KEY_MINUS,
        "brightness keys, + and - being free");
  /* A file written before has auto_increment etc. at their defaults: they
   * aren't in it, so whatever was set stays (main() starts from defaults).
   * Saving writes this build's own file and leaves the old one alone. */
  CHECK(nspire_config_save() == 0, "save after the old file");
  CHECK(file_size(path) == 388 && file_size(old_path) == 148, "files %ld %ld", file_size(path),
        file_size(old_path));

  /* A game's own settings. */
  snprintf(rom, sizeof(rom), "%s/Game.gba.tns", dir);
  nspire_save_file(game_cfg, sizeof(game_cfg), "Game.gba.settings.tns");
  remove(game_cfg);
  screen_scale = scaled_raw;
  frameskip_value = 4;
  nspire_config_open_game(rom);
  CHECK(nspire_settings_for == 0 && nspire_keys_for == 0, "no own settings yet");
  nspire_settings_for = 1;
  nspire_config_settings_for_changed();
  screen_scale = unscaled;   /* the game's own */
  nspire_touchpad_glide_enabled = 0;
  nspire_touchpad_glide_aspect_mode = GLIDE_ASPECT_EQUAL;
  nspire_touchpad_glide_deadzone = 12;
  nspire_touchpad_glide_smoothing = 1;
  nspire_touchpad_glide_smooth_strength = GLIDE_SMOOTH_LOW;
  nspire_input_separation_mode = INPUT_SEPARATION_ALL;
  nspire_input_slice_frames = 4;
  nspire_keys_for = 1;
  nspire_config_keys_for_changed();
  key_bindings[ACTION_B][0] = binding_unpack(KEY_9);
  nspire_touchpad_glide_masks[GLIDE_DIR_LEFT] = BUTTON_A | BUTTON_SELECT;
  CHECK(nspire_config_save() == 0, "save with a game open");
  CHECK(file_size(game_cfg) > 0, "game file written");
  nspire_config_close_game();
  CHECK(screen_scale == scaled_raw, "all games' scale back: %u", screen_scale);
  CHECK(key_bindings[ACTION_B][0].key == KEY_SHIFT, "all games' keys back");

  /* The compressed ROM has the same settings. */
  snprintf(rom, sizeof(rom), "%s/Game.gbz.tns", dir);
  nspire_config_open_game(rom);
  CHECK(nspire_settings_for == 1 && nspire_keys_for == 1, "own settings found");
  CHECK(screen_scale == unscaled && key_bindings[ACTION_B][0].key == KEY_9, "own values used");
  CHECK(nspire_touchpad_glide_enabled == 0 && nspire_touchpad_glide_deadzone == 12 &&
        nspire_touchpad_glide_aspect_mode == GLIDE_ASPECT_EQUAL &&
        nspire_touchpad_glide_smoothing == 1 &&
        nspire_touchpad_glide_smooth_strength == GLIDE_SMOOTH_LOW &&
        nspire_input_separation_mode == INPUT_SEPARATION_ALL &&
        nspire_input_slice_frames == 4 &&
        nspire_touchpad_glide_masks[GLIDE_DIR_LEFT] == (BUTTON_A | BUTTON_SELECT),
        "game profile restores Glide and input-sharing settings");
  frameskip_value = 7;   /* a change while the game has its own settings */
  nspire_config_close_game();
  CHECK(frameskip_value == 4, "all games' frameskip untouched: %u", frameskip_value);
  nspire_config_load();
  CHECK(screen_scale == scaled_raw, "the file for all games kept its scale: %u", screen_scale);

  nspire_config_open_game(rom);
  CHECK(frameskip_value == 7, "the game's change was kept: %u", frameskip_value);
  nspire_settings_for = 0;
  nspire_config_settings_for_changed();
  CHECK(screen_scale == scaled_raw && frameskip_value == 4, "back to all games' settings");
  nspire_keys_for = 0;
  nspire_config_keys_for_changed();
  nspire_config_close_game();
  CHECK(file_size(game_cfg) < 0, "the game's file goes when it has nothing of its own");

  /* Changes made while a game has no settings of its own are for all games. */
  nspire_config_open_game(rom);
  nspire_fps_overlay = 1;
  nspire_config_close_game();
  CHECK(nspire_fps_overlay == 1, "kept for all games");
}

static void write_words(const char *path, const u32 *words, u32 count)
{
  FILE *f = fopen(path, "wb");
  fwrite(words, 4, count, f);
  fclose(f);
}

static int file_contains(const char *path, const char *text)
{
  char buffer[4096];
  size_t n;
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  n = fread(buffer, 1, sizeof(buffer) - 1, f);
  fclose(f);
  buffer[n] = 0;
  return strstr(buffer, text) != NULL;
}

/* This build's settings, and files from the first QoL build ("QOL1"). */
static void test_config_versions(void)
{
  char path[600], old_path[600], rom[600], game_cfg[600];
  u32 o[97];
  FILE *f;

  snprintf(main_path, sizeof(main_path), "%s", dir);
  nspire_save_file(path, sizeof(path), "gpsp_qol.cfg.tns");
  snprintf(old_path, sizeof(old_path), "%s/gpsp.cfg.tns", dir);
  remove(old_path);

  /* Brightness and the brightness keys go round. */
  keys_set_defaults(key_bindings);
  CHECK(key_bindings[ACTION_BRIGHTER][0].key == KEY_PLUS && key_bindings[ACTION_DIMMER][0].key == KEY_MINUS,
        "brightness defaults");
  key_bindings[ACTION_BRIGHTER][1] = binding_unpack(KEY_9 | (KEY_SHIFT << 8));
  key_bindings[ACTION_DIMMER][0] = binding_unpack(KEY_7);
  nspire_brightness = 7;
  nspire_cpu_speed = 9;
  screen_scale = unscaled;   /* chosen in this build: kept */
  CHECK(nspire_config_save() == 0, "save");
  keys_set_defaults(key_bindings);
  nspire_brightness = 0;
  nspire_cpu_speed = 0;
  screen_scale = scaled_raw;
  CHECK(nspire_config_load() == 0, "load");
  CHECK(nspire_brightness == 7 && nspire_cpu_speed == 9 && screen_scale == unscaled,
        "brightness %u, cpu %u, scale %u", nspire_brightness, nspire_cpu_speed, screen_scale);
  CHECK(key_bindings[ACTION_BRIGHTER][0].key == KEY_PLUS && key_bindings[ACTION_BRIGHTER][1].key == KEY_9 &&
        key_bindings[ACTION_BRIGHTER][1].with == KEY_SHIFT && key_bindings[ACTION_DIMMER][0].key == KEY_7,
        "brightness keys kept");
  CHECK(key_bindings[ACTION_RESTART][0].key == 0 && key_bindings[ACTION_A][0].key == KEY_DEL, "others kept");

  /* A 340-byte settings file from the immediately previous build has combo
   * maps but no Glide fields; it must remain readable with Glide defaults. */
  f = fopen(path, "rb");
  CHECK(f && fread(o, 4, 97, f) == 97, "read current settings before truncating to the old layout");
  if (f)
    fclose(f);
  write_words(path, o, 85);
  nspire_touchpad_glide_enabled = 1;
  nspire_touchpad_glide_aspect_mode = GLIDE_ASPECT_EQUAL;
  nspire_touchpad_glide_deadzone = 75;
  nspire_touchpad_glide_invert_x = nspire_touchpad_glide_invert_y = 1;
  CHECK(nspire_config_load() == 0 && nspire_touchpad_glide_enabled == 0 &&
        nspire_touchpad_glide_deadzone == 8 && !nspire_touchpad_glide_invert_x &&
        !nspire_touchpad_glide_invert_y &&
        nspire_touchpad_glide_aspect_mode == GLIDE_ASPECT_4_3 &&
        nspire_touchpad_glide_smoothing == 0 &&
        nspire_touchpad_glide_smooth_strength == GLIDE_SMOOTH_MEDIUM &&
        nspire_input_separation_mode == INPUT_SEPARATION_OFF &&
        nspire_input_slice_frames == 2,
        "previous 340-byte configs load with safe Glide defaults");

  /* The first QoL build's file: its unscaled picture becomes the 320x214
   * one, its CPU speeds (normal, 432, 456, 480) move to today's list, and
   * brightness gets + and - unless they're taken. */
  memset(o, 0, sizeof(o));
  o[0] = unscaled;
  o[37] = 0x314C4F51u;   /* "QOL1" */
  o[38 + ACTION_START] = KEY_PLUS;   /* + on Start */
  o[38 + ACTION_A] = KEY_DEL;
  o[58] = 1;
  o[59] = 1;
  o[60] = 1;
  o[61] = 1;   /* 432 MHz */
  o[62] = 1;
  write_words(path, o, 72);
  keys_set_defaults(key_bindings);
  nspire_brightness = 4;
  CHECK(nspire_config_load() == 0, "load QOL1");
  CHECK(screen_scale == scaled_raw, "QOL1's unscaled is now the 320x214 picture: %u", screen_scale);
  CHECK(nspire_cpu_speed == 3, "432 MHz is speed %u now", nspire_cpu_speed);
  CHECK(key_bindings[ACTION_START][0].key == KEY_PLUS, "QOL1's keys");
  CHECK(key_bindings[ACTION_BRIGHTER][0].key == 0 && key_bindings[ACTION_BRIGHTER][1].key == 0,
        "+ is Start's, so brightness up has no key");
  CHECK(key_bindings[ACTION_DIMMER][0].key == KEY_MINUS, "- was free");
  CHECK(nspire_brightness == 4, "brightness isn't in QOL1 files: %u", nspire_brightness);
  o[61] = 3;   /* 480 MHz */
  write_words(path, o, 72);
  nspire_config_load();
  CHECK(nspire_cpu_speed == 7, "480 MHz is speed %u now", nspire_cpu_speed);
  o[61] = 2;
  write_words(path, o, 72);
  nspire_config_load();
  CHECK(nspire_cpu_speed == 5, "456 MHz is speed %u now", nspire_cpu_speed);
  CHECK(nspire_config_save() == 0 && file_size(path) == 388, "saved as this build's");
  f = fopen(path, "rb");
  CHECK(f && fread(o, 4, 97, f) == 97, "read back");
  if (f)
    fclose(f);
  CHECK(o[37] == 0x334C4F51u && o[61] == 5 && o[0] == scaled_raw && o[66] == 1,
        "QOL3 %08x, cpu %u, scale %u, compress %u", o[37], o[61], o[0], o[66]);
  CHECK(o[67] == 0x31505753u && o[68] == nspire_touchpad_swipe &&
        o[69] == nspire_touchpad_deadzone && o[70] == nspire_touchpad_sensitivity,
        "swipe options use reserved config words");
  CHECK(o[85] == 0x33444C47u && o[86] == 0 && o[87] == 8 &&
        o[90] == 0 && o[91] == 0 && o[92] == GLIDE_ASPECT_4_3 &&
        o[93] == 0 && o[94] == GLIDE_SMOOTH_MEDIUM &&
        o[95] == INPUT_SEPARATION_OFF && o[96] == 2,
        "GLD3 glide and input options use the appended config words");

  /* GLD2 preserves aspect settings and gives the new controls safe defaults. */
  o[85] = 0x32444C47u;
  write_words(path, o, 93);
  nspire_touchpad_glide_smoothing = 1;
  nspire_input_separation_mode = INPUT_SEPARATION_ALL;
  nspire_input_slice_frames = 6;
  CHECK(nspire_config_load() == 0 &&
        nspire_touchpad_glide_aspect_mode == GLIDE_ASPECT_4_3 &&
        nspire_touchpad_glide_smoothing == 0 &&
        nspire_touchpad_glide_smooth_strength == GLIDE_SMOOTH_MEDIUM &&
        nspire_input_separation_mode == INPUT_SEPARATION_OFF &&
        nspire_input_slice_frames == 2,
        "GLD2 settings migrate with defaults for smoothing and multiplexing");

  /* GLD1 had a raw 20 px deadzone and no aspect mode; migrate both safely. */
  o[85] = 0x31444C47u;
  o[87] = 20;
  write_words(path, o, 92);
  CHECK(nspire_config_load() == 0 && nspire_touchpad_glide_deadzone == 8 &&
        nspire_touchpad_glide_aspect_mode == GLIDE_ASPECT_4_3,
        "GLD1 raw deadzone migrates to normalized percent and 4:3 mode");

  /* A game's own settings from the first QoL build (no version line). */
  snprintf(rom, sizeof(rom), "%s/Old.gba.tns", dir);
  nspire_save_file(game_cfg, sizeof(game_cfg), "Old.gba.settings.tns");
  f = fopen(game_cfg, "w");
  fprintf(f, "own_settings=1\nscreen_scale=0\ncpu_speed=3\nframeskip_value=6\n");
  fclose(f);
  screen_scale = fullscreen;
  nspire_config_open_game(rom);
  CHECK(screen_scale == scaled_raw && nspire_cpu_speed == 7 && frameskip_value == 6,
        "game file: scale %u cpu %u frameskip %u", screen_scale, nspire_cpu_speed, frameskip_value);
  nspire_config_close_game();
  CHECK(file_contains(game_cfg, "version=10") && file_contains(game_cfg, "cpu_speed=7"), "rewritten");
  nspire_config_open_game(rom);
  CHECK(nspire_cpu_speed == 7 && screen_scale == scaled_raw, "a version 3 file is read as it is");
  screen_scale = unscaled;
  nspire_config_close_game();
  nspire_config_open_game(rom);
  CHECK(screen_scale == unscaled, "unscaled chosen for the game since stays");
  nspire_settings_for = 0;
  nspire_config_settings_for_changed();
  nspire_config_close_game();
  remove(game_cfg);
  remove(path);
}

/* The slot keys, ) and ( as in PocketSNES: defaults, and settings from
 * before them (QOL2 files, version 2 game files) getting them only where
 * the keys are free; QOL3 files keep what was chosen, even no key. */
/* The settings for all games go inside the program file too, and come from
 * there first; a program without them (a new one copied over) takes the
 * saves folder's copy, and before that this build's file next to gpSP. */
static void test_settings_in_program(void)
{
  char program[600], copy[600], beside[600];
  long bare, stored;
  u8 data[400];
  FILE *f;
  int i;

  snprintf(main_path, sizeof(main_path), "%s", dir);
  snprintf(program, sizeof(program), "%s/_gpSP.tns", dir);
  nspire_save_file(copy, sizeof(copy), "gpsp_qol.cfg.tns");
  snprintf(beside, sizeof(beside), "%s/gpsp_qol.cfg.tns", dir);
  f = fopen(program, "wb");
  for (i = 0; i < 5000; i++)
    fputc(i & 0xFF, f);
  fclose(f);
  bare = file_size(program);
  snprintf(nspire_program_path, sizeof(nspire_program_path), "%.500s", program);

  keys_set_defaults(key_bindings);
  nspire_brightness = 4;
  CHECK(nspire_config_save() == 0, "save");
  stored = file_size(program);
  CHECK(stored == bare + SELFSTORE_SIZE + 16, "the program file grew by its settings area: %ld", stored);
  CHECK(selfstore_read(program, data, sizeof(data)) == 388, "388 bytes stored in it");
  f = fopen(program, "rb");
  for (i = 0; f && i < 5000 && fgetc(f) == (i & 0xFF); i++)
    ;
  if (f)
    fclose(f);
  CHECK(i == 5000, "the program's own bytes untouched (%d)", i);
  CHECK(file_size(copy) == 388, "and the copy in the saves folder");

  /* From the program file first. */
  remove(copy);
  nspire_brightness = 9;
  CHECK(nspire_config_load() == 0 && nspire_brightness == 4, "loaded from the program: %u", nspire_brightness);
  nspire_brightness = 6;
  CHECK(nspire_config_save() == 0 && file_size(program) == stored, "saved again in place");
  CHECK(nspire_config_load() == 0 && nspire_brightness == 6, "the new value: %u", nspire_brightness);

  /* A new program file copied over: the copy in the saves folder. */
  f = fopen(program, "wb");
  for (i = 0; i < 5000; i++)
    fputc(i & 0xFF, f);
  fclose(f);
  nspire_brightness = 2;
  CHECK(nspire_config_load() == 0 && nspire_brightness == 6, "from the saves folder's copy: %u",
        nspire_brightness);
  CHECK(nspire_config_save() == 0 && file_size(program) == stored, "back in the program");

  /* Before the saves folder: the file next to gpSP. */
  rename(copy, beside);
  f = fopen(program, "wb");
  fclose(f);
  nspire_brightness = 1;
  CHECK(nspire_config_load() == 0 && nspire_brightness == 6, "from the file next to gpSP: %u",
        nspire_brightness);
  remove(beside);
  remove(program);
  nspire_program_path[0] = 0;
  nspire_brightness = 0;
  remove(copy);
}

static void test_slot_keys(void)
{
  char path[600], rom[600], game_cfg[600];
  u32 o[72];
  FILE *f;

  keys_set_defaults(key_bindings);
  CHECK(key_bindings[ACTION_NEXT_SLOT][0].key == KEY_RPAREN, "next slot: )");
  CHECK(key_bindings[ACTION_PREVIOUS_SLOT][0].key == KEY_LPAREN, "previous slot: (");

  /* A QOL2 file (the build before) with the slots unbound and ( on Start. */
  nspire_save_file(path, sizeof(path), "gpsp_qol.cfg.tns");
  keys_set_defaults(key_bindings);
  key_bindings[ACTION_NEXT_SLOT][0].key = 0;
  key_bindings[ACTION_PREVIOUS_SLOT][0].key = 0;
  key_bindings[ACTION_START][1].key = KEY_LPAREN;
  nspire_compress_saves = 0;
  CHECK(nspire_config_save() == 0, "save");
  f = fopen(path, "r+b");
  CHECK(f && fread(o, 4, 72, f) == 72, "read");
  o[37] = 0x324C4F51u;   /* "QOL2" */
  o[66] = 0;
  fseek(f, 0, SEEK_SET);
  fwrite(o, 4, 72, f);
  fclose(f);
  keys_set_defaults(key_bindings);
  nspire_compress_saves = 0;
  CHECK(nspire_config_load() == 0, "load QOL2");
  CHECK(key_bindings[ACTION_NEXT_SLOT][0].key == KEY_RPAREN, "QOL2: next slot gets ), which is free");
  CHECK(key_bindings[ACTION_PREVIOUS_SLOT][0].key == 0 && key_bindings[ACTION_PREVIOUS_SLOT][1].key == 0,
        "QOL2: ( is Start's second key, so previous slot has none");
  CHECK(key_bindings[ACTION_START][1].key == KEY_LPAREN, "Start keeps (");
  CHECK(nspire_compress_saves == 1, "QOL2: Compress saves starts on");

  /* Saved as QOL3: a slot key cleared on purpose stays cleared. */
  memset(key_bindings[ACTION_NEXT_SLOT], 0, sizeof(key_bindings[ACTION_NEXT_SLOT]));
  nspire_compress_saves = 0;
  CHECK(nspire_config_save() == 0, "save QOL3");
  keys_set_defaults(key_bindings);
  nspire_compress_saves = 1;
  CHECK(nspire_config_load() == 0, "load QOL3");
  CHECK(key_bindings[ACTION_NEXT_SLOT][0].key == 0, "QOL3: next slot stays without a key");
  CHECK(nspire_compress_saves == 0, "QOL3: Compress saves off is kept");

  /* A game's own keys from before version 3. */
  keys_set_defaults(key_bindings);
  snprintf(rom, sizeof(rom), "%s/Keys.gba.tns", dir);
  nspire_save_file(game_cfg, sizeof(game_cfg), "Keys.gba.settings.tns");
  f = fopen(game_cfg, "w");
  fprintf(f, "version=2\nown_keys=1\nkey_next_slot=0\nkey_previous_slot=0\nkey_a=%u\n", KEY_RPAREN);
  fclose(f);
  /* Simulate an all-games map that cleared a swipe slot; old game-specific
   * profiles didn't know about swipes, so the missing defaults are restored
   * only in that legacy profile and the global user's choice is preserved. */
  key_bindings[ACTION_UP][1].key = 0;
  key_bindings[ACTION_RIGHT][1].key = 0;
  nspire_config_open_game(rom);
  CHECK(key_bindings[ACTION_A][0].key == KEY_RPAREN, "the game's A is )");
  CHECK(key_bindings[ACTION_NEXT_SLOT][0].key == 0, "so its next slot has no key");
  CHECK(key_bindings[ACTION_PREVIOUS_SLOT][0].key == KEY_LPAREN, "and its previous slot gets (");
  CHECK(key_bindings[ACTION_UP][1].key == KEY_SWIPE_UP &&
        key_bindings[ACTION_RIGHT][1].key == KEY_SWIPE_RIGHT,
        "legacy per-game key map gets missing swipe defaults");
  nspire_config_close_game();
  CHECK(key_bindings[ACTION_UP][1].key == 0 && key_bindings[ACTION_RIGHT][1].key == 0,
        "legacy game's defaults do not overwrite the all-games map");
  CHECK(file_contains(game_cfg, "version=10"), "the game file is version 10 now");
  remove(game_cfg);
  nspire_compress_saves = 1;
  remove(path);
}

static void test_brightness(void)
{
  u32 level;
  int i;

  CHECK(nspire_brightness_value(BRIGHTNESS_LEVELS) == 0 && nspire_brightness_value(1) == 225,
        "the OS's range: 0 brightest, 225 darkest");
  for (level = 1; level <= BRIGHTNESS_LEVELS; level++)
  {
    CHECK(nspire_brightness_level(nspire_brightness_value(level)) == level, "level %u round trip", level);
    if (level > 1)
      CHECK(nspire_brightness_value(level) < nspire_brightness_value(level - 1), "level %u brighter", level);
  }
  CHECK(nspire_brightness_level(255) == 1 && nspire_brightness_level(0) == BRIGHTNESS_LEVELS &&
        nspire_brightness_level(120) == 5 && nspire_brightness_level(13) == 9 && nspire_brightness_level(12) == 10,
        "nearest levels");

  /* Setting 0 leaves the calculator's own alone. */
  unit_backlight = 120;
  unit_backlight_writes = 0;
  nspire_brightness = 0;
  nspire_brightness_init();
  nspire_brightness_apply();
  nspire_brightness_restore();
  CHECK(unit_backlight_writes == 0 && unit_backlight == 120, "untouched: %ld writes", unit_backlight_writes);

  /* The hotkeys start from the level nearest the calculator's. */
  nspire_brightness_step(1);
  CHECK(nspire_brightness == 6 && unit_backlight == 100 && !strcmp(unit_osd_message, "Brightness 6/10"),
        "one up: level %u, %u, \"%s\"", nspire_brightness, unit_backlight, unit_osd_message);
  for (i = 0; i < 12; i++)
    nspire_brightness_step(1);
  CHECK(nspire_brightness == 10 && unit_backlight == 0, "stops at the brightest");
  for (i = 0; i < 12; i++)
    nspire_brightness_step(-1);
  CHECK(nspire_brightness == 1 && unit_backlight == 225, "stops at the darkest");
  nspire_brightness = 0;
  nspire_brightness_apply();
  CHECK(unit_backlight == 120, "back to the calculator's own");
  nspire_brightness = 3;
  nspire_brightness_apply();
  CHECK(unit_backlight == nspire_brightness_value(3), "level 3");
  nspire_brightness = 99;
  nspire_brightness_apply();
  CHECK(nspire_brightness == 0 && unit_backlight == 120, "a bad setting is the calculator's own");
  nspire_brightness = 8;
  nspire_brightness_apply();
  nspire_brightness_restore();
  CHECK(unit_backlight == 120, "gpSP closing puts the calculator's own back");

  /* A register that doesn't read like the OS's is left alone. */
  unit_backlight = 0x12345;
  unit_backlight_writes = 0;
  nspire_brightness_init();
  nspire_brightness = 5;
  nspire_brightness_apply();
  nspire_brightness_step(1);
  nspire_brightness_restore();
  CHECK(unit_backlight_writes == 0 && !strcmp(unit_osd_message, "Brightness: CX II only"),
        "unknown register: %ld writes, \"%s\"", unit_backlight_writes, unit_osd_message);
  unit_backlight = 120;
  nspire_brightness = 0;
  nspire_brightness_init();
}

static void test_game_list(void)
{
  char *names[] = { "b.gba.tns", "A.gba.tns", "c.gbz.tns", "D.gba.tns" };
  char *dirs[] = { "zelda", "..", "Alpha" };
  char order_path[700];
  FILE *f;

  game_list_sort(names, 4);
  CHECK(!strcmp(names[0], "A.gba.tns") && !strcmp(names[1], "b.gba.tns") && !strcmp(names[2], "c.gbz.tns") &&
        !strcmp(names[3], "D.gba.tns"), "A to Z, ignoring case");
  game_list_sort(dirs, 3);
  CHECK(!strcmp(dirs[0], "..") && !strcmp(dirs[1], "Alpha") && !strcmp(dirs[2], "zelda"), "folders, .. first");

  game_list_move(names, 4, 3, 0);
  CHECK(!strcmp(names[0], "D.gba.tns") && !strcmp(names[1], "A.gba.tns") && !strcmp(names[3], "c.gbz.tns"),
        "moved up");
  game_list_move(names, 4, 0, 2);
  CHECK(!strcmp(names[0], "A.gba.tns") && !strcmp(names[1], "b.gba.tns") && !strcmp(names[2], "D.gba.tns"),
        "moved down");
  game_list_move(names, 4, 4, 0);
  game_list_move(names, 4, 1, 1);
  CHECK(!strcmp(names[1], "b.gba.tns") && !strcmp(names[3], "c.gbz.tns"), "nothing moves out of range");

  nspire_order_file(order_path, sizeof(order_path), dir);
  game_list_forget_order(dir);
  game_list_sort(names, 4);
  CHECK(game_list_apply_order(names, 4, dir) == 0 && !strcmp(names[0], "A.gba.tns"), "no order: A to Z");

  /* D first, then c; a game since deleted is skipped. */
  f = fopen(order_path, "w");
  fprintf(f, "D.gba.tns\nGone.gba.tns\r\nc.gbz.tns\n");
  fclose(f);
  CHECK(game_list_apply_order(names, 4, dir) == 1, "has an order");
  CHECK(!strcmp(names[0], "D.gba.tns") && !strcmp(names[1], "c.gbz.tns") && !strcmp(names[2], "A.gba.tns") &&
        !strcmp(names[3], "b.gba.tns"), "the order, then the rest A to Z: %s %s %s %s", names[0], names[1],
        names[2], names[3]);

  game_list_move(names, 4, 3, 1);
  CHECK(game_list_save_order(names, 4, dir) == 1, "saved");
  game_list_sort(names, 4);
  game_list_apply_order(names, 4, dir);
  CHECK(!strcmp(names[0], "D.gba.tns") && !strcmp(names[1], "b.gba.tns") && !strcmp(names[2], "c.gbz.tns") &&
        !strcmp(names[3], "A.gba.tns"), "the saved order comes back");

  /* A folder name ending in a slash. */
  {
    char slashed[600];
    snprintf(slashed, sizeof(slashed), "%s/", dir);
    game_list_sort(names, 4);
    CHECK(game_list_apply_order(names, 4, slashed) == 1 && !strcmp(names[0], "D.gba.tns"), "dir with a slash");
  }

  game_list_forget_order(dir);
  CHECK(file_size(order_path) < 0, "sorting A to Z forgets it");
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    fprintf(stderr, "usage: frontend_tests DIR\n");
    return 2;
  }
  snprintf(dir, sizeof(dir), "%s", argv[1]);
  snprintf(unit_documents_dir, sizeof(unit_documents_dir), "%s/", dir);
  nspire_save_dir_make();

  printf("turning frames\n");
  test_turn_raw();
  test_turn_1x();
  test_turn_rects();
  printf("key bindings\n");
  test_keys();
  printf("touchpad swipe pipeline\n");
  test_swipe_input_pipeline();
  printf("touchpad glide pipeline\n");
  test_glide_input_pipeline();
  printf("glide smoothing\n");
  test_glide_smoothing();
  printf("input separation scheduler\n");
  test_input_separation_scheduler();
  printf("settings files\n");
  test_config();
  test_config_versions();
  printf("settings in the program file\n");
  test_settings_in_program();
  printf("slot keys\n");
  test_slot_keys();
  printf("brightness\n");
  test_brightness();
  printf("file list order\n");
  test_game_list();
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
