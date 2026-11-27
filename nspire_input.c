/* Ndless keypad input for libretro gpSP core (replaces libretro/input.c):
 * the keypad, key bindings (nspire_keys.h), the GBA buttons and hotkeys of a
 * frame, and menu input with key repeat. */

#include "common.h"
#include "input.h"
#include "serial.h"
#include "gba_memory.h"
#include "gui.h"
#include "nspire.h"
#include "nspire_gui_video.h"
#include "nspire_keys.h"
#include "nspire_hw.h"
#include "nspire_brightness.h"

#include <os.h>
#include <libndls.h>

bool libretro_supports_bitmasks    = false;
bool libretro_supports_ff_override = false;
bool libretro_ff_enabled           = false;
bool libretro_ff_enabled_prev      = false;

unsigned turbo_period      = TURBO_PERIOD_MIN;
unsigned turbo_pulse_width = TURBO_PULSE_WIDTH_MIN;
unsigned turbo_a_counter   = 0;
unsigned turbo_b_counter   = 0;

u32 global_enable_analog = 0;
u32 analog_sensitivity_level = 4;

static u32 old_key = 0;

void retro_set_input_state(retro_input_state_t cb) { (void)cb; }

void set_fastforward_override(bool fastforward) { (void)fastforward; }

static void trigger_key(u32 key)
{
  u32 p1_cnt = read_ioreg(REG_P1CNT);

  if ((p1_cnt >> 14) & 0x01)
  {
    u32 key_intersection = (p1_cnt & key) & 0x3FF;

    if (p1_cnt >> 15)
    {
      if (key_intersection == (p1_cnt & 0x3FF))
      {
        flag_interrupt(IRQ_KEYPAD);
        check_and_raise_interrupts();
      }
    }
    else
    {
      if (key_intersection)
      {
        flag_interrupt(IRQ_KEYPAD);
        check_and_raise_interrupts();
      }
    }
  }
}

/* ---- The keypad ---- */

#define TOUCHPAD_KEYS_MASK 0xFF00FFFD
#define TOUCHPAD_DEFAULT_WIDTH 320u
#define TOUCHPAD_DEFAULT_HEIGHT 240u
#define GLIDE_WEIGHT_ONE 4096u

static u32 current_keys[NUM_KEY_WORDS];
static u32 new_keys[NUM_KEY_WORDS];
static unsigned touchpad_width = TOUCHPAD_DEFAULT_WIDTH;
static unsigned touchpad_height = TOUCHPAD_DEFAULT_HEIGHT;
static int swipe_contact_active;
static unsigned short swipe_origin_x, swipe_origin_y;
/* Raw (un-pulsed) swipe directions currently above the deadzone, used during
 * key_capture() so capturing a swipe never drops out mid-duty-cycle, and
 * per-axis weights (0..100) for proportional diagonal duty-cycling in-game. */
static unsigned swipe_raw_directions;
static unsigned swipe_x_weight, swipe_y_weight;
static unsigned swipe_x_accum, swipe_y_accum;
/* Preserve edge-arrow navigation in menus while reserving those zones from
 * GBA gameplay input when swipe mode is enabled. */
static unsigned current_touchpad_arrow;
static int touchpad_available;
static int touchpad_info_checked;
static int glide_was_touching;
static int glide_was_enabled;
static int glide_anchor_x, glide_anchor_y;
static int glide_latched_wx, glide_latched_wy;
static int glide_smooth_vx, glide_smooth_vy;
static int glide_hysteresis_x_active, glide_hysteresis_y_active;
static unsigned glide_smoothing_mode;
static int glide_smoothing_mode_known;
static unsigned glide_accum_x, glide_accum_y;
static unsigned glide_hold_remaining_x, glide_hold_remaining_y;
static int glide_hold_direction_x, glide_hold_direction_y;
static unsigned short glide_current_buttons[NUM_GLIDE_DIRS];
static unsigned input_current_layer_idx, input_slice_frame_counter;
static unsigned input_previous_separation_mode;

/* The glide maps use the native bit positions already consumed by gba_buttons. */
const unsigned short touchpad_glide_button_bits[NUM_GBA_ACTIONS] =
{
  BUTTON_UP, BUTTON_DOWN, BUTTON_LEFT, BUTTON_RIGHT,
  BUTTON_A, BUTTON_B, BUTTON_L, BUTTON_R, BUTTON_START, BUTTON_SELECT
};

static void touchpad_initialize(void)
{
  if (!touchpad_info_checked)
  {
    if (is_touchpad)
    {
      touchpad_info_t *tp_info = touchpad_getinfo();
      if (tp_info && tp_info->width && tp_info->height)
      {
        touchpad_width = tp_info->width;
        touchpad_height = tp_info->height;
        touchpad_available = 1;
      }
    }
    /* Preserve the old 4:3 swipe fallback on devices without a raw touchpad. */
    touchpad_info_checked = 1;
  }
}

static void touchpad_glide_reset(void)
{
  glide_was_touching = 0;
  glide_anchor_x = glide_anchor_y = 0;
  glide_latched_wx = glide_latched_wy = 0;
  glide_smooth_vx = glide_smooth_vy = 0;
  glide_hysteresis_x_active = glide_hysteresis_y_active = 0;
  glide_smoothing_mode_known = 0;
  glide_accum_x = glide_accum_y = 0;
  glide_hold_remaining_x = glide_hold_remaining_y = 0;
  glide_hold_direction_x = glide_hold_direction_y = 0;
  memset(glide_current_buttons, 0, sizeof(glide_current_buttons));
}

/* Integer square root used only after a movement crosses the deadzone. */
static unsigned touchpad_glide_isqrt(unsigned value)
{
  unsigned result = 0;
  unsigned bit = 1u << 30;

  while (bit > value)
    bit >>= 2;
  while (bit)
  {
    if (value >= result + bit)
    {
      value -= result + bit;
      result = (result >> 1) + bit;
    }
    else
    {
      result >>= 1;
    }
    bit >>= 2;
  }
  return result;
}

