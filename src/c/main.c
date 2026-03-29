#include <pebble.h>
#include <stdint.h>
#include <string.h>

#define PERSIST_KEY_TEMPERATURE 10
#define PERSIST_KEY_WEATHER_TIME 11
#define PERSIST_KEY_TEMP_MIN 12
#define PERSIST_KEY_TEMP_MAX 13
#define PERSIST_KEY_WEATHER_CODE 14

static Window *s_window;
static Layer *s_canvas;

static GFont s_font_time;
static GFont s_font_sm;
static GFont s_font_sm_bold;
static GFont s_font_xs;

static char s_hour_buf[8];
static char s_min_buf[8];
static char s_date_buf[12];
static char s_temp_buf[24];

static int s_temp_cur;
static int s_temp_min;
static int s_temp_max;
static int s_weather_code;
static bool s_has_weather;

static int s_phone_batt;
static int s_watch_batt;
static bool s_phone_batt_known;

static bool s_bt_connected;
static bool s_bt_prev;

static int32_t clamp_i(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static void update_time(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  strftime(s_hour_buf, sizeof(s_hour_buf), clock_is_24h_style() ? "%H" : "%I", t);
  strftime(s_min_buf, sizeof(s_min_buf), "%M", t);
  if (!clock_is_24h_style() && s_hour_buf[0] == '0' && s_hour_buf[1]) {
    memmove(s_hour_buf, s_hour_buf + 1, strlen(s_hour_buf));
  }

  strftime(s_date_buf, sizeof(s_date_buf), "%a %d", t);
  if (s_date_buf[0] >= 'a' && s_date_buf[0] <= 'z') s_date_buf[0] -= 32;
  for (int i = 1; s_date_buf[i] && s_date_buf[i] != ' '; i++) {
    if (s_date_buf[i] >= 'A' && s_date_buf[i] <= 'Z') s_date_buf[i] += 32;
  }
}

// ---------------------------------------------------------------------------
// Rounded slider bar with position marker
// ---------------------------------------------------------------------------

static void draw_slider(GContext *ctx, GRect track, int32_t lo, int32_t hi, int32_t val) {
  int w = track.size.w;
  int h = track.size.h;
  if (w <= 0 || h <= 0) return;

  int32_t span = (hi > lo) ? (hi - lo) : 1;
  int32_t cv = clamp_i(val, lo, hi);
  int fw = (int)(((int64_t)(cv - lo) * w) / span);
  fw = clamp_i(fw, 0, w);

  uint16_t r = (uint16_t)(h / 2);
  if (r > 8) r = 8;
  if (r < 1) r = 1;

  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorDarkGray));
  graphics_fill_rect(ctx, track, r, GCornersAll);

  if (fw > 0) {
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
    graphics_fill_rect(ctx, GRect(track.origin.x, track.origin.y, fw, h), r, GCornersAll);
  }

  int mx = track.origin.x + fw;
  mx = clamp_i(mx, track.origin.x + 1, track.origin.x + w - 2);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(mx - 1, track.origin.y - 1, 3, h + 2), 0, GCornerNone);
}

// ---------------------------------------------------------------------------
// Framebuffer dithering for B&W two-tone effect
// ---------------------------------------------------------------------------

#ifdef PBL_BW
static void dither_rect(GContext *ctx, GRect area) {
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  uint8_t *data = gbitmap_get_data(fb);
  uint16_t stride = gbitmap_get_bytes_per_row(fb);
  GRect bounds = gbitmap_get_bounds(fb);

  int x0 = area.origin.x > 0 ? area.origin.x : 0;
  int y0 = area.origin.y > 0 ? area.origin.y : 0;
  int x1 = area.origin.x + area.size.w;
  int y1 = area.origin.y + area.size.h;
  if (x1 > bounds.origin.x + bounds.size.w) x1 = bounds.origin.x + bounds.size.w;
  if (y1 > bounds.origin.y + bounds.size.h) y1 = bounds.origin.y + bounds.size.h;

  for (int y = y0; y < y1; y++) {
    uint8_t *row = data + y * stride;
    for (int x = x0; x < x1; x++) {
      if ((x + y) % 2 == 0) {
        row[x / 8] &= ~(1 << (x % 8));
      }
    }
  }
  graphics_release_frame_buffer(ctx, fb);
}
#endif

// ---------------------------------------------------------------------------
// Bluetooth icon (runic B shape, ~10x14)
// ---------------------------------------------------------------------------

