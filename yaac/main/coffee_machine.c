#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "coffee_machine.h"

static const char *TAG = "machine";

#define BREW_SECONDS    25
#define MAINT_SECONDS   60

typedef struct {
    const char *name;
    int  base_coffee_ml;
    int  base_milk_s;
    bool milk;              /* MILK_DRINKS */
    bool no_coffee;         /* NO_COFFEE */
} drink_info_t;

static const drink_info_t DRINKS[] = {
    {"ristretto", 25, 0, false, false},   {"espresso", 40, 0, false, false},
    {"coffee", 100, 0, false, false},     {"lungo", 140, 0, false, false},
    {"americano", 180, 0, false, false},  {"cappuccino", 40, 5, true, false},
    {"latte_macchiato", 40, 5, true, false}, {"caffe_latte", 60, 5, true, false},
    {"flat_white", 60, 5, true, false},   {"hot_milk", 0, 5, true, true},
    {"hot_water", 200, 0, false, true},
};
#define N_DRINKS (sizeof(DRINKS) / sizeof(DRINKS[0]))

typedef struct {
    int  strength;
    char coffee_length[12];
    char milk_amount[8];
    char temperature[8];
} drink_settings_t;

static const cJSON *s_decls;
static drink_settings_t settings[N_DRINKS];
static cJSON *favorite;         /* args of the favourite drink */
static cJSON *last;             /* args of the last brew */
static int64_t busy_until_us;
static char busy_what[24];
static const int cups_since_descale = 412;

static const struct { const char *name; const char *needs; } MAINTENANCE[] = {
    {"rinse", "Only a cup under the coffee spout is needed. Takes about 30 seconds."},
    {"clean_milk_circuit", "A container under the milk spout and the milk tube in a jug of water are needed. About 1 minute."},
    {"descale", "Philips descaler, water tank filled to the marked level, a container of at least 1.5 litres "
                "and the AquaClean filter removed are needed. About 30 minutes."},
    {"replace_water_filter", "A new AquaClean filter to insert in the water tank is needed. The machine then runs a short rinse."},
};

/* ---- helpers ------------------------------------------------------------- */

static int drink_index(const char *name)
{
    for (size_t i = 0; i < N_DRINKS; i++) if (strcmp(DRINKS[i].name, name) == 0) return (int)i;
    return -1;
}

static double level_factor(const char *v, const char *const *names, const double *f, int n)
{
    for (int i = 0; i < n; i++) if (strcmp(v, names[i]) == 0) return f[i];
    return 1.0;
}

static double length_factor(const char *v)
{
    static const char *n[] = {"short", "normal", "long", "extra_long"};
    static const double f[] = {0.7, 1.0, 1.4, 1.8};
    return level_factor(v, n, f, 4);
}

static double milk_factor(const char *v)
{
    static const char *n[] = {"less", "normal", "more"};
    static const double f[] = {0.7, 1.0, 1.3};
    return level_factor(v, n, f, 3);
}

static int temp_c(const char *v)
{
    if (strcmp(v, "low") == 0) return 84;
    if (strcmp(v, "high") == 0) return 94;
    return 90;
}

static bool busy(void) { return esp_timer_get_time() < busy_until_us; }

static void set_busy(const char *what, int seconds)
{
    busy_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    snprintf(busy_what, sizeof(busy_what), "%s", what);
}

static void uart(const char *frame)
{
    ESP_LOGI(TAG, "[UART -> machine] %s", frame);
}

static cJSON *status(const char *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", s);
    return o;
}

static cJSON *busy_result(void)
{
    char reason[48];
    snprintf(reason, sizeof(reason), "%s in progress", busy_what);
    cJSON *o = status("busy");
    cJSON_AddStringToObject(o, "reason", reason);
    return o;
}

static cJSON *settings_json(int d)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "strength", settings[d].strength);
    cJSON_AddStringToObject(o, "coffee_length", settings[d].coffee_length);
    cJSON_AddStringToObject(o, "milk_amount", settings[d].milk_amount);
    cJSON_AddStringToObject(o, "temperature", settings[d].temperature);
    return o;
}

static bool arg_bool(const cJSON *a, const char *k)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, k));
}