/* Digital buttons approximate an axis weight with a Q12 duty-cycle pulse. */
static int touchpad_glide_axis_pulse(int weight, unsigned *accum)
{
  unsigned amount;
  if (!weight)
  {
    *accum = 0;
    return 0;
  }
  amount = (unsigned)(weight < 0 ? -weight : weight);
  *accum += amount;
  if (*accum >= GLIDE_WEIGHT_ONE)
  {
    *accum -= GLIDE_WEIGHT_ONE;
    return 1;
  }
  return 0;
}

/* Smoothing groups fractional pulses into short, continuous button holds. */
static int touchpad_glide_axis_held(int weight, unsigned *accum,
                                    unsigned *hold_remaining,
                                    int *hold_direction, unsigned hold_frames)
{
  unsigned threshold = GLIDE_WEIGHT_ONE * hold_frames;
  int sign = weight > 0 ? 1 : -1;

  if (weight)
  {
    *accum += (unsigned)(weight < 0 ? -weight : weight);
    if (*accum >= threshold)
    {
      *accum -= threshold;
      *hold_remaining = hold_frames;
      *hold_direction = sign;
    }
  }

  if (*hold_remaining)
  {
    (*hold_remaining)--;
    return *hold_direction;
  }
  if (!weight)
    *hold_direction = 0;
  return 0;
}

/* Anchor based movement latches its normalized angle until another valid
 * glide step arrives or contact ends. X/Y remain independent for diagonals. */