static void draw_bt_icon(GContext *ctx, int cx, int cy, bool connected) {
  graphics_context_set_stroke_width(ctx, 1);
  if (connected) {
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
    graphics_draw_line(ctx, GPoint(cx, cy - 7), GPoint(cx, cy + 7));
    graphics_draw_line(ctx, GPoint(cx, cy - 7), GPoint(cx + 5, cy - 3));
    graphics_draw_line(ctx, GPoint(cx + 5, cy - 3), GPoint(cx - 4, cy + 4));
    graphics_draw_line(ctx, GPoint(cx, cy + 7), GPoint(cx + 5, cy + 3));
    graphics_draw_line(ctx, GPoint(cx + 5, cy + 3), GPoint(cx - 4, cy - 4));
  } else {
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
    graphics_draw_line(ctx, GPoint(cx - 5, cy - 5), GPoint(cx + 5, cy + 5));
    graphics_draw_line(ctx, GPoint(cx + 5, cy - 5), GPoint(cx - 5, cy + 5));
  }
}

// ---------------------------------------------------------------------------
// Weather code → short label
// ---------------------------------------------------------------------------

static const char *weather_label(int code) {
  switch (code) {
    case 0:                         return "Clear";
    case 1: case 2: case 3:        return "Cloudy";
    case 45: case 48:              return "Fog";
    case 51: case 53: case 55:
    case 56: case 57:              return "Drizzle";
    case 61: case 63: case 65:
    case 66: case 67:
    case 80: case 81: case 82:     return "Rain";
    case 71: case 73: case 75:
    case 77: case 85: case 86:     return "Snow";
    case 95: case 96: case 99:     return "Thunder";
    default:                        return "";
  }
}

// ---------------------------------------------------------------------------
// Top-left: temperature + range slider
// ---------------------------------------------------------------------------

