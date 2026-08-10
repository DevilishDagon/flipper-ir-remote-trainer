/*
 * IR Remote Trainer  (Flipper Zero / Momentum firmware)
 *
 * Purpose:
 *   Point the app at ANY .ir database file (e.g. the universal fan database at
 *   /ext/infrared/assets/fans.ir, or any file from Flipper-IRDB). Step through the
 *   codes ONE AT A TIME, press OK to transmit the current one, and when your
 *   device reacts, press Right to SAVE that single code into a brand-new remote
 *   file with a button name you choose ("Speed up", "Power", etc.).
 *
 *   The result is a normal .ir remote that opens like any other saved remote.
 *
 * Controls (on the Trial screen):
 *   Up / Down .... previous / next code
 *   Left ......... jump back 10 codes
 *   OK ........... TEST (transmit) the current code
 *   Right ........ SAVE the current code to your new remote (asks for a name)
 *   Back ......... exit
 *
 * Only firmware-exported (public) APIs are used, so it builds as an external
 * .fap: flipper_format for reading/writing .ir files, and infrared_send /
 * infrared_send_raw_ext for transmitting.
 */

#include <furi.h>
#include <furi_hal.h>
#include <string.h>
#include <stdio.h>

#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <gui/modules/popup.h>

#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#include <infrared.h>
#include <infrared_transmit.h>

#define TAG          "IrTrainer"
#define IR_DIR       EXT_PATH("infrared")
#define MAX_TIMINGS  1024
#define NAME_MAX     64
#define MAX_SIGNALS  4096

typedef enum {
    ViewIdText = 0,
    ViewIdTrial,
    ViewIdPopup,
} ViewId;

typedef enum {
    TextModeRemoteName,
    TextModeButtonName,
} TextMode;

/* View model for the Trial screen */
typedef struct {
    size_t index;
    size_t count;
    FuriString* name;   /* name of the currently-selected source code */
    FuriString* remote; /* name of the new remote being built */
} TrialModel;

/* A single decoded IR signal (either parsed or raw) */
typedef struct {
    bool is_raw;
    /* parsed */
    InfraredProtocol protocol;
    uint8_t address[4];
    uint8_t command[4];
    uint8_t address_len;
    uint8_t command_len;
    /* raw */
    uint32_t frequency;
    float duty_cycle;
    uint32_t timings[MAX_TIMINGS];
    size_t timings_size;
} ParsedSignal;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    DialogsApp* dialogs;
    Storage* storage;

    TextInput* text_input;
    Popup* popup;
    View* trial_view;

    FuriString* source_path; /* selected .ir database */
    FuriString* output_path; /* new remote file path  */
    FuriString* remote_name; /* new remote display name */

    FuriString** names; /* cached names of every code in the source file */
    size_t count;
    size_t capacity;
    size_t index;

    ViewId current_view;
    TextMode text_mode;
    char text_buf[NAME_MAX];
} App;

/* Forward declarations */
static void text_input_done(void* context);
static void popup_timeout_cb(void* context);

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static uint32_t bytes_to_u32_le(const uint8_t* b, uint8_t len) {
    uint32_t v = 0;
    for(uint8_t i = 0; i < len && i < 4; i++) {
        v |= ((uint32_t)b[i]) << (8 * i);
    }
    return v;
}

static void go_view(App* app, ViewId id) {
    app->current_view = id;
    view_dispatcher_switch_to_view(app->view_dispatcher, id);
}

/* Read every code name from the source file into app->names. */
static void scan_names(App* app) {
    app->count = 0;
    app->index = 0;
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    FuriString* tmp = furi_string_alloc();

    if(flipper_format_file_open_existing(ff, furi_string_get_cstr(app->source_path))) {
        flipper_format_rewind(ff);
        while(flipper_format_read_string(ff, "name", tmp)) {
            if(app->count >= MAX_SIGNALS) break;
            if(app->count == app->capacity) {
                size_t newcap = app->capacity ? app->capacity * 2 : 32;
                FuriString** grown =
                    realloc(app->names, sizeof(FuriString*) * newcap); //-V701
                if(!grown) break;
                app->names = grown;
                app->capacity = newcap;
            }
            app->names[app->count++] = furi_string_alloc_set(tmp);
        }
    }

    furi_string_free(tmp);
    flipper_format_free(ff);
}

