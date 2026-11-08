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
#define OUTPUT_FILE IR_DIR "/WK_RC05_responses.ir"
#define RESULTS_FILE IR_DIR "/WK_RC05_results.csv"
#define PROGRESS_FILE IR_DIR "/WK_RC05_progress.txt"

typedef enum {
    ViewIdText = 0,
    ViewIdTrial,
    ViewIdPopup,
} ViewId;

typedef enum {
    TextModeRemoteName,
    TextModeButtonName,
} TextMode;

typedef enum { StageScan, StageReview, StageDone, StageError } Stage;
static const char* effects[] = {
    "Unknown", "Mode", "Power", "Brighter", "Dimmer",
    "Faster", "Slower", "Timer", "Other"
};
#define EFFECT_COUNT 9
/* View model for the Trial screen */
typedef struct {
    size_t index;
    size_t count;
    FuriString* name;   /* name of the currently-selected source code */
    FuriString* remote; /* name of the new remote being built */
    Stage stage;
    size_t saved, review_pos;
    bool paused;
    char effect[20];
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
    FuriTimer* timer;
    Stage stage;
    bool paused, have_sent;
    size_t scan_index, last_sent, saved, review_pos;
    uint16_t* marks;
    uint8_t* labels;
} App;

/* Forward declarations */
static void __attribute__((unused)) text_input_done(void* context);
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


static bool persist(App* app) {
    FlipperFormat* f=flipper_format_file_alloc(app->storage);
    bool ok=flipper_format_file_open_always(f,PROGRESS_FILE);
    if(ok) ok=flipper_format_write_header_cstr(f,"WKRC05 progress",1);
    uint32_t next=app->scan_index,count=app->count,saved=app->saved;
    if(ok) ok=flipper_format_write_uint32(f,"next",&next,1);
    if(ok) ok=flipper_format_write_uint32(f,"total",&count,1);
    if(ok) ok=flipper_format_write_uint32(f,"saved",&saved,1);
    for(size_t i=0;i<app->saved && ok;i++) {
        char key[20]; snprintf(key,sizeof(key),"r%u",(unsigned)i);
        uint32_t p[2]={app->marks[i],app->labels[i]};
        ok=flipper_format_write_uint32(f,key,p,2);
    }
    flipper_format_free(f); return ok;
}
static void restore(App* app) {
    FlipperFormat* f=flipper_format_file_alloc(app->storage);
    if(!flipper_format_file_open_existing(f,PROGRESS_FILE)) {
        flipper_format_free(f); return;
    }
    uint32_t next=0,total=0,saved=0;
    if(flipper_format_read_uint32(f,"next",&next,1) &&
       flipper_format_read_uint32(f,"total",&total,1) &&
       flipper_format_read_uint32(f,"saved",&saved,1) &&
       total==app->count && next<=total && saved<=total) {
        app->scan_index=next;
        for(size_t i=0;i<saved;i++) {
            char key[20];snprintf(key,sizeof(key),"r%u",(unsigned)i);
            uint32_t p[2]={0,0};
            if(!flipper_format_read_uint32(f,key,p,2) || p[0]>=total) break;
            app->marks[app->saved]=(uint16_t)p[0];
            app->labels[app->saved++]=p[1]<EFFECT_COUNT?p[1]:0;
        }
    }
    flipper_format_free(f);
}
static bool export_all(App* app) {
    File* f=storage_file_alloc(app->storage);
    bool ok=storage_file_open(f,RESULTS_FILE,FSAM_WRITE,FSOM_CREATE_ALWAYS);
    const char* hdr="signal_id,effect\n";
    if(ok) ok=storage_file_write(f,hdr,strlen(hdr))==strlen(hdr);
    for(size_t i=0;i<app->saved && ok;i++) {
        char line[64];
        int n=snprintf(line,sizeof(line),"%04u,%s\n",
            (unsigned)app->marks[i]+1,effects[app->labels[i]]);
        ok=storage_file_write(f,line,n)==(size_t)n;
    }
    storage_file_close(f);storage_file_free(f);
    if(!ok) return false;
    storage_common_remove(app->storage,OUTPUT_FILE);
    furi_string_set_str(app->output_path,OUTPUT_FILE);
    for(size_t i=0;i<app->saved;i++) {
        app->index=app->marks[i];
        char name[64];
        snprintf(name,sizeof(name),"%s_%04u",effects[app->labels[i]],
                 (unsigned)app->index+1);
        if(!save_current(app,name)) return false;
    }
    return true;
}
static void refresh_trial(App* app) {
    with_view_model(app->trial_view,TrialModel *m,{
        m->stage=app->stage;
        m->index=app->stage==StageScan?
            (app->have_sent?app->last_sent:app->scan_index):
            (app->saved?app->marks[app->review_pos]:0);
        m->count=app->count;
        m->saved=app->saved;
        m->review_pos=app->review_pos;
        m->paused=app->paused;
        if(m->index<app->count) furi_string_set(m->name,app->names[m->index]);
        else furi_string_set_str(m->name,"(none)");
        furi_string_set(m->remote,app->remote_name);
        snprintf(m->effect,sizeof(m->effect),"%s",
            app->saved?effects[app->labels[app->review_pos]]:"Unknown");
    },true);
}
static void review(App* app) {
    if(!app->saved) return;
    app->stage=StageReview;app->paused=true;app->review_pos=0;
    persist(app);refresh_trial(app);
}
static void timer_callback(void* context) {
    App* app=context;
    view_dispatcher_send_custom_event(app->view_dispatcher,1);
}
static bool custom_event(void* context,uint32_t event) {
    App* app=context;
    if(event!=1 || app->stage!=StageScan || app->paused) return true;
    if(app->scan_index>=app->count) {
        if(app->saved) review(app);
        else {app->stage=StageDone;refresh_trial(app);}
        return true;
    }
    app->index=app->scan_index;
    transmit_current(app);
    app->last_sent=app->scan_index++;
    app->have_sent=true;
    if(app->scan_index%16==0) persist(app);
    refresh_trial(app);return true;
}