static void touchpad_glide_update(const touchpad_report_t *report)
{
  static const unsigned alpha_q12[3] = { 2662u, 1638u, 1024u };
  int dx, dy, vx, vy, new_wx, new_wy;
  unsigned weight_x, weight_y, deadzone, abs_vx, abs_vy;
  unsigned magnitude_sq, deadzone_sq, magnitude;
  unsigned smoothing, strength, hold_frames;

  if (!nspire_touchpad_glide_enabled)
  {
    if (glide_was_enabled)
      touchpad_glide_reset();
    glide_was_enabled = 0;
    return;
  }
  glide_was_enabled = 1;
  smoothing = nspire_touchpad_glide_smoothing % 2u;
  strength = nspire_touchpad_glide_smooth_strength % 3u;
  hold_frames = strength == GLIDE_SMOOTH_HIGH ? 3u : 2u;
  if (!glide_smoothing_mode_known || glide_smoothing_mode != smoothing)
  {
    glide_smooth_vx = glide_smooth_vy = 0;
    glide_hysteresis_x_active = smoothing && glide_latched_wx != 0;
    glide_hysteresis_y_active = smoothing && glide_latched_wy != 0;
    glide_accum_x = glide_accum_y = smoothing ? GLIDE_WEIGHT_ONE * hold_frames : 0;
    glide_hold_remaining_x = glide_hold_remaining_y = 0;
    glide_hold_direction_x = glide_hold_direction_y = 0;
    glide_smoothing_mode = smoothing;
    glide_smoothing_mode_known = 1;
  }
  if (!touchpad_available)
  {
    touchpad_glide_reset();
    return;
  }
  if (!report->contact)
  {
    touchpad_glide_reset();
    return;
  }
  if (!glide_was_touching)
  {
    glide_anchor_x = (int)report->x;
    glide_anchor_y = (int)report->y;
    glide_was_touching = 1;
    glide_latched_wx = glide_latched_wy = 0;
    glide_smooth_vx = glide_smooth_vy = 0;
    glide_hysteresis_x_active = glide_hysteresis_y_active = 0;
    glide_accum_x = glide_accum_y = 0;
    glide_hold_remaining_x = glide_hold_remaining_y = 0;
    glide_hold_direction_x = glide_hold_direction_y = 0;
    memset(glide_current_buttons, 0, sizeof(glide_current_buttons));
    return;
  }

  dx = (int)report->x - glide_anchor_x;
  dy = (int)report->y - glide_anchor_y;
  if (nspire_touchpad_glide_invert_x)
    dx = -dx;
  if (nspire_touchpad_glide_invert_y)
    dy = -dy;

  if (nspire_touchpad_glide_aspect_mode == GLIDE_ASPECT_EQUAL)
  {
    weight_x = weight_y = 1;
  }
  else
  {
    weight_x = 4;
    weight_y = 3;
  }

  /* Q12 normalized displacement. The deadzone is a percentage of the
   * shorter weighted touchpad span (3 in 4:3 mode, 1 in equal mode). */
  vx = (dx * (int)weight_x * (int)GLIDE_WEIGHT_ONE) / (int)touchpad_width;
  vy = (dy * (int)weight_y * (int)GLIDE_WEIGHT_ONE) / (int)touchpad_height;
  abs_vx = (unsigned)(vx < 0 ? -vx : vx);
  abs_vy = (unsigned)(vy < 0 ? -vy : vy);
  magnitude_sq = abs_vx * abs_vx + abs_vy * abs_vy;
  deadzone = (nspire_touchpad_glide_deadzone % (GLIDE_DEADZONE_MAX + 1u)) *
             GLIDE_WEIGHT_ONE * (weight_y == 3 ? 3u : 1u) / 100u;
  deadzone_sq = deadzone * deadzone;

  if (magnitude_sq && magnitude_sq >= deadzone_sq)
  {
    if (smoothing)
    {
      unsigned alpha = alpha_q12[strength];
      glide_smooth_vx = ((int)alpha * vx +
        (int)(GLIDE_WEIGHT_ONE - alpha) * glide_smooth_vx) /
        (int)GLIDE_WEIGHT_ONE;
      glide_smooth_vy = ((int)alpha * vy +
        (int)(GLIDE_WEIGHT_ONE - alpha) * glide_smooth_vy) /
        (int)GLIDE_WEIGHT_ONE;
      vx = glide_smooth_vx;
      vy = glide_smooth_vy;
    }

    abs_vx = (unsigned)(vx < 0 ? -vx : vx);
    abs_vy = (unsigned)(vy < 0 ? -vy : vy);
    magnitude_sq = abs_vx * abs_vx + abs_vy * abs_vy;
    magnitude = touchpad_glide_isqrt(magnitude_sq);
    if (magnitude)
    {
      new_wx = (vx * (int)GLIDE_WEIGHT_ONE) / (int)magnitude;
      new_wy = (vy * (int)GLIDE_WEIGHT_ONE) / (int)magnitude;

      if (smoothing)
      {
        unsigned abs_wx = (unsigned)(new_wx < 0 ? -new_wx : new_wx);
        unsigned abs_wy = (unsigned)(new_wy < 0 ? -new_wy : new_wy);
        if (glide_hysteresis_x_active)
        {
          if (abs_wx * 10u < GLIDE_WEIGHT_ONE)
            glide_hysteresis_x_active = 0;
        }
        else if (abs_wx * 5u >= GLIDE_WEIGHT_ONE)
        {
          glide_hysteresis_x_active = 1;
        }
        if (glide_hysteresis_y_active)
        {
          if (abs_wy * 10u < GLIDE_WEIGHT_ONE)
            glide_hysteresis_y_active = 0;
        }
        else if (abs_wy * 5u >= GLIDE_WEIGHT_ONE)
        {
          glide_hysteresis_y_active = 1;
        }
        if (!glide_hysteresis_x_active) new_wx = 0;
        if (!glide_hysteresis_y_active) new_wy = 0;
      }
      else if ((unsigned)(new_wx < 0 ? -new_wx : new_wx) <= GLIDE_WEIGHT_ONE / 10u)
      {
        /* Preserve the original cardinal snap whenever smoothing is off. */
        new_wx = 0;
        new_wy = vy < 0 ? -(int)GLIDE_WEIGHT_ONE : (int)GLIDE_WEIGHT_ONE;
      }
      else if ((unsigned)(new_wy < 0 ? -new_wy : new_wy) <= GLIDE_WEIGHT_ONE / 10u)
      {
        new_wy = 0;
        new_wx = vx < 0 ? -(int)GLIDE_WEIGHT_ONE : (int)GLIDE_WEIGHT_ONE;
      }

      /* Prime a changed axis for an immediate first pulse; same-direction
       * movement retains its fractional remainder for smooth duty cycling. */
      if (!new_wx)
        glide_accum_x = 0;
      else if (!glide_latched_wx || ((glide_latched_wx < 0) != (new_wx < 0)))
      {
        glide_accum_x = smoothing ? GLIDE_WEIGHT_ONE * hold_frames : GLIDE_WEIGHT_ONE;
        glide_hold_remaining_x = 0;
        glide_hold_direction_x = 0;
      }
      if (!new_wy)
        glide_accum_y = 0;
      else if (!glide_latched_wy || ((glide_latched_wy < 0) != (new_wy < 0)))
      {
        glide_accum_y = smoothing ? GLIDE_WEIGHT_ONE * hold_frames : GLIDE_WEIGHT_ONE;
        glide_hold_remaining_y = 0;
        glide_hold_direction_y = 0;
      }

      glide_latched_wx = new_wx;
      glide_latched_wy = new_wy;
    }
    glide_anchor_x = (int)report->x;
    glide_anchor_y = (int)report->y;
  }

  /* A sub-deadzone pause leaves the anchor and latched weights untouched, so
   * the last direction keeps pulsing until movement changes it or contact lifts. */
  memset(glide_current_buttons, 0, sizeof(glide_current_buttons));
  if (smoothing)
  {
    int active_x = touchpad_glide_axis_held(glide_latched_wx, &glide_accum_x,
      &glide_hold_remaining_x, &glide_hold_direction_x, hold_frames);
    int active_y = touchpad_glide_axis_held(glide_latched_wy, &glide_accum_y,
      &glide_hold_remaining_y, &glide_hold_direction_y, hold_frames);
    if (active_x)
      glide_current_buttons[active_x > 0 ? GLIDE_DIR_RIGHT : GLIDE_DIR_LEFT] =
        nspire_touchpad_glide_masks[active_x > 0 ? GLIDE_DIR_RIGHT : GLIDE_DIR_LEFT];
    if (active_y)
      glide_current_buttons[active_y > 0 ? GLIDE_DIR_UP : GLIDE_DIR_DOWN] =
        nspire_touchpad_glide_masks[active_y > 0 ? GLIDE_DIR_UP : GLIDE_DIR_DOWN];
  }
  else
  {
    glide_hold_remaining_x = glide_hold_remaining_y = 0;
    glide_hold_direction_x = glide_hold_direction_y = 0;
    if (touchpad_glide_axis_pulse(glide_latched_wx, &glide_accum_x))
      glide_current_buttons[glide_latched_wx > 0 ? GLIDE_DIR_RIGHT : GLIDE_DIR_LEFT] =
        nspire_touchpad_glide_masks[glide_latched_wx > 0 ? GLIDE_DIR_RIGHT : GLIDE_DIR_LEFT];
    if (touchpad_glide_axis_pulse(glide_latched_wy, &glide_accum_y))
      glide_current_buttons[glide_latched_wy > 0 ? GLIDE_DIR_UP : GLIDE_DIR_DOWN] =
        nspire_touchpad_glide_masks[glide_latched_wy > 0 ? GLIDE_DIR_UP : GLIDE_DIR_DOWN];
  }
}

static void touchpad_get_size(void)
{
  touchpad_initialize();
}

static void set_virtual_key(u32 *mask, unsigned key)
{
  if (key > 128 && key <= NUM_KEYS)
    *mask |= 1u << ((key - 1) & 31);
}