/* Parse the code at the given index (re-reads the file each time). */
static bool parse_signal_at(App* app, size_t index, ParsedSignal* out) {
    memset(out, 0, sizeof(ParsedSignal));
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    FuriString* tmp = furi_string_alloc();
    FuriString* type = furi_string_alloc();
    FuriString* proto = furi_string_alloc();
    bool ok = false;

    do {
        if(!flipper_format_file_open_existing(ff, furi_string_get_cstr(app->source_path))) break;
        if(!flipper_format_rewind(ff)) break;

        /* advance to the (index)-th "name" key */
        bool found = true;
        for(size_t k = 0; k <= index; k++) {
            if(!flipper_format_read_string(ff, "name", tmp)) {
                found = false;
                break;
            }
        }
        if(!found) break;

        if(!flipper_format_read_string(ff, "type", type)) break;

        if(furi_string_equal_str(type, "raw")) {
            out->is_raw = true;
            uint32_t freq = 0;
            float duty = 0.0f;
            uint32_t tcount = 0;
            if(!flipper_format_read_uint32(ff, "frequency", &freq, 1)) break;
            if(!flipper_format_read_float(ff, "duty_cycle", &duty, 1)) break;
            if(!flipper_format_get_value_count(ff, "data", &tcount)) break;
            if(tcount == 0 || tcount > MAX_TIMINGS) break;
            if(!flipper_format_read_uint32(ff, "data", out->timings, tcount)) break;
            out->frequency = freq;
            out->duty_cycle = duty;
            out->timings_size = tcount;
            ok = true;
        } else {
            out->is_raw = false;
            if(!flipper_format_read_string(ff, "protocol", proto)) break;
            InfraredProtocol p = infrared_get_protocol_by_name(furi_string_get_cstr(proto));
            if(!infrared_is_protocol_valid(p)) break;
            uint8_t alen = infrared_get_protocol_address_length(p);
            uint8_t clen = infrared_get_protocol_command_length(p);
            if(alen > 4 || clen > 4 || alen == 0 || clen == 0) break;
            if(!flipper_format_read_hex(ff, "address", out->address, alen)) break;
            if(!flipper_format_read_hex(ff, "command", out->command, clen)) break;
            out->protocol = p;
            out->address_len = alen;
            out->command_len = clen;
            ok = true;
        }
    } while(false);

    furi_string_free(tmp);
    furi_string_free(type);
    furi_string_free(proto);
    flipper_format_free(ff);
    return ok;
}

/* Transmit the currently-selected code. */
static void transmit_current(App* app) {
    if(app->count == 0) return;
    /* ParsedSignal is large (~4KB timings buffer): keep it OFF the stack. */
    ParsedSignal* ps = malloc(sizeof(ParsedSignal));
    if(!ps) return;

    if(parse_signal_at(app, app->index, ps)) {
        if(ps->is_raw) {
            infrared_send_raw_ext(
                ps->timings, ps->timings_size, true, ps->frequency, ps->duty_cycle);
        } else {
            InfraredMessage msg;
            msg.protocol = ps->protocol;
            msg.address = bytes_to_u32_le(ps->address, ps->address_len);
            msg.command = bytes_to_u32_le(ps->command, ps->command_len);
            msg.repeat = false;
            /* send a couple of frames to mimic a short button press */
            infrared_send(&msg, 2);
        }
    }
    free(ps);
}

/* Append the currently-selected code to the output remote, under `name`. */
static bool save_current(App* app, const char* name) {
    if(app->count == 0) return false;
    /* ParsedSignal is large (~4KB timings buffer): keep it OFF the stack. */
    ParsedSignal* ps = malloc(sizeof(ParsedSignal));
    if(!ps) return false;
    if(!parse_signal_at(app, app->index, ps)) {
        free(ps);
        return false;
    }

    storage_simply_mkdir(app->storage, IR_DIR);
    const char* path = furi_string_get_cstr(app->output_path);
    bool exists = storage_common_exists(app->storage, path);

    FlipperFormat* off = flipper_format_file_alloc(app->storage);
    bool ok = false;

    do {
        if(exists) {
            if(!flipper_format_file_open_append(off, path)) break;
        } else {
            if(!flipper_format_file_open_always(off, path)) break;
            if(!flipper_format_write_header_cstr(off, "IR signals file", 1)) break;
        }

        flipper_format_write_comment_cstr(off, "");
        if(!flipper_format_write_string_cstr(off, "name", name)) break;

        if(ps->is_raw) {
            if(!flipper_format_write_string_cstr(off, "type", "raw")) break;
            if(!flipper_format_write_uint32(off, "frequency", &ps->frequency, 1)) break;
            if(!flipper_format_write_float(off, "duty_cycle", &ps->duty_cycle, 1)) break;
            if(!flipper_format_write_uint32(off, "data", ps->timings, ps->timings_size)) break;
        } else {
            if(!flipper_format_write_string_cstr(off, "type", "parsed")) break;
            if(!flipper_format_write_string_cstr(
                   off, "protocol", infrared_get_protocol_name(ps->protocol)))
                break;
            if(!flipper_format_write_hex(off, "address", ps->address, ps->address_len)) break;
            if(!flipper_format_write_hex(off, "command", ps->command, ps->command_len)) break;
        }
        ok = true;
    } while(false);

    flipper_format_file_close(off);
    flipper_format_free(off);
    free(ps);
    return ok;
}