/* ------------------------------------------------------------------ */
/* Trial view: draw + input                                           */
/* ------------------------------------------------------------------ */


static void trial_draw(Canvas* canvas,void* model) {
    TrialModel* m=model;canvas_clear(canvas);
    canvas_set_font(canvas,FontPrimary);
    canvas_draw_str(canvas,2,11,"WK-RC05 AUTO TRAINER");
    canvas_set_font(canvas,FontSecondary);
    char l[64];
    if(m->stage==StageScan) {
        snprintf(l,sizeof(l),"Code %u/%u Saved %u",
            (unsigned)m->index+1,(unsigned)m->count,(unsigned)m->saved);
        canvas_draw_str(canvas,2,24,l);
        snprintf(l,sizeof(l),"%.22s",furi_string_get_cstr(m->name));
        canvas_draw_str(canvas,2,36,l);
        canvas_draw_str(canvas,2,49,m->paused?"PAUSED (Up=resume)":"Send every 3 seconds");
        canvas_draw_str(canvas,2,62,"OK=mark Rt=review");
    } else if(m->stage==StageReview) {
        snprintf(l,sizeof(l),"Review %u/%u IR %04u",
            (unsigned)m->review_pos+1,(unsigned)m->saved,(unsigned)m->index+1);
        canvas_draw_str(canvas,2,24,l);
        snprintf(l,sizeof(l),"Effect: %s",m->effect);
        canvas_draw_str(canvas,2,38,l);
        canvas_draw_str(canvas,2,51,"Up/Dn=label Rt=send");
        canvas_draw_str(canvas,2,62,"OK=next/export");
    } else {
        canvas_draw_str(canvas,2,28,m->stage==StageDone?"Finished.":"Export failed");
        canvas_draw_str(canvas,2,42,"See /infrared/ SD");
        canvas_draw_str(canvas,2,62,"Back=exit");
    }
}
static bool trial_input(InputEvent* e,void* context) {
    App* app=context;
    if(e->type!=InputTypeShort) return false;
    if(app->stage==StageScan) {
        if(e->key==InputKeyOk && app->have_sent) {
            bool exists=false;
            for(size_t i=0;i<app->saved;i++) if(app->marks[i]==app->last_sent) exists=true;
            if(!exists && app->saved<MAX_SIGNALS) {
                app->marks[app->saved]=(uint16_t)app->last_sent;
                app->labels[app->saved++]=0;
                persist(app);
            }
        } else if(e->key==InputKeyUp) app->paused=!app->paused;
        else if(e->key==InputKeyDown && app->have_sent) {
            app->index=app->last_sent;transmit_current(app);
        } else if(e->key==InputKeyRight) review(app);
        else return false;
    } else if(app->stage==StageReview) {
        size_t p=app->review_pos;
        if(e->key==InputKeyUp)
            app->labels[p]=(app->labels[p]+EFFECT_COUNT-1)%EFFECT_COUNT;
        else if(e->key==InputKeyDown)
            app->labels[p]=(app->labels[p]+1)%EFFECT_COUNT;
        else if(e->key==InputKeyRight) {
            app->index=app->marks[p];transmit_current(app);
        } else if(e->key==InputKeyLeft) {
            if(p) app->review_pos--;
        } else if(e->key==InputKeyOk) {
            if(p+1<app->saved) app->review_pos++;
            else app->stage=export_all(app)?StageDone:StageError;
        } else return false;
        persist(app);
    } else return false;
    refresh_trial(app);return true;
}