static u32 touchpad_swipe_keys(const touchpad_report_t *report)
{
  static const unsigned deadzones[SWIPE_DEADZONE_CHOICES] = { 3, 5, 8, 10, 12, 15, 20 };
  static const unsigned sensitivities[SWIPE_SENSITIVITY_CHOICES] = { 50, 75, 100, 125, 175, 150, 200 };
  static const unsigned sens_table[SWIPE_SENSITIVITY_CHOICES] = { 50, 75, 100, 125, 150, 175, 200 };
  unsigned directions, deadzone, sensitivity;
  unsigned x_weight = 0, y_weight = 0;
  int dx, dy;
  u32 mask = 0;

  (void)sensitivities;
  if (!nspire_touchpad_swipe || !report->contact)
  {
    swipe_contact_active = 0;
    swipe_raw_directions = 0;
    swipe_x_weight = 0;
    swipe_y_weight = 0;
    swipe_x_accum = 0;
    swipe_y_accum = 0;
    return 0;
  }

  touchpad_get_size();
  if (!swipe_contact_active)
  {
    swipe_origin_x = report->x;
    swipe_origin_y = report->y;
    swipe_contact_active = 1;
    swipe_raw_directions = 0;
    swipe_x_weight = 0;
    swipe_y_weight = 0;
    swipe_x_accum = 100;
    swipe_y_accum = 100;
    return 0;
  }

  /* CX II coordinates follow its arrow convention: X grows right, Y grows up.
   * Keep the start point for the entire contact, so a swipe is a held gesture
   * until lift-off rather than a one-poll velocity spike. */
  dx = (int)report->x - (int)swipe_origin_x;
  dy = (int)report->y - (int)swipe_origin_y;
  deadzone = deadzones[nspire_touchpad_deadzone % SWIPE_DEADZONE_CHOICES];
  sensitivity = sens_table[nspire_touchpad_sensitivity % SWIPE_SENSITIVITY_CHOICES];

  touchpad_swipe_axis_weights(dx, dy, touchpad_width, touchpad_height,
                              deadzone, sensitivity,
                              &directions, &x_weight, &y_weight);
  swipe_raw_directions = directions;
  swipe_x_weight = x_weight;
  swipe_y_weight = y_weight;

  /* Split any diagonal movement into separate horizontal (LEFT/RIGHT) and
   * vertical (UP/DOWN) inputs, weighted proportionally to how the diagonal
   * movement was made on the 4:3 touchpad via a Bresenham duty-cycle accumulator.
   * If an axis has weight 100 (dominant axis or single-axis swipe), it is active
   * on 100% of frames. If it is the minor axis of a shallow/steep diagonal
   * (e.g. weight 50), it pulses on 50% of frames so the GBA receives a weighted
   * horizontal + vertical vector. */
  if (directions & (SWIPE_DIR_LEFT | SWIPE_DIR_RIGHT))
  {
    int x_active = 1;
    if (x_weight < 100u)
    {
      swipe_x_accum += x_weight;
      if (swipe_x_accum >= 100u)
        swipe_x_accum -= 100u;
      else
        x_active = 0;
    }
    if (x_active)
    {
      if (directions & SWIPE_DIR_LEFT)  set_virtual_key(&mask, KEY_SWIPE_LEFT);
      if (directions & SWIPE_DIR_RIGHT) set_virtual_key(&mask, KEY_SWIPE_RIGHT);
    }
  }
  else
  {
    swipe_x_accum = 100;
  }

  if (directions & (SWIPE_DIR_UP | SWIPE_DIR_DOWN))
  {
    int y_active = 1;
    if (y_weight < 100u)
    {
      swipe_y_accum += y_weight;
      if (swipe_y_accum >= 100u)
        swipe_y_accum -= 100u;
      else
        y_active = 0;
    }
    if (y_active)
    {
      if (directions & SWIPE_DIR_UP)   set_virtual_key(&mask, KEY_SWIPE_UP);
      if (directions & SWIPE_DIR_DOWN) set_virtual_key(&mask, KEY_SWIPE_DOWN);
    }
  }
  else
  {
    swipe_y_accum = 100;
  }

  return mask;
}

void keys_poll(void)
{
  touchpad_report_t report;
  u32 touchpad_pressed = 0;
  unsigned i;

  memset(&report, 0, sizeof(report));
  touchpad_initialize();
  touchpad_scan(&report);
  current_touchpad_arrow = report.arrow;
  if (nspire_touchpad_swipe)
  {
    /* In swipe mode, touching/pressing an edge is not a directional input.
     * Only relative contact movement creates SW-* keys. Retain the old center
     * click behavior; turning swipe mode off restores the original edge arrows. */
    if (report.arrow == TPAD_ARROW_CLICK)
      touchpad_pressed = 0x00000002;
  }
  else
  {
    switch ((tpad_arrow_t)report.arrow)
    {
    case TPAD_ARROW_UP:         touchpad_pressed = 0x00010000; break;
    case TPAD_ARROW_UPRIGHT:    touchpad_pressed = 0x00050000; break;
    case TPAD_ARROW_RIGHT:      touchpad_pressed = 0x00040000; break;
    case TPAD_ARROW_RIGHTDOWN:  touchpad_pressed = 0x00140000; break;
    case TPAD_ARROW_DOWN:       touchpad_pressed = 0x00100000; break;
    case TPAD_ARROW_DOWNLEFT:   touchpad_pressed = 0x00500000; break;
    case TPAD_ARROW_LEFT:       touchpad_pressed = 0x00400000; break;
    case TPAD_ARROW_LEFTUP:     touchpad_pressed = 0x00410000; break;
    case TPAD_ARROW_CLICK:      touchpad_pressed = 0x00000002; break;
    default:                    touchpad_pressed = 0x00000000; break;
    }
  }

  for (i = 0; i < NUM_KEY_WORDS; i++)
    new_keys[i] = ~current_keys[i];

  current_keys[0] = HW_READ(HW_KEYPAD) | (on_key_pressed() ? 0x200 : 0);
  current_keys[1] = HW_READ(HW_KEYPAD + 4);
  current_keys[2] = HW_READ(HW_KEYPAD + 8);
  current_keys[3] = (HW_READ(HW_KEYPAD + 12) & TOUCHPAD_KEYS_MASK) | touchpad_pressed;
  current_keys[4] = touchpad_swipe_keys(&report);
  if (nspire_touchpad_glide_enabled || glide_was_enabled)
    touchpad_glide_update(&report);

  for (i = 0; i < NUM_KEY_WORDS; i++)
    new_keys[i] &= current_keys[i];
}