/* Apply the setting keys present in a to s (skipping the ignored ones) */
static void apply_settings(drink_settings_t *s, const cJSON *a, const cJSON *ignored)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, a) {
        bool skip = false;
        const cJSON *ig;
        cJSON_ArrayForEach(ig, ignored) if (strcmp(ig->valuestring, it->string) == 0) skip = true;
        if (skip) continue;
        if (strcmp(it->string, "strength") == 0) s->strength = it->valueint;
        else if (strcmp(it->string, "coffee_length") == 0) snprintf(s->coffee_length, sizeof(s->coffee_length), "%s", it->valuestring);
        else if (strcmp(it->string, "milk_amount") == 0) snprintf(s->milk_amount, sizeof(s->milk_amount), "%s", it->valuestring);
        else if (strcmp(it->string, "temperature") == 0) snprintf(s->temperature, sizeof(s->temperature), "%s", it->valuestring);
    }
}

/* ---- validation (generic over the tools.json schema) --------------------- */

static cJSON *find_decl(const char *name)
{
    const cJSON *d;
    cJSON_ArrayForEach(d, s_decls) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(d, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) return (cJSON *)d;
    }
    return NULL;
}

/* Returns NULL when valid, otherwise a newly allocated rejection reason */
static char *validate(const cJSON *decl, const cJSON *args)
{
    char buf[96];
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(decl, "parameters");
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    const cJSON *a;
    cJSON_ArrayForEach(a, args) {
        if (!cJSON_GetObjectItemCaseSensitive(props, a->string)) {
            snprintf(buf, sizeof(buf), "parameter not allowed: %s", a->string);
            return strdup(buf);
        }
    }
    const cJSON *req;
    cJSON_ArrayForEach(req, cJSON_GetObjectItemCaseSensitive(schema, "required")) {
        if (!cJSON_GetObjectItemCaseSensitive(args, req->valuestring)) {
            snprintf(buf, sizeof(buf), "missing required parameter: %s", req->valuestring);
            return strdup(buf);
        }
    }
    cJSON_ArrayForEach(a, args) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(props, a->string);
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(p, "type");
        const char *type = cJSON_IsString(t) ? t->valuestring : "";
        if (strcmp(type, "STRING") == 0) {
            if (!cJSON_IsString(a)) {
                snprintf(buf, sizeof(buf), "%s: string expected", a->string);
                return strdup(buf);
            }
            const cJSON *en = cJSON_GetObjectItemCaseSensitive(p, "enum");
            if (en) {
                bool ok = false;
                const cJSON *e;
                cJSON_ArrayForEach(e, en) if (strcmp(e->valuestring, a->valuestring) == 0) ok = true;
                if (!ok) {
                    snprintf(buf, sizeof(buf), "%s: value not allowed '%s'", a->string, a->valuestring);
                    return strdup(buf);
                }
            }
        } else if (strcmp(type, "INTEGER") == 0) {
            if (!cJSON_IsNumber(a) || a->valuedouble != floor(a->valuedouble)) {
                snprintf(buf, sizeof(buf), "%s: integer expected", a->string);
                return strdup(buf);
            }
            const cJSON *r = cJSON_GetObjectItemCaseSensitive(p, "x-range");
            if (cJSON_GetArraySize(r) == 2) {
                const int lo = cJSON_GetArrayItem(r, 0)->valueint, hi = cJSON_GetArrayItem(r, 1)->valueint;
                if (a->valueint < lo || a->valueint > hi) {
                    snprintf(buf, sizeof(buf), "%s: out of range [%d, %d]", a->string, lo, hi);
                    return strdup(buf);
                }
            }
        } else if (strcmp(type, "BOOLEAN") == 0) {
            if (!cJSON_IsBool(a)) {
                snprintf(buf, sizeof(buf), "%s: boolean expected", a->string);
                return strdup(buf);
            }
        }
    }
    return NULL;
}

/* ---- functions ----------------------------------------------------------- */