static void refresh_trial(App* app) {
    with_view_model(
        app->trial_view,
        TrialModel * m,
        {
            m->index = app->index;
            m->count = app->count;
            if(app->count > 0) {
                furi_string_set(m->name, app->names[app->index]);
            } else {
                furi_string_set_str(m->name, "(none)");
            }
            furi_string_set(m->remote, app->remote_name);
        },
        true);
}

/* ------------------------------------------------------------------ */
/* Trial view: draw + input                                           */
/* ------------------------------------------------------------------ */

static void trial_draw(Canvas* canvas, void* model) {
    TrialModel* m = model;
    char line[48];

    canvas_set_font(canvas, FontPrimary);
    snprintf(line, sizeof(line), "New: %s", furi_string_get_cstr(m->remote));
    canvas_draw_str(canvas, 2, 11, line);
    canvas_draw_line(canvas, 0, 14, 128, 14);

    canvas_set_font(canvas, FontSecondary);
    snprintf(line, sizeof(line), "Code %u / %u", (unsigned)(m->index + 1), (unsigned)m->count);
    canvas_draw_str(canvas, 2, 26, line);

    canvas_set_font(canvas, FontPrimary);
    snprintf(line, sizeof(line), "%s", furi_string_get_cstr(m->name));
    canvas_draw_str(canvas, 2, 40, line);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 53, "OK=Test  Up/Dn=Move");
    canvas_draw_str(canvas, 2, 63, "Right=Save   Back=Exit");
}

static bool trial_input(InputEvent* event, void* context) {
    App* app = context;

    if(event->type != InputTypeShort && event->type != InputTypeRepeat) {
        return false;
    }

    switch(event->key) {
    case InputKeyUp:
        if(app->index > 0) {
            app->index--;
            refresh_trial(app);
        }
        return true;
    case InputKeyDown:
        if(app->index + 1 < app->count) {
            app->index++;
            refresh_trial(app);
        }
        return true;
    case InputKeyLeft:
        if(app->index > 10) {
            app->index -= 10;
        } else {
            app->index = 0;
        }
        refresh_trial(app);
        return true;
    case InputKeyOk:
        transmit_current(app);
        return true;
    case InputKeyRight:
        /* start the save flow: name this button (prefill with source name) */
        app->text_mode = TextModeButtonName;
        if(app->count > 0) {
            strncpy(app->text_buf, furi_string_get_cstr(app->names[app->index]), NAME_MAX - 1);
        } else {
            strncpy(app->text_buf, "Button", NAME_MAX - 1);
        }
        app->text_buf[NAME_MAX - 1] = '\0';
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "Name this button");
        text_input_set_result_callback(
            app->text_input, text_input_done, app, app->text_buf, NAME_MAX, false);
        go_view(app, ViewIdText);
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Text input + popup callbacks                                       */
/* ------------------------------------------------------------------ */

static void popup_timeout_cb(void* context) {
    App* app = context;
    go_view(app, ViewIdTrial);
}

static void text_input_done(void* context) {
    App* app = context;

    if(app->text_mode == TextModeRemoteName) {
        if(strlen(app->text_buf) == 0) {
            strncpy(app->text_buf, "Fan", NAME_MAX - 1);
        }
        furi_string_set_str(app->remote_name, app->text_buf);
        furi_string_printf(app->output_path, "%s/%s.ir", IR_DIR, app->text_buf);
        refresh_trial(app);
        go_view(app, ViewIdTrial);
    } else {
        bool ok = save_current(app, app->text_buf);
        popup_reset(app->popup);
        if(ok) {
            popup_set_header(app->popup, "Saved!", 64, 12, AlignCenter, AlignTop);
            char msg[80];
            snprintf(
                msg,
                sizeof(msg),
                "%s\nadded to remote",
                app->text_buf);
            popup_set_text(app->popup, msg, 64, 36, AlignCenter, AlignCenter);
        } else {
            popup_set_header(app->popup, "Save failed", 64, 12, AlignCenter, AlignTop);
            popup_set_text(
                app->popup, "Could not write file", 64, 36, AlignCenter, AlignCenter);
        }
        popup_set_context(app->popup, app);
        popup_set_callback(app->popup, popup_timeout_cb);
        popup_set_timeout(app->popup, 1400);
        popup_enable_timeout(app->popup);
        go_view(app, ViewIdPopup);
    }
}

static bool app_navigation(void* context) {
    App* app = context;
    switch(app->current_view) {
    case ViewIdText:
        if(app->text_mode == TextModeButtonName) {
            go_view(app, ViewIdTrial);
            return true; /* consumed: cancel naming, back to trial */
        }
        return false; /* remote-name screen: exit app */
    case ViewIdPopup:
        go_view(app, ViewIdTrial);
        return true;
    case ViewIdTrial:
    default:
        return false; /* exit app */
    }
}

/* ------------------------------------------------------------------ */
/* Alloc / free                                                       */
/* ------------------------------------------------------------------ */

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    app->gui = furi_record_open(RECORD_GUI);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->storage = furi_record_open(RECORD_STORAGE);

    app->source_path = furi_string_alloc();
    app->output_path = furi_string_alloc();
    app->remote_name = furi_string_alloc_set(app->source_path); /* placeholder, empty */
    furi_string_set_str(app->remote_name, "Fan");

    app->names = NULL;
    app->capacity = 0;
    app->count = 0;
    app->index = 0;

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, app_navigation);

    app->text_input = text_input_alloc();
    app->popup = popup_alloc();

    app->trial_view = view_alloc();
    view_set_context(app->trial_view, app);
    view_set_draw_callback(app->trial_view, trial_draw);
    view_set_input_callback(app->trial_view, trial_input);
    view_allocate_model(app->trial_view, ViewModelTypeLocking, sizeof(TrialModel));
    with_view_model(
        app->trial_view,
        TrialModel * m,
        {
            m->name = furi_string_alloc();
            m->remote = furi_string_alloc();
            m->index = 0;
            m->count = 0;
        },
        true);

    view_dispatcher_add_view(app->view_dispatcher, ViewIdText, text_input_get_view(app->text_input));
    view_dispatcher_add_view(app->view_dispatcher, ViewIdPopup, popup_get_view(app->popup));
    view_dispatcher_add_view(app->view_dispatcher, ViewIdTrial, app->trial_view);

    return app;
}

