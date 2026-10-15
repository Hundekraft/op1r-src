/* Sound loading safety net. A sound that fails parse_patch_json (0x01A037A8) ends in an assert: patch.cpp:390
 * "Invalid synth patch <type>:<name> - error N" (synth slot loader 0x01A051E8) or patch.cpp:221 (drum, 0x01A04BB0),
 * and the OP-1 locks at every boot while the sound is stored. Seen with an FX the parser does not know (error 22).
 * On a failure the FX and LFO blocks are reset to stock defaults, switched off (delay and element, their
 * op1_factory.db default_params), and the patch is parsed again. A sound that still fails asserts as before. */
#include <stdint.h>

#define PARSE_PATCH ((int (*)(void *, int))0x01A037A8)
#define JV_KEY      ((void *(*)(void *, const char *))0x019BFEB4)        /* Value::operator[](const char *) */
#define JV_IDX      ((void *(*)(void *, unsigned))0x019BFAF8)            /* Value::operator[](ArrayIndex) */
#define JV_ASSIGN   ((void *(*)(void *, const void *))0x019BDDC0)        /* Value::operator= */
#define JV_FROM_INT ((void *(*)(void *, int))0x019BD890)                 /* Value(int) */
#define JV_FROM_BOOL ((void *(*)(void *, int))0x019BD832)                /* Value(bool) (parser: fx_active) */
#define JV_FROM_STR ((void *(*)(void *, const char *))0x019BDC4C)        /* Value(const char *) */
#define JV_DTOR     ((void (*)(void *, int))0x019BDD18)
#define JV_REMOVE   ((void (*)(void *, void *, const char *))0x019BEE88) /* removeMember(key) -> out */
enum { JV_OBJECT = 7 };

static void set_ints(void *obj, const char *key, const int16_t *v)
{
    uint32_t t[4];                                     /* a Json::Value (16 bytes) */
    JV_REMOVE(obj, t, key); JV_DTOR(t, 2);             /* whatever it was: the null member below becomes an array */
    void *a = JV_KEY(obj, key);
    for (unsigned i = 0; i < 8; i++) {
        JV_FROM_INT(t, v[i]);                          /* an int Value owns no heap memory */
        JV_ASSIGN(JV_IDX(a, i), t);
    }
}

static void set_block(void *obj, const char *type, const char *name, const char *active, const char *params,
                      const int16_t *v)
{
    uint32_t t[4];
    JV_FROM_STR(t, name); JV_ASSIGN(JV_KEY(obj, type), t); JV_DTOR(t, 2);
    JV_FROM_BOOL(t, 0); JV_ASSIGN(JV_KEY(obj, active), t);
    set_ints(obj, params, v);
}

/* returns 1 if the patch was changed (then parse it again) */
int patch_reset_fx_lfo(void *json)
{
    static const int16_t fx_delay[8] = { 8000, 8000, 8000, 8000, 0, 0, 0, 0 };
    static const int16_t lfo_element[8] = { 2000, 4000, 2000, 2000, 0, 0, 0, 0 };
    if (((const uint8_t *)json)[8] != JV_OBJECT) return 0;
    set_block(json, "fx_type", "delay", "fx_active", "fx_params", fx_delay);
    set_block(json, "lfo_type", "element", "lfo_active", "lfo_params", lfo_element);
    return 1;
}

int synth_load_parse(void *json, int mode)
{
    int r = PARSE_PATCH(json, mode);
    if (r && patch_reset_fx_lfo(json)) r = PARSE_PATCH(json, mode);
    return r;
}