int key_down(unsigned key)
{
  return key && key <= NUM_KEYS && (current_keys[(key - 1) >> 5] >> ((key - 1) & 0x1F)) & 1;
}

int key_pressed(unsigned key)
{
  return key && key <= NUM_KEYS && (new_keys[(key - 1) >> 5] >> ((key - 1) & 0x1F)) & 1;
}

int any_key_down(void)
{
  unsigned i;
  for (i = 0; i < NUM_KEY_WORDS; i++)
    if (current_keys[i])
      return 1;
  return swipe_contact_active;
}

void keys_wait_release(void)
{
  do
  {
    keys_poll();
    nspire_wait_until(nspire_ticks() + NSPIRE_TICK_HZ / 100);
  } while (any_key_down());
}

int binding_held(key_binding b)
{
  return b.key && key_down(b.key) && (!b.with || key_down(b.with));
}

static int binding_pressed(key_binding b)
{
  return binding_held(b) && (key_pressed(b.key) || (b.with && key_pressed(b.with)));
}

static int action_held(int action)
{
  return binding_held(key_bindings[action][0]) || binding_held(key_bindings[action][1]);
}

static int key_is_bound(unsigned key)
{
  int i, slot;
  for (i = 0; i < NUM_ACTIONS; i++)
    for (slot = 0; slot < KEY_SLOTS; slot++)
      if (key_bindings[i][slot].key == key || key_bindings[i][slot].with == key)
        return 1;
  for (i = 0; i < NUM_COMBO_SLOTS; i++)
    if (combo_bindings[i].action_mask &&
        (combo_bindings[i].trigger.key == key || combo_bindings[i].trigger.with == key))
      return 1;
  return 0;
}

/* Only suppresses single-key hotkeys when a two-key hotkey combo is held.
 * Gameplay controls and multi-control combos NEVER block each other so you
 * can freely hold multiple calculator keys and swipe at the same time! */
static int key_in_held_hotkey_combo(unsigned key)
{
  int i, slot;
  for (i = FIRST_HOTKEY; i < NUM_ACTIONS; i++)
    for (slot = 0; slot < KEY_SLOTS; slot++)
    {
      key_binding b = key_bindings[i][slot];
      if (b.with && (b.key == key || b.with == key) && binding_held(b))
        return 1;
    }
  return 0;
}

int hotkey_pressed(int action)
{
  int slot;
  for (slot = 0; slot < KEY_SLOTS; slot++)
  {
    key_binding b = key_bindings[action][slot];
    if (binding_pressed(b) && (b.with || !key_in_held_hotkey_combo(b.key)))
      return 1;
  }
  return 0;
}

static int key_is_swipe(unsigned key)
{
  return key >= KEY_SWIPE_UP && key <= KEY_SWIPE_RIGHT;
}

/* Returns the dominant single-axis swipe key during key_capture() based on
 * the 4:3-normalized weights so capturing a swipe in the menu always picks
 * the primary direction cleanly. */
static unsigned dominant_swipe_key(void)
{
  if (!swipe_raw_directions)
    return 0;
  if (swipe_x_weight > swipe_y_weight)
  {
    if (swipe_raw_directions & SWIPE_DIR_LEFT)  return KEY_SWIPE_LEFT;
    if (swipe_raw_directions & SWIPE_DIR_RIGHT) return KEY_SWIPE_RIGHT;
  }
  if (swipe_raw_directions & SWIPE_DIR_UP)    return KEY_SWIPE_UP;
  if (swipe_raw_directions & SWIPE_DIR_DOWN)  return KEY_SWIPE_DOWN;
  if (swipe_raw_directions & SWIPE_DIR_LEFT)  return KEY_SWIPE_LEFT;
  if (swipe_raw_directions & SWIPE_DIR_RIGHT) return KEY_SWIPE_RIGHT;
  return 0;
}

static unsigned first_pressed_input(void)
{
  unsigned sw = dominant_swipe_key();
  unsigned key;
  if (sw)
    return sw;
  for (key = 1; key <= NUM_KEYS; key++)
    if (key_pressed(key))
      return key;
  return 0;
}

key_binding key_capture(void)
{
  key_binding b = { 0, 0 };
  unsigned first = 0, key, sw;

  keys_wait_release();
  /* The first key pressed, then either another one while it's held (a
   * combination) or its release (a single key). An arrow or the touchpad
   * click gives up and returns no key. */
  while (!first)
  {
    keys_poll();
    key = first_pressed_input();
    if (key)
    {
      if (key_is_reserved(key))
      {
        keys_wait_release();
        return b;
      }
      if (key_is_bindable(key))
        first = key;
    }
  }
  for (;;)
  {
    keys_poll();
    sw = dominant_swipe_key();

    if (key_is_swipe(first))
    {
      if (!swipe_contact_active)
      {
        b.key = (unsigned char)first;
        break;
      }
      if (sw)
        first = sw;
    }
    else if (!key_down(first))
    {
      b.key = (unsigned char)first;
      break;
    }

    for (key = 1; key <= NUM_KEYS; key++)
      if (key != first && key_pressed(key) && key_is_bindable(key))
      {
        if (key_is_swipe(first) && key_is_swipe(key))
        {
          if (sw)
            first = sw;
          continue;
        }
        b.with = (unsigned char)first;
        b.key = (unsigned char)key;
        break;
      }
    if (b.key)
      break;
  }
  keys_wait_release();
  return b;
}