static void app_free(App* app) {
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdText);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdPopup);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdTrial);

    with_view_model(
        app->trial_view,
        TrialModel * m,
        {
            furi_string_free(m->name);
            furi_string_free(m->remote);
        },
        false);
    view_free(app->trial_view);

    text_input_free(app->text_input);
    popup_free(app->popup);
    view_dispatcher_free(app->view_dispatcher);

    if(app->names) {
        for(size_t i = 0; i < app->count; i++) {
            furi_string_free(app->names[i]);
        }
        free(app->names);
    }

    furi_string_free(app->source_path);
    furi_string_free(app->output_path);
    furi_string_free(app->remote_name);

    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_GUI);
    free(app);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int32_t ir_trainer_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();

    /* 1) Pick the source .ir database file. */
    FuriString* start = furi_string_alloc();
    furi_string_set_str(start, IR_DIR);

    DialogsFileBrowserOptions opts;
    dialog_file_browser_set_basic_options(&opts, ".ir", NULL);
    opts.base_path = IR_DIR;
    opts.skip_assets = false; /* the universal fans.ir lives under /infrared/assets */
    opts.hide_dot_files = true;

    bool picked = dialog_file_browser_show(app->dialogs, app->source_path, start, &opts);
    furi_string_free(start);

    if(picked) {
        scan_names(app);

        if(app->count == 0) {
            DialogMessage* dm = dialog_message_alloc();
            dialog_message_set_header(
                dm, "IR Remote Trainer", 64, 4, AlignCenter, AlignTop);
            dialog_message_set_text(
                dm, "No IR codes found\nin that file.", 64, 32, AlignCenter, AlignCenter);
            dialog_message_set_buttons(dm, NULL, "OK", NULL);
            dialog_message_show(app->dialogs, dm);
            dialog_message_free(dm);
        } else {
            /* 2) Name the new remote (first text-input screen). */
            app->text_mode = TextModeRemoteName;
            strncpy(app->text_buf, "Fan", NAME_MAX - 1);
            app->text_buf[NAME_MAX - 1] = '\0';
            text_input_reset(app->text_input);
            text_input_set_header_text(app->text_input, "Name the new remote");
            text_input_set_result_callback(
                app->text_input, text_input_done, app, app->text_buf, NAME_MAX, false);

            view_dispatcher_attach_to_gui(
                app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
            go_view(app, ViewIdText);
            view_dispatcher_run(app->view_dispatcher);
        }
    }

    app_free(app);
    return 0;
}