/* ------------------------------------------------------------------ */
/* Text input + popup callbacks                                       */
/* ------------------------------------------------------------------ */

static void popup_timeout_cb(void* context) {
    App* app = context;
    go_view(app, ViewIdTrial);
}

static void __attribute__((unused)) text_input_done(void* context) {
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
    App* app=context;
    if(app->stage==StageReview) {
        app->stage=StageScan;app->paused=true;app->have_sent=false;
        refresh_trial(app);return true;
    }
    if(app->stage==StageScan && app->saved) {review(app);return true;}
    return false;
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
    app->marks=calloc(MAX_SIGNALS,sizeof(uint16_t));
    app->labels=calloc(MAX_SIGNALS,sizeof(uint8_t));
    app->stage=StageScan;

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher,custom_event);
    app->timer=furi_timer_alloc(timer_callback,FuriTimerTypePeriodic,app);
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
            m->stage=StageScan;m->saved=0;m->review_pos=0;
            m->paused=false;m->effect[0]=0;
        },
        true);

    view_dispatcher_add_view(app->view_dispatcher, ViewIdText, text_input_get_view(app->text_input));
    view_dispatcher_add_view(app->view_dispatcher, ViewIdPopup, popup_get_view(app->popup));
    view_dispatcher_add_view(app->view_dispatcher, ViewIdTrial, app->trial_view);

    return app;
}

static void app_free(App* app) {
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    free(app->marks);free(app->labels);
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
    App* app=app_alloc();
    furi_string_set_str(app->remote_name,"WK-RC05");
    furi_string_set_str(app->output_path,OUTPUT_FILE);
    FuriString* start=furi_string_alloc_set_str(IR_DIR);
    DialogsFileBrowserOptions opts;
    dialog_file_browser_set_basic_options(&opts,".ir",NULL);
    opts.base_path=IR_DIR;opts.skip_assets=false;opts.hide_dot_files=true;
    bool picked=dialog_file_browser_show(app->dialogs,app->source_path,start,&opts);
    furi_string_free(start);
    if(picked) {
        scan_names(app);
        if(app->count && app->marks && app->labels) {
            restore(app);refresh_trial(app);
            view_dispatcher_attach_to_gui(
                app->view_dispatcher,app->gui,ViewDispatcherTypeFullscreen);
            go_view(app,ViewIdTrial);
            furi_timer_start(app->timer,furi_ms_to_ticks(3000));
            view_dispatcher_run(app->view_dispatcher);
            persist(app);
        }
    }
    app_free(app);return 0;
}
