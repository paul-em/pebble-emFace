#include <pebble.h>

#define PERSIST_KEY_TEMPERATURE 0
#define PERSIST_KEY_WEATHER_CODE 1
#define PERSIST_KEY_WEATHER_TIME 2

static Window *s_window;
static TextLayer *s_hour_layer;
static TextLayer *s_minute_layer;
static TextLayer *s_date_layer;
static TextLayer *s_temp_layer;
static Layer *s_calendar_icon_layer;
static Layer *s_weather_icon_layer;

static GFont s_time_font;
static GFont s_info_font;

static char s_hour_buf[4];
static char s_minute_buf[4];
static char s_date_buf[16];
static char s_temp_buf[16] = "";
static int s_weather_code = -1;

// ---------------------------------------------------------------------------
// Time & date
// ---------------------------------------------------------------------------

static void update_time(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  strftime(s_hour_buf, sizeof(s_hour_buf), clock_is_24h_style() ? "%H" : "%I", t);
  strftime(s_minute_buf, sizeof(s_minute_buf), "%M", t);
  strftime(s_date_buf, sizeof(s_date_buf), "%a %d", t);

  // Capitalize only the first letter of the day abbreviation
  if (s_date_buf[0] >= 'a' && s_date_buf[0] <= 'z') {
    s_date_buf[0] -= 32;
  }
  for (int i = 1; s_date_buf[i] && s_date_buf[i] != ' '; i++) {
    if (s_date_buf[i] >= 'A' && s_date_buf[i] <= 'Z') {
      s_date_buf[i] += 32;
    }
  }

  text_layer_set_text(s_hour_layer, s_hour_buf);
  text_layer_set_text(s_minute_layer, s_minute_buf);
  text_layer_set_text(s_date_layer, s_date_buf);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time();

  // Request weather refresh every 30 minutes
  if (tick_time->tm_min % 30 == 0) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
      dict_write_uint8(iter, 0, 0);
      app_message_outbox_send();
    }
  }
}

// ---------------------------------------------------------------------------
// Calendar icon  (small outlined rectangle with two tabs on top)
// ---------------------------------------------------------------------------

static void calendar_icon_draw(Layer *layer, GContext *ctx) {
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_fill_color(ctx, GColorWhite);

  graphics_draw_round_rect(ctx, GRect(0, 3, 13, 11), 1);
  graphics_fill_rect(ctx, GRect(3, 0, 2, 5), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(8, 0, 2, 5), 0, GCornerNone);
  graphics_draw_line(ctx, GPoint(0, 7), GPoint(12, 7));
}

// ---------------------------------------------------------------------------
// Weather icon  (drawn from WMO weather code)
// ---------------------------------------------------------------------------

static void draw_cloud(GContext *ctx, int cx, int cy) {
  graphics_fill_circle(ctx, GPoint(cx - 4, cy + 1), 5);
  graphics_fill_circle(ctx, GPoint(cx + 4, cy + 1), 4);
  graphics_fill_circle(ctx, GPoint(cx, cy - 3), 5);
  graphics_fill_rect(ctx, GRect(cx - 9, cy + 1, 18, 6), 0, GCornerNone);
}

static void weather_icon_draw(Layer *layer, GContext *ctx) {
  if (s_weather_code < 0) return;

  GRect b = layer_get_bounds(layer);
  int cx = b.size.w / 2;
  int cy = b.size.h / 2;

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 1);

  if (s_weather_code == 0) {
    // Clear – sun
    graphics_fill_circle(ctx, GPoint(cx, cy), 4);
    for (int i = 0; i < 8; i++) {
      int32_t angle = TRIG_MAX_ANGLE * i / 8;
      int x1 = cx + 7  * cos_lookup(angle) / TRIG_MAX_RATIO;
      int y1 = cy - 7  * sin_lookup(angle) / TRIG_MAX_RATIO;
      int x2 = cx + 10 * cos_lookup(angle) / TRIG_MAX_RATIO;
      int y2 = cy - 10 * sin_lookup(angle) / TRIG_MAX_RATIO;
      graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
    }
  } else if (s_weather_code <= 3) {
    // Partly cloudy
    graphics_fill_circle(ctx, GPoint(cx + 4, cy - 4), 3);
    draw_cloud(ctx, cx - 1, cy + 1);
  } else if (s_weather_code <= 49) {
    // Overcast / fog
    draw_cloud(ctx, cx, cy);
  } else if (s_weather_code <= 69) {
    // Rain
    draw_cloud(ctx, cx, cy - 2);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(cx - 4, cy + 5), GPoint(cx - 5, cy + 8));
    graphics_draw_line(ctx, GPoint(cx,     cy + 5), GPoint(cx - 1, cy + 8));
    graphics_draw_line(ctx, GPoint(cx + 4, cy + 5), GPoint(cx + 3, cy + 8));
  } else if (s_weather_code <= 79) {
    // Snow
    draw_cloud(ctx, cx, cy - 2);
    graphics_fill_circle(ctx, GPoint(cx - 4, cy + 6), 1);
    graphics_fill_circle(ctx, GPoint(cx,     cy + 7), 1);
    graphics_fill_circle(ctx, GPoint(cx + 4, cy + 6), 1);
  } else {
    // Thunderstorm
    draw_cloud(ctx, cx, cy - 3);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(cx + 1, cy + 2), GPoint(cx - 2, cy + 5));
    graphics_draw_line(ctx, GPoint(cx - 2, cy + 5), GPoint(cx + 1, cy + 5));
    graphics_draw_line(ctx, GPoint(cx + 1, cy + 5), GPoint(cx - 2, cy + 9));
  }
}

// ---------------------------------------------------------------------------
// AppMessage – receive weather data from phone JS
// ---------------------------------------------------------------------------