static void draw_corner_tl(GContext *ctx, int lx, int ty, int cw) {
  if (s_has_weather) {
    snprintf(s_temp_buf, sizeof(s_temp_buf), "%d\u00B0 %s", s_temp_cur, weather_label(s_weather_code));
  } else {
    snprintf(s_temp_buf, sizeof(s_temp_buf), "--\u00B0");
  }

  GRect track = GRect(lx, ty, cw, 5);
  if (s_has_weather) {
    draw_slider(ctx, track, s_temp_min, s_temp_max, s_temp_cur);
  } else {
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorDarkGray));
    graphics_fill_rect(ctx, track, 2, GCornersAll);
  }

  graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
  graphics_draw_text(ctx, s_temp_buf, s_font_sm,
      GRect(lx, ty + 5, cw, 22),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

// ---------------------------------------------------------------------------
// Top-right: "Fri 27"
// ---------------------------------------------------------------------------

static void draw_corner_tr(GContext *ctx, int rx, int ty, int cw) {
  graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
  graphics_draw_text(ctx, s_date_buf, s_font_sm,
      GRect(rx, ty, cw, 22),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

// ---------------------------------------------------------------------------
// Bottom-left: bluetooth + phone battery
// ---------------------------------------------------------------------------

static void draw_corner_bl(GContext *ctx, int lx, int by, int cw) {
  draw_bt_icon(ctx, lx + 6, by + 10, s_bt_connected);

  if (s_bt_connected && s_phone_batt_known) {
    static char pb[8];
    snprintf(pb, sizeof(pb), "%d%%", s_phone_batt);
    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
    graphics_draw_text(ctx, pb, s_font_sm,
        GRect(lx + 16, by, cw - 16, 22),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  } else if (!s_bt_connected) {
    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
    graphics_draw_text(ctx, "disc.", s_font_xs,
        GRect(lx + 16, by + 2, cw - 16, 18),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
}

// ---------------------------------------------------------------------------
// Bottom-right: watch battery + slider
// ---------------------------------------------------------------------------

static void draw_corner_br(GContext *ctx, int rx, int by, int cw) {
  static char wb[8];
  snprintf(wb, sizeof(wb), "%d%%", s_watch_batt);

  graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
  graphics_draw_text(ctx, wb, s_font_sm,
      GRect(rx, by, cw, 22),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);

  GRect track = GRect(rx, by + 22, cw, 5);
  draw_slider(ctx, track, 0, 100, s_watch_batt);
}

// ---------------------------------------------------------------------------
// Center time (two-tone stacked, same size)
// ---------------------------------------------------------------------------

static void draw_time(GContext *ctx, int cx, int cy) {
  int tw = 140;
  int tx = cx - tw / 2;
  int line_h = 68;
  int gap = -18;
  int total = line_h * 2 + gap;
  int y0 = cy - total / 2;

  GRect min_rect = GRect(tx, y0 + line_h + gap, tw, line_h + 4);

#ifdef PBL_BW
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_min_buf, s_font_time, min_rect,
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  dither_rect(ctx, min_rect);

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_hour_buf, s_font_time,
      GRect(tx, y0, tw, line_h + 4),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
#else
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, s_hour_buf, s_font_time,
      GRect(tx, y0, tw, line_h + 4),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  graphics_context_set_text_color(ctx, GColorDarkGray);
  graphics_draw_text(ctx, s_min_buf, s_font_time, min_rect,
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
#endif
}

// ---------------------------------------------------------------------------
// Canvas: three horizontal bands — top corners, center time, bottom corners
// ---------------------------------------------------------------------------

static void canvas_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int w = b.size.w;
  int h = b.size.h;

  int mx = PBL_IF_ROUND_ELSE(30, 8);
  int my = PBL_IF_ROUND_ELSE(24, 6);

  int cw = (w - mx * 2) / 2 - 4;
  int lx = mx;
  int rx = w - mx - cw;

  draw_corner_tl(ctx, lx, my, cw);
  draw_corner_tr(ctx, rx, 0, cw);

  int by = h - my - 22;
  draw_corner_bl(ctx, lx, h - 22, cw);
  draw_corner_br(ctx, rx, by, cw);

  draw_time(ctx, w / 2, h / 2);
}

// ---------------------------------------------------------------------------
// Weather persistence + messaging
// ---------------------------------------------------------------------------

static void apply_weather(int temp, int tmin, int tmax, int wcode) {
  s_temp_cur = temp;
  s_temp_min = tmin;
  s_temp_max = tmax;
  s_weather_code = wcode;
  s_has_weather = true;
  layer_mark_dirty(s_canvas);
}

static void load_cached_weather(void) {
  if (!persist_exists(PERSIST_KEY_WEATHER_TIME)) return;
  if ((int)time(NULL) - persist_read_int(PERSIST_KEY_WEATHER_TIME) > 3600) return;
  apply_weather(
      persist_read_int(PERSIST_KEY_TEMPERATURE),
      persist_read_int(PERSIST_KEY_TEMP_MIN),
      persist_read_int(PERSIST_KEY_TEMP_MAX),
      persist_read_int(PERSIST_KEY_WEATHER_CODE));
}

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *temp_t = dict_find(iter, MESSAGE_KEY_Temperature);
  Tuple *tmin_t = dict_find(iter, MESSAGE_KEY_TempMin);
  Tuple *tmax_t = dict_find(iter, MESSAGE_KEY_TempMax);
  Tuple *wcode_t = dict_find(iter, MESSAGE_KEY_WeatherCode);
  Tuple *phone_t = dict_find(iter, MESSAGE_KEY_PhoneBattery);

  if (temp_t && tmin_t && tmax_t) {
    int temp = (int)temp_t->value->int32;
    int tmin = (int)tmin_t->value->int32;
    int tmax = (int)tmax_t->value->int32;
    int wcode = wcode_t ? (int)wcode_t->value->int32 : 0;
    persist_write_int(PERSIST_KEY_TEMPERATURE, temp);
    persist_write_int(PERSIST_KEY_TEMP_MIN, tmin);
    persist_write_int(PERSIST_KEY_TEMP_MAX, tmax);
    persist_write_int(PERSIST_KEY_WEATHER_CODE, wcode);
    persist_write_int(PERSIST_KEY_WEATHER_TIME, (int)time(NULL));
    apply_weather(temp, tmin, tmax, wcode);
  }

  if (phone_t) {
    int pb = (int)phone_t->value->int32;
    s_phone_batt_known = (pb >= 0);
    s_phone_batt = (pb >= 0) ? pb : 0;
    layer_mark_dirty(s_canvas);
  }
}

// ---------------------------------------------------------------------------
// Services
// ---------------------------------------------------------------------------

static void battery_handler(BatteryChargeState state) {
  s_watch_batt = state.charge_percent;
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void bt_handler(bool connected) {
  if (s_bt_prev && !connected) vibes_short_pulse();
  s_bt_prev = connected;
  s_bt_connected = connected;
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time();
  if (s_canvas) layer_mark_dirty(s_canvas);
  if (tick_time->tm_min % 30 == 0) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
      dict_write_uint8(iter, MESSAGE_KEY_Ping, 0);
      app_message_outbox_send();
    }
  }
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_font_time     = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_MONO_60));
  s_font_sm       = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  s_font_sm_bold  = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_font_xs       = fonts_get_system_font(FONT_KEY_GOTHIC_14);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_draw);
  layer_add_child(root, s_canvas);

  s_watch_batt = battery_state_service_peek().charge_percent;
  s_bt_connected = bluetooth_connection_service_peek();
  s_bt_prev = s_bt_connected;
  s_phone_batt = 0;
  s_phone_batt_known = false;
  s_has_weather = false;
  s_temp_cur = 0;
  s_temp_min = 0;
  s_temp_max = 1;

  update_time();
  load_cached_weather();
}

static void window_unload(Window *window) {
  fonts_unload_custom_font(s_font_time);
  layer_destroy(s_canvas);
  s_canvas = NULL;
}

static void init(void) {
  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(256, 128);
  battery_state_service_subscribe(battery_handler);
  bluetooth_connection_service_subscribe(bt_handler);

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  bluetooth_connection_service_unsubscribe();
  battery_state_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
