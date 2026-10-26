// Copyright © 2026 dancingmirrors
// SPDX-License-Identifier: MIT

#include "howl.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

HowlOptions howl_opts = {TRUE, TRUE, FALSE, TRUE, 96, 32, "name", FALSE, 960, 620, FALSE};
gint64 howl_start_time;

static GMainLoop *loop;
static int n_windows;
static int n_holds;

static void
maybe_quit(void) {
    g_debug("%d windows, %d jobs", n_windows, n_holds);
    if (n_windows <= 0 && n_holds <= 0 && loop != NULL) {
        g_main_loop_quit(loop);
    }
}

void howl_app_window_opened(void) {
    n_windows++;
}

void howl_app_window_closed(void) {
    n_windows--;
    maybe_quit();
}

void howl_app_hold(void) {
    n_holds++;
}

void howl_app_release(void) {
    n_holds--;
    maybe_quit();
}

static int signal_pipe[2] = {-1, -1};

static void
signal_handler(int signum) {
    int saved_errno = errno;
    char byte = (char)signum;

    if (write(signal_pipe[1], &byte, 1) < 0) {
    }
    errno = saved_errno;
}

static gboolean
signal_pipe_ready(int fd, GIOCondition condition, gpointer data) {
    char byte;

    while (read(fd, &byte, 1) == 1) {
    }
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    if (loop != NULL) {
        g_main_loop_quit(loop);
    }
    return G_SOURCE_REMOVE;
}

static void
install_signal_handlers(void) {
    static const int signums[] = {SIGINT, SIGTERM, SIGHUP};
    struct sigaction action;

    if (pipe(signal_pipe) != 0) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(signal_pipe[i], F_SETFL, fcntl(signal_pipe[i], F_GETFL) | O_NONBLOCK);
        fcntl(signal_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    g_unix_fd_add(signal_pipe[0], G_IO_IN, signal_pipe_ready, NULL);

    memset(&action, 0, sizeof action);
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND | SA_RESTART;
    for (gsize i = 0; i < G_N_ELEMENTS(signums); i++) {
        sigaction(signums[i], &action, NULL);
    }
}

static const char css[] =
    "headerbar.howl-header > windowhandle > box { border-spacing: 0; }\n"
    ".howl-pathbar button { padding-left: 7px; padding-right: 7px; min-height: 28px; }\n"
    ".howl-pathbar button.howl-pathbar-current { font-weight: bold; }\n"
    ".howl-pathbar-separator { opacity: 0.45; margin: 0 -3px; }\n"
    ".howl-floating-bar { background-color: @theme_bg_color; border: 1px solid @borders;"
    "  border-bottom-width: 0; border-right-width: 0; border-radius: 8px 0 0 0; padding: 3px 12px; }\n"
    ".howl-empty { font-size: 1.8em; font-weight: bold; opacity: 0.55; }\n"
    ".howl-message { opacity: 0.7; }\n"
    ".howl-error { color: @error_color; }\n"
    "gridview.howl-grid > child { padding: 4px; }\n"
    ".howl-drop-target { outline: 2px solid @theme_selected_bg_color; outline-offset: -2px; border-radius: 6px; }\n"
    ".howl-pathbar button:drop(active) { color: inherit; border-color: transparent; box-shadow: none; }\n"
    ".howl-drag-icon { background-color: @theme_bg_color; border: 1px solid @borders;"
    "  border-radius: 8px; padding: 6px 10px; }\n"
    ".howl-drag-badge { background-color: @theme_selected_bg_color; color: @theme_selected_fg_color;"
    "  border-radius: 99px; padding: 0 6px; margin: 2px; font-weight: bold; font-size: 0.85em; }\n";

static void
load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();

#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(provider, css);
#else
    gtk_css_provider_load_from_data(provider, css, -1);
#endif
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static gboolean
open_argument(const char *arg) {
    GFile *file = g_file_new_for_commandline_arg(arg);
    GFileType type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);
    gboolean ok = TRUE;

    if (type == G_FILE_TYPE_DIRECTORY) {
        howl_window_new(file, NULL);
    } else if (type == G_FILE_TYPE_UNKNOWN) {
        g_printerr("howl: %s: no such file or directory\n", arg);
        ok = FALSE;
    } else {
        GFile *parent = g_file_get_parent(file);
        char *name = g_file_get_basename(file);

        if (parent != NULL) {
            howl_window_new(parent, name);
            g_object_unref(parent);
        }
        g_free(name);
    }
    g_object_unref(file);
    return ok;
}