/* ---- A frame of the game ---- */

/* Computes the combined bitmask of GBA actions (bits 0..9 = ACTION_UP..SELECT)
 * activated by the 4 orthogonal touchpad swipe multi-combos and 8 calculator
 * combo slots. Because a diagonal swipe activates both a horizontal SW-* key
 * and a vertical SW-* key (weighted according to the 4:3 diagonal angle),
 * swiping diagonally automatically combines the horizontal swipe's actions
 * and the vertical swipe's actions simultaneously! */
static unsigned short active_multi_action_mask(void)
{
  unsigned short mask = 0;
  int i;

  /* 1. Direct touchpad swipe multi-control mappings (Up, Down, Left, Right).
   * Both horizontal and vertical components can be active at once on a
   * diagonal swipe, weighted by touchpad_swipe_keys(). */
  for (i = 0; i < NUM_SWIPE_DIRS; i++)
    if (key_down(KEY_SWIPE_UP + (unsigned)i))
      mask |= swipe_action_masks[i];

  /* 2. Custom calculator key / key+with / swipe multi-control combo slots.
   * Multiple combo slots and physical keys can be held simultaneously. */
  for (i = 0; i < NUM_COMBO_SLOTS; i++)
  {
    combo_binding c = combo_bindings[i];
    if (c.action_mask && binding_held(c.trigger))
      mask |= c.action_mask;
  }

  return mask;
}

#define MAX_ACTIVE_INPUT_SOURCES 48u

typedef struct
{
  u16 mask;
  u8 bit_count;
} ActiveInputSource;

static u16 action_mask_to_buttons(unsigned short action_mask)
{
  u16 buttons = 0;
  unsigned i;
  for (i = 0; i < NUM_GBA_ACTIONS; i++)
    if (action_mask & (1u << i))
      buttons |= touchpad_glide_button_bits[i];
  return buttons;
}

static u8 input_mask_bit_count(u16 mask)
{
  u8 count = 0;
  while (mask)
  {
    mask &= (u16)(mask - 1u);
    count++;
  }
  return count;
}

static void append_input_source(ActiveInputSource *sources, unsigned *count,
                                u16 mask)
{
  if (mask && *count < MAX_ACTIVE_INPUT_SOURCES)
  {
    sources[*count].mask = mask;
    sources[*count].bit_count = input_mask_bit_count(mask);
    (*count)++;
  }
}

static int same_binding(key_binding a, key_binding b)
{
  return a.key == b.key && a.with == b.with;
}

#define MAX_COLLECTED_BINDINGS (NUM_GBA_ACTIONS * KEY_SLOTS + NUM_COMBO_SLOTS + NUM_SWIPE_DIRS + 4)

typedef struct
{
  key_binding trigger;
  u16 mask;
} CollectedBinding;

/* Reuse one entry for every mapping attached to the same active calculator
 * trigger, so one key that maps to several buttons remains one source. */
static int collected_binding_index(CollectedBinding *bindings, unsigned *count,
                                   key_binding trigger)
{
  unsigned i;
  for (i = 0; i < *count; i++)
    if (same_binding(bindings[i].trigger, trigger))
      return (int)i;
  if (*count >= MAX_COLLECTED_BINDINGS)
    return -1;
  bindings[*count].trigger = trigger;
  bindings[*count].mask = 0;
  return (int)(*count)++;
}

