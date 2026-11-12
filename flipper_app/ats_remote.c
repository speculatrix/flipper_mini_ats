// ATS-Mini Remote for Flipper Zero.
//
// Talks the ATS-Mini "ad hoc" remote protocol
// (https://esp32-si4732.github.io/ats-mini/remote.html) over the GPIO UART
// (pin 13 TX, pin 14 RX). The Flipper Wi-Fi devboard, running the bridge in
// ../wifi_bridge, forwards that to the receiver's TCP port 60000.

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/number_input.h>
#include <gui/modules/widget.h>
#include <expansion/expansion.h>
#include <notification/notification_messages.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "AtsRemote"

#define UART_BAUD 115200
#define LINE_MAX 160
#define STATUS_FIELDS 15
// The receiver prints a status line every 500 ms while its monitor log is on.
#define STATUS_STALE_MS 2500
#define MONITOR_RETRY_MS 3000
#define MESSAGE_SHOW_MS 3000

typedef enum {
    ViewIdMenu,
    ViewIdRemote,
    ViewIdCommands,
    ViewIdFrequency,
    ViewIdHelp,
} ViewId;

typedef enum {
    MenuRemote,
    MenuFrequency,
    MenuCommands,
    MenuHelp,
} MenuIndex;

typedef enum {
    LinkUnknown, // nothing heard from the bridge yet
    LinkDown, // bridge up, no TCP session
    LinkUp, // TCP session to the receiver
} LinkState;

typedef struct {
    bool valid;
    uint32_t freq_hz;
    bool is_fm;
    bool is_ssb;
    char band[12];
    char mode[4];
    char step[8];
    char bandwidth[8];
    uint16_t agc;
    uint16_t volume;
    uint16_t rssi;
    uint16_t snr;
    uint16_t centivolts;

    LinkState link;
    char link_text[32];
    char message[40];
    uint32_t message_tick;
} RemoteModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* menu;
    Submenu* commands;
    NumberInput* number_input;
    Widget* help;
    View* remote_view;
    NotificationApp* notifications;

    Expansion* expansion;
    bool otg_enabled_by_us;

    FuriHalSerialHandle* serial;
    FuriMutex* tx_mutex;
    FuriStreamBuffer* rx_stream;
    FuriThread* worker;
    volatile bool running;

    uint32_t last_status_tick;
    uint32_t last_monitor_tick;
    int32_t last_freq_khz;
} AtsRemoteApp;

// ---------------------------------------------------------------- UART ----

static void uart_send(AtsRemoteApp* app, const char* data) {
    furi_mutex_acquire(app->tx_mutex, FuriWaitForever);
    furi_hal_serial_tx(app->serial, (const uint8_t*)data, strlen(data));
    furi_hal_serial_tx_wait_complete(app->serial);
    furi_mutex_release(app->tx_mutex);
}

static void uart_send_char(AtsRemoteApp* app, char c) {
    char s[2] = {c, 0};
    uart_send(app, s);
}

static void uart_rx_callback(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    AtsRemoteApp* app = ctx;
    if(event & FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->rx_stream, &byte, 1, 0);
    }
}

// ---------------------------------------------------- protocol parsing ----