static void apply_weather(int temp, int code) {
  s_weather_code = code;
  snprintf(s_temp_buf, sizeof(s_temp_buf), "%d\u00B0", temp);
  text_layer_set_text(s_temp_layer, s_temp_buf);
  if (s_weather_icon_layer) layer_mark_dirty(s_weather_icon_layer);
}

static void load_cached_weather(void) {
  if (!persist_exists(PERSIST_KEY_WEATHER_TIME)) return;

  int cached_time = persist_read_int(PERSIST_KEY_WEATHER_TIME);
  int now = (int)time(NULL);

  // Only use cache if less than 1 hour old
  if (now - cached_time > 3600) return;

  int temp = persist_read_int(PERSIST_KEY_TEMPERATURE);
  int code = persist_read_int(PERSIST_KEY_WEATHER_CODE);
  apply_weather(temp, code);
}

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *temp_tuple = dict_find(iter, MESSAGE_KEY_Temperature);
  Tuple *code_tuple = dict_find(iter, MESSAGE_KEY_WeatherCode);

  if (temp_tuple && code_tuple) {
    int temp = temp_tuple->value->int32;
    int code = code_tuple->value->int32;

    persist_write_int(PERSIST_KEY_TEMPERATURE, temp);
    persist_write_int(PERSIST_KEY_WEATHER_CODE, code);
    persist_write_int(PERSIST_KEY_WEATHER_TIME, (int)time(NULL));

    apply_weather(temp, code);
  }
}

// ---------------------------------------------------------------------------
// Window load / unload
// ---------------------------------------------------------------------------

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  int w = bounds.size.w;
  int h = bounds.size.h;

  s_time_font = fonts_load_custom_font(
      resource_get_handle(RESOURCE_ID_FONT_GOOGLESANSFLEX_68));
  s_info_font = fonts_load_custom_font(
      resource_get_handle(RESOURCE_ID_FONT_OUTFIT_18));

  // Layout: two big time rows, then a single info line at the bottom
  int info_h = 22;
  int bottom_margin = PBL_IF_ROUND_ELSE(10, 2);
  int y_info = h - bottom_margin - info_h;

  // Fill remaining height with time digits
  int time_avail = y_info;
  int time_h     = (time_avail + 24) / 2;  // overlap rows by 24px
  int gap        = time_avail - time_h * 2; // negative = overlap

  int y_hour = 0;
  int y_min  = time_h + gap;

  // Hour
  s_hour_layer = text_layer_create(GRect(0, y_hour, w, time_h));
  text_layer_set_background_color(s_hour_layer, GColorClear);
  text_layer_set_text_color(s_hour_layer, GColorWhite);
  text_layer_set_font(s_hour_layer, s_time_font);
  text_layer_set_text_alignment(s_hour_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_hour_layer));

  // Minute
  s_minute_layer = text_layer_create(GRect(0, y_min, w, time_h));
  text_layer_set_background_color(s_minute_layer, GColorClear);
  text_layer_set_text_color(s_minute_layer, GColorWhite);
  text_layer_set_font(s_minute_layer, s_time_font);
  text_layer_set_text_alignment(s_minute_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_minute_layer));

  // Info line: date left-aligned, weather+temp right-aligned
  int cal_sz      = 14;
  int weather_sz  = 20;
  int margin      = PBL_IF_ROUND_ELSE(20, 8);
  bool show_cal   = (w > 144);

  // Left side: [cal_icon] WED 25
  int lx = margin;

  if (show_cal) {
    s_calendar_icon_layer = layer_create(GRect(lx, y_info + 5, cal_sz, cal_sz));
    layer_set_update_proc(s_calendar_icon_layer, calendar_icon_draw);
    layer_add_child(root, s_calendar_icon_layer);
    lx += cal_sz + 4;
  } else {
    s_calendar_icon_layer = NULL;
  }

  s_date_layer = text_layer_create(GRect(lx, y_info, w / 2 - lx, info_h));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorWhite);
  text_layer_set_font(s_date_layer, s_info_font);
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentLeft);
  layer_add_child(root, text_layer_get_layer(s_date_layer));

  // Right side: 4° [weather_icon]  — icon on the very right
  int right_edge  = w - margin;
  int temp_text_w = 40;

  s_weather_icon_layer = layer_create(GRect(right_edge - weather_sz, y_info + 2, weather_sz, weather_sz));
  layer_set_update_proc(s_weather_icon_layer, weather_icon_draw);
  layer_add_child(root, s_weather_icon_layer);

  s_temp_layer = text_layer_create(GRect(right_edge - weather_sz - 2 - temp_text_w, y_info, temp_text_w, info_h));
  text_layer_set_background_color(s_temp_layer, GColorClear);
  text_layer_set_text_color(s_temp_layer, GColorWhite);
  text_layer_set_font(s_temp_layer, s_info_font);
  text_layer_set_text_alignment(s_temp_layer, GTextAlignmentRight);
  layer_add_child(root, text_layer_get_layer(s_temp_layer));

  update_time();
  load_cached_weather();
}

static void window_unload(Window *window) {
  text_layer_destroy(s_hour_layer);
  text_layer_destroy(s_minute_layer);
  text_layer_destroy(s_date_layer);
  text_layer_destroy(s_temp_layer);
  if (s_calendar_icon_layer) layer_destroy(s_calendar_icon_layer);
  if (s_weather_icon_layer) layer_destroy(s_weather_icon_layer);
  fonts_unload_custom_font(s_time_font);
  fonts_unload_custom_font(s_info_font);
}

// ---------------------------------------------------------------------------
// Init / deinit
// ---------------------------------------------------------------------------

static void init(void) {
  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(256, 64);

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load   = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