static unsigned collect_active_input_sources(ActiveInputSource *sources)
{
  static const unsigned char arrow_keys[4] =
    { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
  CollectedBinding bindings[MAX_COLLECTED_BINDINGS];
  unsigned binding_count = 0;
  unsigned count = 0, action, slot, i;

  for (action = 0; action < NUM_GBA_ACTIONS; action++)
    for (slot = 0; slot < KEY_SLOTS; slot++)
    {
      key_binding trigger = key_bindings[action][slot];
      int index;
      if (!trigger.key || !binding_held(trigger))
        continue;
      index = collected_binding_index(bindings, &binding_count, trigger);
      if (index >= 0)
        bindings[index].mask |= touchpad_glide_button_bits[action];
    }

  for (i = 0; i < NUM_COMBO_SLOTS; i++)
  {
    key_binding trigger = combo_bindings[i].trigger;
    int index;
    if (!combo_bindings[i].action_mask || !binding_held(trigger))
      continue;
    index = collected_binding_index(bindings, &binding_count, trigger);
    if (index >= 0)
      bindings[index].mask |= action_mask_to_buttons(combo_bindings[i].action_mask);
  }

  /* Swipe directions and raw touchpad arrows share a source with any
   * configured key or macro mapping on that same trigger. */
  for (i = 0; i < NUM_SWIPE_DIRS; i++)
  {
    key_binding swipe = { (unsigned char)(KEY_SWIPE_UP + i), 0 };
    if (key_down(swipe.key))
    {
      int index = collected_binding_index(bindings, &binding_count, swipe);
      if (index >= 0)
        bindings[index].mask |= action_mask_to_buttons(swipe_action_masks[i]);
    }
  }
  for (i = 0; i < 4; i++)
  {
    key_binding arrow = { arrow_keys[i], 0 };
    if (key_down(arrow.key))
    {
      int index = collected_binding_index(bindings, &binding_count, arrow);
      if (index >= 0)
        bindings[index].mask |= touchpad_glide_button_bits[i];
    }
  }

  for (i = 0; i < binding_count; i++)
    append_input_source(sources, &count, bindings[i].mask);

  /* The number-pad corner shortcuts are one physical input each, with a
   * two-button map. Keep the old bound-key suppression rule. */
  if (key_down(KEY_7) && !key_is_bound(KEY_7))
    append_input_source(sources, &count, BUTTON_UP | BUTTON_LEFT);
  if (key_down(KEY_9) && !key_is_bound(KEY_9))
    append_input_source(sources, &count, BUTTON_UP | BUTTON_RIGHT);
  if (key_down(KEY_1) && !key_is_bound(KEY_1))
    append_input_source(sources, &count, BUTTON_DOWN | BUTTON_LEFT);
  if (key_down(KEY_3) && !key_is_bound(KEY_3))
    append_input_source(sources, &count, BUTTON_DOWN | BUTTON_RIGHT);

  if (nspire_touchpad_glide_enabled)
    for (i = 0; i < NUM_GLIDE_DIRS; i++)
      append_input_source(sources, &count, glide_current_buttons[i]);

  return count;
}

static u16 schedule_input_layers(const ActiveInputSource *sources,
                                 unsigned source_count, unsigned mode,
                                 unsigned slice_frames)
{
  u16 layers[MAX_ACTIVE_INPUT_SOURCES];
  unsigned layer_count = 0, i;

  if (mode == INPUT_SEPARATION_SMART)
  {
    u16 base_layer = 0;
    for (i = 0; i < source_count; i++)
      if (sources[i].bit_count == 1)
        base_layer |= sources[i].mask;
    if (base_layer)
      layers[layer_count++] = base_layer;
    for (i = 0; i < source_count; i++)
      if (sources[i].bit_count >= 2)
        layers[layer_count++] = sources[i].mask;
  }
  else
  {
    for (i = 0; i < source_count; i++)
      layers[layer_count++] = sources[i].mask;
  }

  if (!layer_count)
  {
    input_current_layer_idx = 0;
    input_slice_frame_counter = 0;
    return 0;
  }
  if (layer_count == 1)
  {
    input_current_layer_idx = 0;
    input_slice_frame_counter = 0;
    return layers[0];
  }
  if (input_current_layer_idx >= layer_count)
  {
    input_current_layer_idx = 0;
    input_slice_frame_counter = 0;
  }

  {
    u16 buttons = layers[input_current_layer_idx];
    input_slice_frame_counter++;
    if (input_slice_frame_counter >= slice_frames)
    {
      input_slice_frame_counter = 0;
      input_current_layer_idx = (input_current_layer_idx + 1) % layer_count;
    }
    return buttons;
  }
}

/* Reads the keys for a frame: sets the GBA's buttons and returns the hotkeys
 * just pressed, one bit per action (1 << ACTION_MENU, ...). */
u32 nspire_input_frame(void)
{
  u32 gba_buttons = 0, hotkeys = 0;
  ActiveInputSource active_sources[MAX_ACTIVE_INPUT_SOURCES];
  unsigned active_source_count, separation_mode, slice_frames;
  unsigned short multi_mask;
  int up, down, left, right, i;

  keys_poll();
  separation_mode = nspire_input_separation_mode % 3u;
  if (separation_mode != input_previous_separation_mode)
  {
    input_current_layer_idx = 0;
    input_slice_frame_counter = 0;
    input_previous_separation_mode = separation_mode;
  }

  /* Separation collects each source before choosing a layer. The legacy path
   * below remains unchanged when the option is off (or GBP serial mode owns it). */
  if (separation_mode != INPUT_SEPARATION_OFF && serial_mode != SERIAL_MODE_GBP)
  {
    active_source_count = collect_active_input_sources(active_sources);
    slice_frames = nspire_input_slice_frames;
    if (slice_frames < 1u || slice_frames > 6u)
      slice_frames = 2u;
    gba_buttons = schedule_input_layers(active_sources, active_source_count,
                                        separation_mode, slice_frames);
  }
  else
  {
    multi_mask = active_multi_action_mask();
    up    = key_down(KEY_UP)    || action_held(ACTION_UP)    || ((multi_mask & (1u << ACTION_UP)) != 0);
    down  = key_down(KEY_DOWN)  || action_held(ACTION_DOWN)  || ((multi_mask & (1u << ACTION_DOWN)) != 0);
    left  = key_down(KEY_LEFT)  || action_held(ACTION_LEFT)  || ((multi_mask & (1u << ACTION_LEFT)) != 0);
    right = key_down(KEY_RIGHT) || action_held(ACTION_RIGHT) || ((multi_mask & (1u << ACTION_RIGHT)) != 0);
    /* The number pad's corners are diagonals, unless bound to something. */
    if (key_down(KEY_7) && !key_is_bound(KEY_7)) up = left = 1;
    if (key_down(KEY_9) && !key_is_bound(KEY_9)) up = right = 1;
    if (key_down(KEY_1) && !key_is_bound(KEY_1)) down = left = 1;
    if (key_down(KEY_3) && !key_is_bound(KEY_3)) down = right = 1;
    if (up && down)
      up = down = 0;
    if (left && right)
      left = right = 0;

    if (up)    gba_buttons |= BUTTON_UP;
    if (down)  gba_buttons |= BUTTON_DOWN;
    if (left)  gba_buttons |= BUTTON_LEFT;
    if (right) gba_buttons |= BUTTON_RIGHT;
    if (action_held(ACTION_A)      || (multi_mask & (1u << ACTION_A)))      gba_buttons |= BUTTON_A;
    if (action_held(ACTION_B)      || (multi_mask & (1u << ACTION_B)))      gba_buttons |= BUTTON_B;
    if (action_held(ACTION_L)      || (multi_mask & (1u << ACTION_L)))      gba_buttons |= BUTTON_L;
    if (action_held(ACTION_R)      || (multi_mask & (1u << ACTION_R)))      gba_buttons |= BUTTON_R;
    if (action_held(ACTION_START)  || (multi_mask & (1u << ACTION_START)))  gba_buttons |= BUTTON_START;
    if (action_held(ACTION_SELECT) || (multi_mask & (1u << ACTION_SELECT))) gba_buttons |= BUTTON_SELECT;

    if (serial_mode == SERIAL_MODE_GBP)
    {
      if (frame_counter > 20 && frame_counter < 100)
        gba_buttons = (frame_counter % 3) ? 0x3FF : 0x30F;
    }

    /* Glide remains a non-destructive OR addition in the legacy mode. */
    if (nspire_touchpad_glide_enabled)
      for (i = 0; i < NUM_GLIDE_DIRS; i++)
        gba_buttons |= glide_current_buttons[i];
  }

  if ((gba_buttons | old_key) != old_key)
    trigger_key(gba_buttons);
  old_key = gba_buttons;
  write_ioreg(REG_P1, (~old_key) & 0x3FF);

  for (i = FIRST_HOTKEY; i < NUM_ACTIONS; i++)
    if (hotkey_pressed(i))
      hotkeys |= 1u << i;
  /* Without a key for the menu, the menu key opens it, so it can't get lost. */
  if (!key_bindings[ACTION_MENU][0].key && !key_bindings[ACTION_MENU][1].key &&
      key_pressed(KEY_MENU))
    hotkeys |= 1u << ACTION_MENU;
  return hotkeys;
}

/* The buttons the game sees while it's paused (menus, loading). */
void nspire_input_release_all(void)
{
  old_key = 0;
  touchpad_glide_reset();
  input_current_layer_idx = 0;
  input_slice_frame_counter = 0;
  input_previous_separation_mode = nspire_input_separation_mode % 3u;
  write_ioreg(REG_P1, 0x3FF);
}

void init_input(void)
{
  touchpad_initialize();
}

bool input_check_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  return (p && bson_contains_key(p, "prevkey", BSON_TYPE_INT32));
}