static void copy_field(char* dst, size_t size, const char* src) {
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

// "3.95" -> 395
static uint16_t parse_centi(const char* s) {
    uint32_t whole = strtoul(s, NULL, 10);
    uint32_t frac = 0;
    const char* dot = strchr(s, '.');
    if(dot) {
        if(dot[1] >= '0' && dot[1] <= '9') frac += (dot[1] - '0') * 10;
        if(dot[1] && dot[2] >= '0' && dot[2] <= '9') frac += dot[2] - '0';
    }
    return (uint16_t)(whole * 100 + frac);
}

// Status line (monitor log), 15 comma-separated fields:
// version, freq, bfo, cal, band, mode, step, bandwidth, agc, volume,
// rssi, snr, tuning cap, voltage, seq
static bool parse_status(char* line, RemoteModel* m) {
    char* f[STATUS_FIELDS];
    size_t n = 0;
    f[n++] = line;
    for(char* p = line; *p; p++) {
        if(*p == ',') {
            if(n == STATUS_FIELDS) return false;
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    if(n != STATUS_FIELDS) return false;

    copy_field(m->mode, sizeof(m->mode), f[5]);
    m->is_fm = strcmp(m->mode, "FM") == 0;
    m->is_ssb = strcmp(m->mode, "USB") == 0 || strcmp(m->mode, "LSB") == 0;

    // Frequency is in 10 kHz units for FM, 1 kHz units otherwise; the BFO
    // (Hz) gives the fine SSB offset.
    uint32_t raw = strtoul(f[1], NULL, 10);
    int32_t bfo = strtol(f[2], NULL, 10);
    m->freq_hz = raw * (m->is_fm ? 10000 : 1000) + bfo;

    copy_field(m->band, sizeof(m->band), f[4]);
    copy_field(m->step, sizeof(m->step), f[6]);
    copy_field(m->bandwidth, sizeof(m->bandwidth), f[7]);
    m->agc = strtoul(f[8], NULL, 10);
    m->volume = strtoul(f[9], NULL, 10);
    m->rssi = strtoul(f[10], NULL, 10);
    m->snr = strtoul(f[11], NULL, 10);
    m->centivolts = parse_centi(f[13]);
    m->valid = true;
    return true;
}

static void set_message(RemoteModel* m, const char* text) {
    copy_field(m->message, sizeof(m->message), text);
    m->message_tick = furi_get_tick();
}

static void handle_line(AtsRemoteApp* app, char* line) {
    // Skip leading spaces (the receiver prints " Ok").
    while(*line == ' ')
        line++;
    if(!*line) return;

    if(line[0] == '~') {
        const char* s = line + 1;
        bool connected = strcmp(s, "TCP:CONNECTED") == 0;
        with_view_model(
            app->remote_view,
            RemoteModel * m,
            {
                m->link = connected ? LinkUp : LinkDown;
                if(!connected) m->valid = false;
                copy_field(m->link_text, sizeof(m->link_text), s);
            },
            true);
        if(connected) {
            // A new TCP session always starts with the monitor log off.
            app->last_status_tick = 0;
            app->last_monitor_tick = 0;
        }
        return;
    }

    char tmp[LINE_MAX];
    copy_field(tmp, sizeof(tmp), line);
    bool is_status = false;
    with_view_model(
        app->remote_view,
        RemoteModel * m,
        {
            is_status = parse_status(tmp, m);
            if(is_status) {
                m->link = LinkUp;
            } else {
                set_message(m, line);
            }
        },
        true);
    if(is_status) app->last_status_tick = furi_get_tick();
}

static int32_t worker_thread(void* ctx) {
    AtsRemoteApp* app = ctx;
    char line[LINE_MAX];
    size_t len = 0;
    bool overflow = false;

    while(app->running) {
        uint8_t buf[64];
        size_t n = furi_stream_buffer_receive(app->rx_stream, buf, sizeof(buf), 50);
        for(size_t i = 0; i < n; i++) {
            char c = (char)buf[i];
            if(c == '\r' || c == '\n') {
                if(len && !overflow) {
                    line[len] = '\0';
                    handle_line(app, line);
                }
                len = 0;
                overflow = false;
            } else if(len < LINE_MAX - 1) {
                line[len++] = c;
            } else {
                overflow = true; // e.g. screenshot data; drop the line
            }
        }

        // Keep the monitor log running. 't' toggles it, so only send it when
        // no status line has arrived for a while (i.e. it is off).
        uint32_t now = furi_get_tick();
        LinkState link;
        with_view_model(app->remote_view, RemoteModel * m, { link = m->link; }, false);
        if(link != LinkDown && now - app->last_status_tick > STATUS_STALE_MS &&
           (app->last_monitor_tick == 0 || now - app->last_monitor_tick > MONITOR_RETRY_MS)) {
            app->last_monitor_tick = now;
            uart_send_char(app, 't');
            with_view_model(app->remote_view, RemoteModel * m, { m->valid = false; }, true);
        }
    }
    return 0;
}

// --------------------------------------------------------- remote view ----

static void format_frequency(const RemoteModel* m, char* out, size_t size, const char** unit) {
    if(m->is_fm) {
        snprintf(
            out,
            size,
            "%lu.%02lu",
            (unsigned long)(m->freq_hz / 1000000),
            (unsigned long)((m->freq_hz / 10000) % 100));
        *unit = "MHz";
    } else if(m->is_ssb) {
        snprintf(
            out,
            size,
            "%lu.%03lu",
            (unsigned long)(m->freq_hz / 1000),
            (unsigned long)(m->freq_hz % 1000));
        *unit = "kHz";
    } else {
        snprintf(out, size, "%lu", (unsigned long)(m->freq_hz / 1000));
        *unit = "kHz";
    }
}

static void remote_draw_callback(Canvas* canvas, void* model) {
    RemoteModel* m = model;
    char buf[40];
    canvas_clear(canvas);

    bool show_message = m->message[0] && furi_get_tick() - m->message_tick < MESSAGE_SHOW_MS;
    const char* footer = show_message ? m->message :
                         m->link_text[0] ? m->link_text :
                                           "Waiting for Wi-Fi board";

    if(!m->valid) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 14, AlignCenter, AlignBottom, "ATS-Mini Remote");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas,
            64,
            32,
            AlignCenter,
            AlignBottom,
            m->link == LinkUp ? "Waiting for status..." : "Not connected");
        canvas_draw_str_aligned(canvas, 64, 46, AlignCenter, AlignBottom, footer);
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, "Back: menu");
        return;
    }

    // Top row: band, mode, volume
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 8, m->band);
    canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignBottom, m->mode);
    snprintf(buf, sizeof(buf), "Vol %u", m->volume);
    canvas_draw_str_aligned(canvas, 128, 8, AlignRight, AlignBottom, buf);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    // Frequency
    const char* unit;
    format_frequency(m, buf, sizeof(buf), &unit);
    canvas_set_font(canvas, FontBigNumbers);
    uint16_t fw = canvas_string_width(canvas, buf);
    canvas_set_font(canvas, FontSecondary);
    uint16_t uw = canvas_string_width(canvas, unit);
    if(fw + 3 + uw <= 128) {
        int x = (128 - (fw + 3 + uw)) / 2;
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str(canvas, x, 30, buf);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, x + fw + 3, 30, unit);
    } else {
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignBottom, buf);
    }

    // Details
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "Stp %s  BW %s  AGC %u", m->step, m->bandwidth, m->agc);
    canvas_draw_str(canvas, 0, 42, buf);
    snprintf(
        buf,
        sizeof(buf),
        "RSSI %u  SNR %u  %u.%02uV",
        m->rssi,
        m->snr,
        m->centivolts / 100,
        m->centivolts % 100);
    canvas_draw_str(canvas, 0, 52, buf);

    // Signal bar (RSSI 0..127 dBuV, scaled so ~64 fills it)
    uint16_t bar = m->rssi > 64 ? 64 : m->rssi;
    canvas_draw_frame(canvas, 0, 55, 128, 3);
    canvas_draw_box(canvas, 0, 55, bar * 2, 3);

    canvas_draw_str(canvas, 0, 64, show_message ? m->message : "^v tune  <> vol  OK clk");
}