static cJSON *fn_brew(const cJSON *a)
{
    if (busy()) return busy_result();
    const char *drink = cJSON_GetObjectItemCaseSensitive(a, "drink")->valuestring;
    const int d = drink_index(drink);
    cJSON *ignored = cJSON_CreateArray();
    if (!DRINKS[d].milk && cJSON_GetObjectItemCaseSensitive(a, "milk_amount")) {
        cJSON_AddItemToArray(ignored, cJSON_CreateString("milk_amount"));
    }
    if (DRINKS[d].no_coffee) {
        static const char *k[] = {"strength", "coffee_length", "extra_shot"};
        for (int i = 0; i < 3; i++) {
            if (cJSON_GetObjectItemCaseSensitive(a, k[i])) cJSON_AddItemToArray(ignored, cJSON_CreateString(k[i]));
        }
    }
    drink_settings_t s = settings[d];
    apply_settings(&s, a, ignored);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(a, "cups");
    const int cups = cJSON_IsNumber(c) ? c->valueint : 1;
    const int coffee_ml = (int)lround(DRINKS[d].base_coffee_ml * length_factor(s.coffee_length));
    const int milk_s = (int)lround(DRINKS[d].base_milk_s * milk_factor(s.milk_amount));
    bool extra_ignored = false;
    const cJSON *ig;
    cJSON_ArrayForEach(ig, ignored) if (strcmp(ig->valuestring, "extra_shot") == 0) extra_ignored = true;
    const int extra = arg_bool(a, "extra_shot") && !extra_ignored;

    char frame[160];
    snprintf(frame, sizeof(frame), "BREW %s aroma=%d coffee=%dml milk=%ds temp=%dC extra_shot=%d cups=%d",
             drink, s.strength, coffee_ml, milk_s, temp_c(s.temperature), extra, cups);
    uart(frame);
    cJSON_Delete(last);
    last = cJSON_Duplicate(a, true);
    set_busy("brewing", BREW_SECONDS * cups);

    cJSON *res = status("started");
    cJSON_AddStringToObject(res, "drink", drink);
    cJSON *applied = cJSON_CreateObject();
    cJSON_AddNumberToObject(applied, "strength", s.strength);
    cJSON_AddStringToObject(applied, "coffee_length", s.coffee_length);
    cJSON_AddStringToObject(applied, "milk_amount", s.milk_amount);
    cJSON_AddStringToObject(applied, "temperature", s.temperature);
    cJSON_AddItemToObject(res, "applied", applied);
    cJSON_AddNumberToObject(res, "cups", cups);
    cJSON_AddNumberToObject(res, "estimated_seconds", BREW_SECONDS * cups);
    if (cJSON_GetArraySize(ignored)) {
        cJSON_AddItemToObject(res, "ignored", ignored);
    } else {
        cJSON_Delete(ignored);
    }
    return res;
}

static cJSON *fn_brew_saved(const cJSON *a)
{
    const char *which = cJSON_GetObjectItemCaseSensitive(a, "which")->valuestring;
    const cJSON *src = strcmp(which, "favorite") == 0 ? favorite : last;
    if (!src) {
        cJSON *r = status("rejected");
        cJSON_AddStringToObject(r, "reason", "no previous drink in memory");
        return r;
    }
    cJSON *copy = cJSON_Duplicate(src, true);
    cJSON *r = fn_brew(copy);
    cJSON_Delete(copy);
    return r;
}

static cJSON *fn_stop(const cJSON *a)
{
    if (!busy()) return status("nothing_to_stop");
    busy_until_us = 0;
    uart("STOP");
    return status("stopped");
}

static cJSON *fn_get_drink_settings(const cJSON *a)
{
    const char *drink = cJSON_GetObjectItemCaseSensitive(a, "drink")->valuestring;
    const int d = drink_index(drink);
    cJSON *r = settings_json(d);
    cJSON_AddStringToObject(r, "drink", drink);
    const cJSON *fd = cJSON_GetObjectItemCaseSensitive(favorite, "drink");
    cJSON_AddBoolToObject(r, "is_favorite", cJSON_IsString(fd) && strcmp(fd->valuestring, drink) == 0);
    return r;
}

static cJSON *fn_save_drink_settings(const cJSON *a)
{
    const char *drink = cJSON_GetObjectItemCaseSensitive(a, "drink")->valuestring;
    const int d = drink_index(drink);
    cJSON *changes = cJSON_CreateObject();
    static const char *keys[] = {"strength", "coffee_length", "milk_amount", "temperature"};
    for (int i = 0; i < 4; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, keys[i]);
        if (v) cJSON_AddItemToObject(changes, keys[i], cJSON_Duplicate(v, true));
    }
    const bool as_fav = arg_bool(a, "as_favorite");
    if (!arg_bool(a, "confirmed")) {
        cJSON *r = status("needs_confirmation");
        cJSON_AddStringToObject(r, "drink", drink);
        cJSON_AddItemToObject(r, "will_change", changes);
        cJSON_AddBoolToObject(r, "as_favorite", as_fav);
        return r;
    }
    apply_settings(&settings[d], changes, NULL);
    if (as_fav) {
        cJSON_Delete(favorite);
        favorite = cJSON_Duplicate(changes, true);
        cJSON_AddStringToObject(favorite, "drink", drink);
    }
    char *c = cJSON_PrintUnformatted(changes);
    char frame[200];
    snprintf(frame, sizeof(frame), "SAVE %s %s favorite=%s", drink, c ? c : "{}", as_fav ? "True" : "False");
    cJSON_free(c);
    uart(frame);
    cJSON_Delete(changes);
    cJSON *r = status("saved");
    cJSON_AddStringToObject(r, "drink", drink);
    cJSON_AddItemToObject(r, "settings", settings_json(d));
    return r;
}