bool input_read_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  if (p)
    return bson_read_int32(p, "prevkey", &old_key);
  return false;
}

unsigned input_write_savestate(u8 *dst)
{
  u8 *wbptr1, *startp = dst;
  bson_start_document(dst, "input", wbptr1);
  bson_write_int32(dst, "prevkey", old_key);
  bson_finish_document(dst, wbptr1);
  return (unsigned int)(dst - startp);
}

/* ---- Menus ---- */

static gui_action_type held_direction = CURSOR_NONE;
static u32 repeat_at;
static unsigned repeats;

int nspire_gui_digits_move = 1;

int nspire_digit_pressed(void)
{
  static const unsigned char digits[10] =
    { KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0 };
  int i;

  for (i = 0; i < 10; i++)
    if (key_pressed(digits[i]))
      return i;
  return -1;
}

static int touchpad_menu_key_down(unsigned key)
{
  if (!nspire_touchpad_swipe)
    return 0;

  switch (key)
  {
  case KEY_UP:
    return current_touchpad_arrow == TPAD_ARROW_UP ||
           current_touchpad_arrow == TPAD_ARROW_UPRIGHT ||
           current_touchpad_arrow == TPAD_ARROW_LEFTUP;
  case KEY_DOWN:
    return current_touchpad_arrow == TPAD_ARROW_DOWN ||
           current_touchpad_arrow == TPAD_ARROW_RIGHTDOWN ||
           current_touchpad_arrow == TPAD_ARROW_DOWNLEFT;
  case KEY_LEFT:
    return current_touchpad_arrow == TPAD_ARROW_LEFT ||
           current_touchpad_arrow == TPAD_ARROW_LEFTUP ||
           current_touchpad_arrow == TPAD_ARROW_DOWNLEFT;
  case KEY_RIGHT:
    return current_touchpad_arrow == TPAD_ARROW_RIGHT ||
           current_touchpad_arrow == TPAD_ARROW_UPRIGHT ||
           current_touchpad_arrow == TPAD_ARROW_RIGHTDOWN;
  default:
    return 0;
  }
}

unsigned gui_repeat_count(void)
{
  return repeats;
}

/* Arrows (or 8 2 5 4 6, except in the file list) move and repeat while
 * held: after 0.4 s, 15 times a second. The brightness keys work here too. */
gui_action_type get_gui_input(void)
{
  gui_action_type direction = CURSOR_NONE;
  u32 now;

  keys_poll();

  if (hotkey_pressed(ACTION_BRIGHTER))
    nspire_brightness_step(1);
  if (hotkey_pressed(ACTION_DIMMER))
    nspire_brightness_step(-1);

  if (key_pressed(KEY_CLICK) || key_pressed(KEY_ENTER))
    return CURSOR_SELECT;
  if (key_pressed(KEY_ESC))
    return CURSOR_BACK;
  if (key_pressed(KEY_MENU) || hotkey_pressed(ACTION_MENU))
    return CURSOR_EXIT;
  if (key_pressed(KEY_DEL))
    return CURSOR_CLEAR;

  if (key_down(KEY_UP) || touchpad_menu_key_down(KEY_UP) ||
      (nspire_gui_digits_move && key_down(KEY_8)))
    direction = CURSOR_UP;
  else if (key_down(KEY_DOWN) || touchpad_menu_key_down(KEY_DOWN) ||
           (nspire_gui_digits_move && (key_down(KEY_5) || key_down(KEY_2))))
    direction = CURSOR_DOWN;
  else if (key_down(KEY_RIGHT) || touchpad_menu_key_down(KEY_RIGHT) ||
           (nspire_gui_digits_move && key_down(KEY_6)))
    direction = CURSOR_RIGHT;
  else if (key_down(KEY_LEFT) || touchpad_menu_key_down(KEY_LEFT) ||
           (nspire_gui_digits_move && key_down(KEY_4)))
    direction = CURSOR_LEFT;

  now = nspire_ticks();
  if (direction != held_direction)
  {
    held_direction = direction;
    repeat_at = now + NSPIRE_TICK_HZ * 2 / 5;
    repeats = 0;
    return direction;
  }
  if (direction != CURSOR_NONE && (s32)(now - repeat_at) >= 0)
  {
    repeat_at = now + NSPIRE_TICK_HZ / 15;
    repeats++;
    return direction;
  }
  return CURSOR_NONE;
}