static bool remote_input_callback(InputEvent* event, void* ctx) {
    AtsRemoteApp* app = ctx;
    char cmd = 0;

    switch(event->key) {
    case InputKeyUp:
        if(event->type == InputTypeShort || event->type == InputTypeRepeat) cmd = 'R';
        break;
    case InputKeyDown:
        if(event->type == InputTypeShort || event->type == InputTypeRepeat) cmd = 'r';
        break;
    case InputKeyLeft:
        if(event->type == InputTypeShort || event->type == InputTypeRepeat) cmd = 'v';
        break;
    case InputKeyRight:
        if(event->type == InputTypeShort || event->type == InputTypeRepeat) cmd = 'V';
        break;
    case InputKeyOk:
        if(event->type == InputTypeShort) cmd = 'e';
        if(event->type == InputTypeLong) cmd = 'E';
        break;
    default:
        return false; // Back -> previous view
    }

    if(cmd) uart_send_char(app, cmd);
    return true;
}

// Redraw periodically so the message timeout clears.
static void remote_timer_callback(void* ctx) {
    AtsRemoteApp* app = ctx;
    with_view_model(app->remote_view, RemoteModel * m, { UNUSED(m); }, true);
}

// ----------------------------------------------------------- commands ----

typedef struct {
    const char* label;
    char cmd;
} CommandItem;