int main(int argc, char **argv) {
    gboolean version = FALSE, no_hidden = FALSE, no_thumbnails = FALSE, list = FALSE, grid = FALSE;
    int icon_size = 0, list_icon_size = 0;
    char **paths = NULL;
    GOptionEntry entries[] = {
        {"version", 'v', 0, G_OPTION_ARG_NONE, &version, "Print the version and exit", NULL},
        {"no-hidden", 0, 0, G_OPTION_ARG_NONE, &no_hidden, "Start with hidden files hidden", NULL},
        {"no-thumbnails", 0, 0, G_OPTION_ARG_NONE, &no_thumbnails, "Never generate thumbnails", NULL},
        {"list", 'l', 0, G_OPTION_ARG_NONE, &list, "Start in list view", NULL},
        {"grid", 'g', 0, G_OPTION_ARG_NONE, &grid, "Start in grid view", NULL},
        {"icon-size", 's', 0, G_OPTION_ARG_INT, &icon_size, "Icon size in the grid view (default 96)", "PX"},
        {"list-icon-size", 0, 0, G_OPTION_ARG_INT, &list_icon_size, "Icon size in the list view (default 32)", "PX"},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &paths, NULL, "[DIRECTORY|FILE...]"},
        G_OPTION_ENTRY_NULL};
    GOptionContext *context;
    GError *error = NULL;
    gboolean opened = FALSE;

    howl_start_time = g_get_monotonic_time();
    g_set_prgname("howl");
    context = g_option_context_new("[DIRECTORY|FILE...] - browse files");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("howl: %s\n", error->message);
        g_error_free(error);
        return 1;
    }
    g_option_context_free(context);

    if (version) {
        printf("howl %s\n", HOWL_VERSION);
        return 0;
    }

    howl_settings_load();
    if (no_hidden) {
        howl_opts.show_hidden = FALSE;
    }
    howl_opts.thumbnails = !no_thumbnails;
    if (list) {
        howl_opts.list_view = TRUE;
    }
    if (grid) {
        howl_opts.list_view = FALSE;
    }
    if (icon_size > 0) {
        howl_opts.grid_size = CLAMP(icon_size, 16, 512);
    }
    if (list_icon_size > 0) {
        howl_opts.list_size = CLAMP(list_icon_size, 16, 512);
    }
    howl_settings_baseline();

    if (!gtk_init_check()) {
        g_printerr("howl: could not open the display\n");
        return 1;
    }
    g_set_application_name("Howl");
    gtk_window_set_default_icon_name("system-file-manager");
    load_css();
    howl_thumbs_init();

    if (paths == NULL || paths[0] == NULL) {
        GFile *home = g_file_new_for_path(g_get_home_dir());

        howl_window_new(home, NULL);
        g_object_unref(home);
        opened = TRUE;
    } else {
        for (char **p = paths; *p != NULL; p++) {
            opened = open_argument(*p) || opened;
        }
    }
    g_strfreev(paths);

    if (!opened) {
        return 1;
    }

    loop = g_main_loop_new(NULL, FALSE);
    install_signal_handlers();
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    loop = NULL;

    howl_settings_flush();
    howl_thumbs_shutdown();
    g_debug("clean shutdown %.0f ms after start", (g_get_monotonic_time() - howl_start_time) / 1000.0);
    return 0;
}