static cJSON *fn_get_machine_status(const cJSON *a)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "water_tank", "ok");
    cJSON_AddStringToObject(r, "beans", "ok");
    cJSON_AddStringToObject(r, "drip_tray", "ok");
    cJSON_AddStringToObject(r, "grounds_container", "70%");
    cJSON_AddStringToObject(r, "aquaclean_filter", "ok");
    cJSON_AddNumberToObject(r, "cups_until_descale", cups_since_descale < 500 ? 500 - cups_since_descale : 0);
    if (busy()) {
        cJSON_AddStringToObject(r, "busy", busy_what);
    } else {
        cJSON_AddNullToObject(r, "busy");
    }
    return r;
}

static cJSON *fn_start_maintenance(const cJSON *a)
{
    if (busy()) return busy_result();
    const char *proc = cJSON_GetObjectItemCaseSensitive(a, "procedure")->valuestring;
    if (!arg_bool(a, "confirmed")) {
        cJSON *r = status("needs_confirmation");
        cJSON_AddStringToObject(r, "procedure", proc);
        for (size_t i = 0; i < sizeof(MAINTENANCE) / sizeof(MAINTENANCE[0]); i++) {
            if (strcmp(MAINTENANCE[i].name, proc) == 0) cJSON_AddStringToObject(r, "user_must_prepare", MAINTENANCE[i].needs);
        }
        return r;
    }
    char frame[48];
    snprintf(frame, sizeof(frame), "MAINT %s", proc);
    uart(frame);
    set_busy(proc, MAINT_SECONDS);
    cJSON *r = status("started");
    cJSON_AddStringToObject(r, "procedure", proc);
    return r;
}

/* ---- API ----------------------------------------------------------------- */

void coffee_machine_init(const cJSON *decls)
{
    s_decls = decls;
    for (size_t i = 0; i < N_DRINKS; i++) {
        settings[i] = (drink_settings_t){3, "normal", "normal", "normal"};
    }
    favorite = cJSON_CreateObject();
    cJSON_AddStringToObject(favorite, "drink", "cappuccino");
    cJSON_AddNumberToObject(favorite, "strength", 4);
    ESP_LOGI(TAG, "Simulated machine ready (%d functions)", cJSON_GetArraySize(decls));
}

cJSON *coffee_machine_handle(const char *name, const cJSON *args)
{
    static const struct { const char *name; cJSON *(*fn)(const cJSON *); } FNS[] = {
        {"brew", fn_brew}, {"brew_saved", fn_brew_saved}, {"stop", fn_stop},
        {"get_drink_settings", fn_get_drink_settings}, {"save_drink_settings", fn_save_drink_settings},
        {"get_machine_status", fn_get_machine_status}, {"start_maintenance", fn_start_maintenance},
    };
    char buf[96];
    const cJSON *decl = name ? find_decl(name) : NULL;
    if (!decl) {
        snprintf(buf, sizeof(buf), "unknown function: %s", name ? name : "?");
        cJSON *r = status("rejected");
        cJSON_AddStringToObject(r, "reason", buf);
        return r;
    }
    cJSON *empty = NULL;
    if (!args) args = empty = cJSON_CreateObject();
    char *err = validate(decl, args);
    cJSON *r = NULL;
    if (err) {
        r = status("rejected");
        cJSON_AddStringToObject(r, "reason", err);
        free(err);
    } else {
        for (size_t i = 0; i < sizeof(FNS) / sizeof(FNS[0]); i++) {
            if (strcmp(FNS[i].name, name) == 0) r = FNS[i].fn(args);
        }
        if (!r) {
            snprintf(buf, sizeof(buf), "function not implemented on the device: %s", name);
            r = status("rejected");
            cJSON_AddStringToObject(r, "reason", buf);
        }
    }
    cJSON_Delete(empty);
    return r;
}