static const CommandItem command_items[] = {
    {"Band next", 'B'},
    {"Band previous", 'b'},
    {"Mode next", 'M'},
    {"Mode previous", 'm'},
    {"Step next", 'S'},
    {"Step previous", 's'},
    {"Bandwidth next", 'W'},
    {"Bandwidth previous", 'w'},
    {"AGC/Att up", 'A'},
    {"AGC/Att down", 'a'},
    {"Backlight up", 'L'},
    {"Backlight down", 'l'},
    {"Calibration up", 'I'},
    {"Calibration down", 'i'},
    {"Sleep on", 'O'},
    {"Sleep off", 'o'},
    {"Encoder click", 'e'},
    {"Encoder press", 'E'},
};

static void commands_callback(void* ctx, uint32_t index) {
    AtsRemoteApp* app = ctx;
    if(index < COUNT_OF(command_items)) {
        uart_send_char(app, command_items[index].cmd);
        notification_message(app->notifications, &sequence_blink_blue_10);
    }
}

// ---------------------------------------------------------- frequency ----

static void frequency_callback(void* ctx, int32_t khz) {
    AtsRemoteApp* app = ctx;
    char cmd[24];
    app->last_freq_khz = khz;
    snprintf(cmd, sizeof(cmd), "F%lu000\r\n", (unsigned long)khz);
    uart_send(app, cmd);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdRemote);
}

// --------------------------------------------------------------- menu ----

static void menu_callback(void* ctx, uint32_t index) {
    AtsRemoteApp* app = ctx;
    switch(index) {
    case MenuRemote:
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdRemote);
        break;
    case MenuFrequency:
        number_input_set_header_text(app->number_input, "Frequency (kHz)");
        number_input_set_result_callback(
            app->number_input, frequency_callback, app, app->last_freq_khz, 150, 108000);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdFrequency);
        break;
    case MenuCommands:
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdCommands);
        break;
    case MenuHelp:
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdHelp);
        break;
    }
}

static uint32_t nav_to_menu(void* ctx) {
    UNUSED(ctx);
    return ViewIdMenu;
}

