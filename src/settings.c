// Copyright © 2026 dancingmirrors
// SPDX-License-Identifier: MIT

#include "howl.h"

#define GROUP "Howl"
#define SAVE_DELAY_MS 500

static char *last_written;
static guint save_source;

static const char *const sort_names[] = {"name", "size", "type", "modified"};

const char *
howl_settings_canonical_sort(const char *name) {
    for (gsize i = 0; i < G_N_ELEMENTS(sort_names); i++) {
        if (g_strcmp0(name, sort_names[i]) == 0) {
            return sort_names[i];
        }
    }
    return sort_names[0];
}

static char *
settings_path(void) {
    return g_build_filename(g_get_user_config_dir(), "howl.rc", NULL);
}

static int
read_int(GKeyFile *kf, const char *key, int fallback, int min, int max) {
    GError *error = NULL;
    int value = g_key_file_get_integer(kf, GROUP, key, &error);

    if (error != NULL) {
        g_error_free(error);
        return fallback;
    }
    return CLAMP(value, min, max);
}

static gboolean
read_bool(GKeyFile *kf, const char *key, gboolean fallback) {
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean(kf, GROUP, key, &error);

    if (error != NULL) {
        g_error_free(error);
        return fallback;
    }
    return value;
}

static char *
serialize(void) {
    GKeyFile *kf = g_key_file_new();
    char *text;

    g_key_file_set_integer(kf, GROUP, "icon-size", howl_opts.grid_size);
    g_key_file_set_integer(kf, GROUP, "list-icon-size", howl_opts.list_size);
    g_key_file_set_string(kf, GROUP, "view", howl_opts.list_view ? "list" : "grid");
    g_key_file_set_string(kf, GROUP, "sort-by", howl_opts.sort_by);
    g_key_file_set_boolean(kf, GROUP, "sort-reverse", howl_opts.sort_reverse);
    g_key_file_set_boolean(kf, GROUP, "show-hidden", howl_opts.show_hidden);
    g_key_file_set_boolean(kf, GROUP, "show-sidebar", howl_opts.show_sidebar);
    g_key_file_set_integer(kf, GROUP, "window-width", howl_opts.window_width);
    g_key_file_set_integer(kf, GROUP, "window-height", howl_opts.window_height);
    g_key_file_set_boolean(kf, GROUP, "maximized", howl_opts.maximized);

    text = g_key_file_to_data(kf, NULL, NULL);
    g_key_file_unref(kf);
    return text;
}

void howl_settings_load(void) {
    GKeyFile *kf = g_key_file_new();
    char *path = settings_path();

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        char *view = g_key_file_get_string(kf, GROUP, "view", NULL);
        char *sort = g_key_file_get_string(kf, GROUP, "sort-by", NULL);

        howl_opts.grid_size = read_int(kf, "icon-size", howl_opts.grid_size, 16, 512);
        howl_opts.list_size = read_int(kf, "list-icon-size", howl_opts.list_size, 16, 512);
        if (view != NULL) {
            howl_opts.list_view = g_str_equal(view, "list");
        }
        if (sort != NULL) {
            howl_opts.sort_by = howl_settings_canonical_sort(sort);
        }
        howl_opts.sort_reverse = read_bool(kf, "sort-reverse", howl_opts.sort_reverse);
        howl_opts.show_hidden = read_bool(kf, "show-hidden", howl_opts.show_hidden);
        howl_opts.show_sidebar = read_bool(kf, "show-sidebar", howl_opts.show_sidebar);
        howl_opts.window_width = read_int(kf, "window-width", howl_opts.window_width, 200, 16384);
        howl_opts.window_height = read_int(kf, "window-height", howl_opts.window_height, 150, 16384);
        howl_opts.maximized = read_bool(kf, "maximized", howl_opts.maximized);
        g_free(view);
        g_free(sort);
    }

    g_key_file_unref(kf);
    g_free(path);
}

void howl_settings_baseline(void) {
    g_free(last_written);
    last_written = serialize();
}

static gboolean
save_now(gpointer data) {
    char *text = serialize();

    save_source = 0;
    if (g_strcmp0(text, last_written) != 0) {
        char *path = settings_path();
        char *dir = g_path_get_dirname(path);
        GError *error = NULL;

        g_mkdir_with_parents(dir, 0700);
        if (!g_file_set_contents(path, text, -1, &error)) {
            g_warning("could not save %s: %s", path, error->message);
            g_error_free(error);
        }
        g_free(dir);
        g_free(path);
        g_free(last_written);
        last_written = text;
    } else {
        g_free(text);
    }
    return G_SOURCE_REMOVE;
}

void howl_settings_save(void) {
    if (save_source == 0) {
        save_source = g_timeout_add(SAVE_DELAY_MS, save_now, NULL);
    }
}

void howl_settings_flush(void) {
    if (save_source != 0) {
        g_source_remove(save_source);
        save_now(NULL);
    }
}
