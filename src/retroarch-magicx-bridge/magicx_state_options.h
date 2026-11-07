/* SPDX-License-Identifier: GPL-3.0-or-later
 * Private helpers included by the pinned RetroArch binding. Default-option
 * checkpoints remain byte-for-byte RASTATE1. Changed options are carried in a
 * bounded, authenticated state suffix and restored before deserialization. */
#ifndef MAGICX_STATE_OPTIONS_H
#define MAGICX_STATE_OPTIONS_H
#define MX_OPTIONS_LIMIT (512U * 1024U)
#define MX_OPTIONS_FOOTER 16U
static config_file_t *factory_options(void)
{
    core_option_manager_t *options = runloop_state_get_ptr()->core_options;
    const char *path = getenv("MAGICX_FACTORY_OPTIONS");
    if (path == NULL || path[0] == '\0') path = options != NULL ? options->conf_path : "";
    return path[0] == '/' ? config_file_new_from_path_to_string(path) : NULL;
}
static bool factory_option_index(const struct core_option *option, config_file_t *conf, size_t *index)
{
    char value[513];
    if (option->key == NULL || option->vals == NULL || option->default_index >= option->vals->size)
        return false;
    *index = option->default_index;
    if (conf != NULL && config_get_array(conf, option->key, value, sizeof(value))) {
        for (size_t i = 0U; i < option->vals->size; ++i)
            if (strcmp(value, option->vals->elems[i].data) == 0) { *index = i; return true; }
        return false;
    }
    return true;
}
static void options_put_u32(unsigned char *p, uint32_t value)
{ for (unsigned int i = 0U; i < 4U; ++i) p[i] = (unsigned char)(value >> (8U * i)); }
static bool options_snapshot(struct magicx_blob *state, size_t maximum)
{
    core_option_manager_t *options = runloop_state_get_ptr()->core_options;
    config_file_t *conf = factory_options();
    size_t bytes = 4U, count = options != NULL ? options->size : 0U;
    bool changed = false, valid = count <= 512U;
    for (size_t i = 0U; i < count && valid; ++i) {
        const struct core_option *option = &options->opts[i];
        const char *value = core_option_manager_get_val(options, option->opt_idx);
        size_t factory;
        valid = option->key != NULL && value != NULL && strlen(option->key) > 0U &&
            strlen(option->key) <= 256U && strlen(value) <= 512U &&
            factory_option_index(option, conf, &factory);
        if (!valid) break;
        if (option->index != factory) changed = true;
        bytes += 4U + strlen(option->key) + strlen(value);
    }
    if (conf != NULL) config_file_free(conf);
    if (!valid || bytes > MX_OPTIONS_LIMIT) return false;
    if (!changed) return true;
    if (state->size > maximum || bytes + MX_OPTIONS_FOOTER > maximum - state->size) return false;
    unsigned char *data = realloc(state->data, state->size + bytes + MX_OPTIONS_FOOTER);
    if (data == NULL) return false;
    state->data = data;
    unsigned char *cursor = data + state->size;
    options_put_u32(cursor, (uint32_t)count); cursor += 4U;
    for (size_t i = 0U; i < count; ++i) {
        const struct core_option *option = &options->opts[i];
        const char *value = core_option_manager_get_val(options, option->opt_idx);
        size_t key_bytes = strlen(option->key), value_bytes = strlen(value);
        cursor[0] = (unsigned char)key_bytes; cursor[1] = (unsigned char)(key_bytes >> 8U);
        cursor[2] = (unsigned char)value_bytes; cursor[3] = (unsigned char)(value_bytes >> 8U);
        cursor += 4U; memcpy(cursor, option->key, key_bytes); cursor += key_bytes;
        memcpy(cursor, value, value_bytes); cursor += value_bytes;
    }
    memcpy(cursor, "CLOVEOPT", 8U); options_put_u32(cursor + 8U, (uint32_t)bytes);
    options_put_u32(cursor + 12U, 1U);
    state->size += bytes + MX_OPTIONS_FOOTER;
    return true;
}
/* Parse all keys, values, bounds and the original RASTATE before mutating any
 * option. Exact enum values from the loaded core are the only accepted input. */
static bool options_restore(struct magicx_blob *state)
{
    core_option_manager_t *options = runloop_state_get_ptr()->core_options;
    const size_t count = options != NULL ? options->size : 0U;
    size_t indices[512]; bool seen[512] = {false};
    struct magicx_blob native = *state;
    if (count > 512U || state->data == NULL) return false;
    const unsigned char *data = state->data;
    const unsigned char *footer = state->size >= MX_OPTIONS_FOOTER ? data + state->size - MX_OPTIONS_FOOTER : NULL;
    if (footer != NULL && memcmp(footer, "CLOVEOPT", 8U) == 0) {
        size_t bytes = little_u32(footer + 8U);
        if (little_u32(footer + 12U) != 1U || bytes < 4U || bytes > MX_OPTIONS_LIMIT ||
            bytes > state->size - MX_OPTIONS_FOOTER) return false;
        const unsigned char *cursor = footer - bytes;
        native.size -= bytes + MX_OPTIONS_FOOTER;
        if (little_u32(cursor) != count) return false;
        cursor += 4U;
        for (size_t n = 0U; n < count; ++n) {
            if ((size_t)(footer - cursor) < 4U) return false;
            size_t key_bytes = (size_t)cursor[0] + ((size_t)cursor[1] << 8U);
            size_t value_bytes = (size_t)cursor[2] + ((size_t)cursor[3] << 8U);
            cursor += 4U;
            if (key_bytes == 0U || key_bytes > 256U || value_bytes > 512U ||
                key_bytes + value_bytes > (size_t)(footer - cursor) ||
                memchr(cursor, '\0', key_bytes + value_bytes) != NULL) return false;
            bool found = false;
            for (size_t i = 0U; i < count && !found; ++i) {
                const struct core_option *option = &options->opts[i];
                if (option->key == NULL || strlen(option->key) != key_bytes ||
                    memcmp(option->key, cursor, key_bytes) != 0) continue;
                if (seen[i] || option->vals == NULL) return false;
                for (size_t v = 0U; v < option->vals->size; ++v) {
                    const char *value = option->vals->elems[v].data;
                    if (strlen(value) == value_bytes && memcmp(value, cursor + key_bytes, value_bytes) == 0) {
                        indices[i] = v; seen[i] = true; found = true; break;
                    }
                }
            }
            if (!found) return false;
            cursor += key_bytes + value_bytes;
        }
        if (cursor != footer) return false;
    } else {
        config_file_t *conf = factory_options();
        bool valid = true;
        for (size_t i = 0U; i < count && valid; ++i)
            valid = factory_option_index(&options->opts[i], conf, &indices[i]);
        if (conf != NULL) config_file_free(conf);
        if (!valid) return false;
    }
    if (!safe_rastate(&native)) return false;
    for (size_t i = 0U; i < count; ++i)
        if (options->opts[i].index != indices[i])
            core_option_manager_set_val(options, options->opts[i].opt_idx, indices[i], false);
    *state = native;
    return true;
}
#endif