static uint32_t nav_exit(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

static const char help_text[] =
    "Plug in the Wi-Fi devboard\n"
    "running the ATS bridge.\n\n"
    "On the ATS-Mini set:\n"
    "Settings > Wi-Fi > AP Only\n"
    "Settings > TCP Port > Ad hoc\n\n"
    "Remote screen:\n"
    "Up/Down: tune\n"
    "Left/Right: volume\n"
    "OK: click, hold OK: press\n"
    "Back: menu\n\n"
    "Set Frequency must be in\n"
    "the current band.";

// ---------------------------------------------------------- lifecycle ----

static AtsRemoteApp* app_alloc(void) {
    AtsRemoteApp* app = malloc(sizeof(AtsRemoteApp));
    memset(app, 0, sizeof(AtsRemoteApp));
    app->last_freq_khz = 7200;

    // Free the GPIO UART from the expansion module protocol and power the
    // Wi-Fi devboard from the 5V pin.
    app->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(app->expansion);
    if(!furi_hal_power_is_otg_enabled()) {
        furi_hal_power_enable_otg();
        app->otg_enabled_by_us = true;
    }

    app->tx_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->rx_stream = furi_stream_buffer_alloc(2048, 1);

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Main menu
    app->menu = submenu_alloc();
    submenu_set_header(app->menu, "ATS-Mini Remote");
    submenu_add_item(app->menu, "Remote", MenuRemote, menu_callback, app);
    submenu_add_item(app->menu, "Set Frequency", MenuFrequency, menu_callback, app);
    submenu_add_item(app->menu, "Commands", MenuCommands, menu_callback, app);
    submenu_add_item(app->menu, "Help", MenuHelp, menu_callback, app);
    view_set_previous_callback(submenu_get_view(app->menu), nav_exit);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdMenu, submenu_get_view(app->menu));

    // Remote view
    app->remote_view = view_alloc();
    view_allocate_model(app->remote_view, ViewModelTypeLocking, sizeof(RemoteModel));
    view_set_context(app->remote_view, app);
    view_set_draw_callback(app->remote_view, remote_draw_callback);
    view_set_input_callback(app->remote_view, remote_input_callback);
    view_set_previous_callback(app->remote_view, nav_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdRemote, app->remote_view);

    // Commands
    app->commands = submenu_alloc();
    submenu_set_header(app->commands, "Commands");
    for(size_t i = 0; i < COUNT_OF(command_items); i++) {
        submenu_add_item(app->commands, command_items[i].label, i, commands_callback, app);
    }
    view_set_previous_callback(submenu_get_view(app->commands), nav_to_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdCommands, submenu_get_view(app->commands));

    // Frequency input
    app->number_input = number_input_alloc();
    view_set_previous_callback(number_input_get_view(app->number_input), nav_to_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdFrequency, number_input_get_view(app->number_input));

    // Help
    app->help = widget_alloc();
    widget_add_text_scroll_element(app->help, 0, 0, 128, 64, help_text);
    view_set_previous_callback(widget_get_view(app->help), nav_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdHelp, widget_get_view(app->help));

    // UART
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->serial) {
        furi_hal_serial_init(app->serial, UART_BAUD);
        furi_hal_serial_async_rx_start(app->serial, uart_rx_callback, app, false);
    } else {
        FURI_LOG_E(TAG, "USART busy");
        with_view_model(
            app->remote_view,
            RemoteModel * m,
            { copy_field(m->link_text, sizeof(m->link_text), "UART busy!"); },
            false);
    }

    app->running = true;
    app->worker = furi_thread_alloc_ex("AtsRemoteWorker", 2048, worker_thread, app);
    if(app->serial) furi_thread_start(app->worker);

    return app;
}

static void app_free(AtsRemoteApp* app) {
    app->running = false;
    if(app->serial) {
        furi_thread_join(app->worker);

        // Leave the receiver's monitor log off if we were receiving it.
        if(app->last_status_tick &&
           furi_get_tick() - app->last_status_tick < STATUS_STALE_MS) {
            uart_send_char(app, 't');
        }
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }
    furi_thread_free(app->worker);

    view_dispatcher_remove_view(app->view_dispatcher, ViewIdMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdRemote);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdCommands);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdFrequency);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdHelp);
    submenu_free(app->menu);
    submenu_free(app->commands);
    number_input_free(app->number_input);
    widget_free(app->help);
    view_free(app->remote_view);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    furi_stream_buffer_free(app->rx_stream);
    furi_mutex_free(app->tx_mutex);

    if(app->otg_enabled_by_us) furi_hal_power_disable_otg();
    expansion_enable(app->expansion);
    furi_record_close(RECORD_EXPANSION);

    free(app);
}

int32_t ats_remote_app(void* p) {
    UNUSED(p);
    AtsRemoteApp* app = app_alloc();

    FuriTimer* timer = furi_timer_alloc(remote_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(timer, 500);

    view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdMenu);
    view_dispatcher_run(app->view_dispatcher);

    furi_timer_stop(timer);
    furi_timer_free(timer);
    app_free(app);
    return 0;
}
