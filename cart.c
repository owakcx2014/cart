/*
 * Cart - drag-and-drop .deb installer
 * Copyright (c) 2026 [owakcx2014]
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>

#define APP_NAME   "Cart"
#define ICON_SIZE  112
#define CART_MIME  "application/x-cart-package"

enum {
    TARGET_URI_LIST = 0,
    TARGET_CART     = 1
};

static const GtkTargetEntry uri_targets[] = {
    { "text/uri-list", 0, TARGET_URI_LIST }
};
static const GtkTargetEntry cart_targets[] = {
    { CART_MIME, GTK_TARGET_SAME_APP, TARGET_CART }
};
static const GtkTargetEntry folder_targets[] = {
    { "text/uri-list", 0,                   TARGET_URI_LIST },
    { CART_MIME,       GTK_TARGET_SAME_APP, TARGET_CART }
};

typedef enum { STATUS_INFO, STATUS_OK, STATUS_ERROR } StatusKind;

typedef struct {
    GtkWidget  *window;
    GtkWidget  *statusbar;
    GtkWidget  *spinner;
    GtkWidget  *app_slot;
    GtkWidget  *app_image;
    GtkWidget  *name_label;
    GtkWidget  *sub_label;
    GtkWidget  *hint_label;
    GtkWidget  *folder_slot;
    GtkWidget  *folder_box;
    guint       status_ctx;

    GdkPixbuf  *app_pixbuf;
    gchar      *deb_path;
    gchar      *display_name;
    gchar      *pkg_name;

    gboolean    busy;
    gboolean    prompt_pending;
} AppState;

typedef struct {
    gchar     *deb_path;
    gchar     *pkg;
    gchar     *version;
    gchar     *display;
    GdkPixbuf *icon;
} DebInfo;

static void
deb_info_free(DebInfo *i)
{
    if (!i) return;
    g_free(i->deb_path);
    g_free(i->pkg);
    g_free(i->version);
    g_free(i->display);
    g_clear_object(&i->icon);
    g_free(i);
}

static void
set_status(AppState *s, StatusKind kind, const gchar *msg)
{
    GtkStyleContext *ctx = gtk_widget_get_style_context(s->statusbar);

    gtk_style_context_remove_class(ctx, "ok");
    gtk_style_context_remove_class(ctx, "err");
    if (kind == STATUS_OK)    gtk_style_context_add_class(ctx, "ok");
    if (kind == STATUS_ERROR) gtk_style_context_add_class(ctx, "err");

    gtk_statusbar_remove_all(GTK_STATUSBAR(s->statusbar), s->status_ctx);
    gtk_statusbar_push(GTK_STATUSBAR(s->statusbar), s->status_ctx, msg);
}

static void
set_busy(AppState *s, gboolean busy)
{
    s->busy = busy;
    if (busy) {
        gtk_widget_set_visible(s->spinner, TRUE);
        gtk_spinner_start(GTK_SPINNER(s->spinner));
    } else {
        gtk_spinner_stop(GTK_SPINNER(s->spinner));
        gtk_widget_set_visible(s->spinner, FALSE);
    }
}

static GdkPixbuf *
load_themed_icon(const gchar *names[], gint size)
{
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GtkIconInfo  *info  = gtk_icon_theme_choose_icon(theme, names, size,
                                                     GTK_ICON_LOOKUP_FORCE_SIZE);
    GdkPixbuf *pb = NULL;

    if (info) {
        pb = gtk_icon_info_load_icon(info, NULL);
        g_object_unref(info);
    }
    return pb;
}

static GdkPixbuf *
load_bundled_folder_icon(gint size)
{
    const gchar *file = "folder-template.svg";
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    gchar *exe_dir = exe ? g_path_get_dirname(exe) : NULL;
    gchar *cwd = g_get_current_dir();
    gchar *paths[6];
    GdkPixbuf *pb = NULL;
    guint i;

    paths[0] = exe_dir ? g_build_filename(exe_dir, "assets", file, NULL) : NULL;
    paths[1] = g_build_filename(cwd, "assets", file, NULL);
    paths[2] = exe_dir ? g_build_filename(exe_dir, "..", "share", "cart", "assets", file, NULL) : NULL;
    paths[3] = g_build_filename("/usr/local/share/cart/assets", file, NULL);
    paths[4] = g_build_filename("/usr/share/cart/assets", file, NULL);
    paths[5] = NULL;

    for (i = 0; i < 5 && !pb; i++) {
        if (paths[i] && g_file_test(paths[i], G_FILE_TEST_IS_REGULAR))
            pb = gdk_pixbuf_new_from_file_at_scale(paths[i], size, size,
                                                   TRUE, NULL);
    }

    for (i = 0; i < 5; i++)
        g_free(paths[i]);
    g_free(cwd);
    g_free(exe_dir);
    g_free(exe);
    return pb;
}

static GdkPixbuf *
load_default_app_icon(void)
{
    const gchar *names[] = {
        "package-x-generic", "application-x-deb",
        "application-x-executable", "application-default-icon", NULL
    };
    return load_themed_icon(names, ICON_SIZE);
}

static void
remove_tree(const gchar *path)
{
    g_chmod(path, 0700);
    if (!g_file_test(path, G_FILE_TEST_IS_SYMLINK) &&
        g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir *d = g_dir_open(path, 0, NULL);
        if (d) {
            const gchar *n;
            while ((n = g_dir_read_name(d)) != NULL) {
                gchar *child = g_build_filename(path, n, NULL);
                remove_tree(child);
                g_free(child);
            }
            g_dir_close(d);
        }
    }
    g_remove(path);
}

static void
collect_files(const gchar *dir, GPtrArray *out)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    const gchar *n;

    if (!d) return;
    while ((n = g_dir_read_name(d)) != NULL) {
        gchar *path = g_build_filename(dir, n, NULL);

        if (g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
            g_free(path);
        } else if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            collect_files(path, out);
            g_free(path);
        } else if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            g_ptr_array_add(out, path);
        } else {
            g_free(path);
        }
    }
    g_dir_close(d);
}

static gboolean
is_image_ext(const gchar *ext)
{
    return ext &&
           (g_ascii_strcasecmp(ext, ".png") == 0 ||
            g_ascii_strcasecmp(ext, ".svg") == 0 ||
            g_ascii_strcasecmp(ext, ".xpm") == 0);
}

static gchar *
name_stem(const gchar *path)
{
    gchar *base = g_path_get_basename(path);
    gchar *dot  = strrchr(base, '.');

    if (dot && is_image_ext(dot))
        *dot = '\0';
    return base;
}

static void
read_desktop_info(GPtrArray *files, gchar **name, gchar **icon)
{
    guint i;

    *name = NULL;
    *icon = NULL;

    for (i = 0; i < files->len; i++) {
        const gchar *path = g_ptr_array_index(files, i);
        GKeyFile *kf;

        if (!g_str_has_suffix(path, ".desktop"))
            continue;

        kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
            gchar *n  = g_key_file_get_locale_string(kf, "Desktop Entry",
                                                     "Name", NULL, NULL);
            gchar *ic = g_key_file_get_string(kf, "Desktop Entry",
                                              "Icon", NULL);
            if (n || ic) {
                *name = n;
                *icon = ic;
                g_key_file_free(kf);
                return;
            }
            g_free(n);
            g_free(ic);
        }
        g_key_file_free(kf);
    }
}

static gchar *
find_best_icon(GPtrArray *files, const gchar *icon_name, const gchar *pkg)
{
    gchar *icon_key = icon_name ? name_stem(icon_name) : NULL;
    gchar *best = NULL;
    glong  best_score = -1;
    guint  i;

    for (i = 0; i < files->len; i++) {
        const gchar *path = g_ptr_array_index(files, i);
        gchar *base = g_path_get_basename(path);
        gchar *ext  = strrchr(base, '.');
        gint   w = 0, h = 0, tier = 1;
        glong  score, size;
        gchar *stem;
        gboolean is_svg;

        if (!is_image_ext(ext) || strstr(path, "symbolic")) {
            g_free(base);
            continue;
        }
        if (!gdk_pixbuf_get_file_info(path, &w, &h)) {
            g_free(base);
            continue;
        }

        is_svg = (g_ascii_strcasecmp(ext, ".svg") == 0);
        stem   = name_stem(path);

        if (icon_key && g_ascii_strcasecmp(stem, icon_key) == 0)
            tier = 3;
        else if (pkg && g_ascii_strcasecmp(stem, pkg) == 0)
            tier = 2;

        size  = is_svg ? 1024 : w;
        score = (glong)tier * 100000 + size;

        if (score > best_score) {
            best_score = score;
            g_free(best);
            best = g_strdup(path);
        }
        g_free(stem);
        g_free(base);
    }

    g_free(icon_key);
    return best;
}

static void
extract_thread(GTask *task, gpointer src, gpointer task_data,
               GCancellable *cancellable)
{
    DebInfo *info = task_data;
    GError  *err  = NULL;
    gchar   *out  = NULL;
    gint     st   = 0;
    gchar  **lines, **p;
    gchar   *tmpdir;
    GPtrArray *files;
    gchar   *dname = NULL, *dicon = NULL, *best;

    gchar *argv1[] = { "dpkg-deb", "-f", info->deb_path,
                       "Package", "Version", NULL };

    if (!g_spawn_sync(NULL, argv1, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, &out, NULL, &st, &err)) {
        g_task_return_error(task, err);
        return;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        g_free(out);
        g_task_return_new_error(task, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                                "not a valid Debian package");
        return;
    }

    lines = g_strsplit(out ? out : "", "\n", -1);
    for (p = lines; *p; p++) {
        if (g_str_has_prefix(*p, "Package:"))
            info->pkg = g_strstrip(g_strdup(*p + 8));
        else if (g_str_has_prefix(*p, "Version:"))
            info->version = g_strstrip(g_strdup(*p + 8));
    }
    g_strfreev(lines);
    g_free(out);

    if (!info->pkg || !*info->pkg) {
        g_task_return_new_error(task, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                                "package has no Package field");
        return;
    }

    tmpdir = g_dir_make_tmp("cart-XXXXXX", &err);
    if (!tmpdir) {
        g_task_return_error(task, err);
        return;
    }

    {
        const gchar *script =
            "dpkg-deb --fsys-tarfile \"$1\" 2>/dev/null | "
            "tar -x -C \"$2\" --no-same-owner --no-same-permissions "
            "--wildcards --wildcards-match-slash "
            "'*usr/share/icons/*' '*usr/share/pixmaps/*' "
            "'*usr/share/applications/*.desktop' 2>/dev/null; exit 0";
        gchar *argv2[] = { "sh", "-c", (gchar *)script, "sh",
                           info->deb_path, tmpdir, NULL };

        g_spawn_sync(NULL, argv2, NULL,
                     G_SPAWN_SEARCH_PATH |
                     G_SPAWN_STDOUT_TO_DEV_NULL |
                     G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, NULL, NULL, NULL, NULL);
    }

    files = g_ptr_array_new_with_free_func(g_free);
    collect_files(tmpdir, files);

    read_desktop_info(files, &dname, &dicon);
    info->display = (dname && *dname) ? g_strdup(dname) : g_strdup(info->pkg);

    best = find_best_icon(files, dicon, info->pkg);
    if (best) {
        info->icon = gdk_pixbuf_new_from_file_at_scale(best, ICON_SIZE,
                                                       ICON_SIZE, TRUE, NULL);
        g_free(best);
    }

    g_free(dname);
    g_free(dicon);
    g_ptr_array_free(files, TRUE);
    remove_tree(tmpdir);
    g_free(tmpdir);

    g_task_return_boolean(task, TRUE);
}

static void
apply_package(AppState *s, DebInfo *info)
{
    GdkPixbuf *pb;
    gchar *title, *sub;

    g_free(s->deb_path);
    g_free(s->display_name);
    g_free(s->pkg_name);
    s->deb_path     = g_strdup(info->deb_path);
    s->display_name = g_strdup(info->display);
    s->pkg_name     = g_strdup(info->pkg);

    pb = info->icon ? g_object_ref(info->icon) : load_default_app_icon();
    g_clear_object(&s->app_pixbuf);
    s->app_pixbuf = pb;

    gtk_image_set_from_pixbuf(GTK_IMAGE(s->app_image), pb);
    gtk_widget_set_opacity(s->app_image, 1.0);

    sub = g_strdup_printf("%s · %s", info->pkg,
                          info->version ? info->version : "?");
    gtk_label_set_text(GTK_LABEL(s->name_label), info->display);
    gtk_label_set_text(GTK_LABEL(s->sub_label), sub);
    gtk_label_set_text(GTK_LABEL(s->hint_label),
                       "Drag the app icon onto the Applications folder");

    title = g_strdup_printf("Install — %s", info->display);
    gtk_window_set_title(GTK_WINDOW(s->window), title);

    gtk_drag_source_set(s->app_slot, GDK_BUTTON1_MASK, cart_targets,
                        G_N_ELEMENTS(cart_targets), GDK_ACTION_COPY);

    g_free(title);
    g_free(sub);
}

static void
on_extract_done(GObject *src, GAsyncResult *res, gpointer data)
{
    AppState *s = data;
    GError   *err = NULL;
    DebInfo  *info = g_task_get_task_data(G_TASK(res));

    set_busy(s, FALSE);

    if (!g_task_propagate_boolean(G_TASK(res), &err)) {
        gchar *msg = g_strdup_printf("✗ Cannot read package: %s",
                                     err ? err->message : "unknown error");
        set_status(s, STATUS_ERROR, msg);
        g_free(msg);
        g_clear_error(&err);
        return;
    }

    apply_package(s, info);
    {
        gchar *msg = g_strdup_printf("Ready — drag “%s” onto Applications "
                                     "to install", s->display_name);
        set_status(s, STATUS_INFO, msg);
        g_free(msg);
    }
}

static void
load_deb(AppState *s, const gchar *path)
{
    gchar   *low;
    gboolean ok;
    DebInfo *info;
    GTask   *task;

    if (s->busy) {
        set_status(s, STATUS_INFO, "Please wait — an operation is in progress…");
        return;
    }

    low = g_ascii_strdown(path, -1);
    ok  = g_str_has_suffix(low, ".deb");
    g_free(low);
    if (!ok) {
        set_status(s, STATUS_ERROR, "✗ Only .deb files are supported");
        return;
    }

    set_busy(s, TRUE);
    set_status(s, STATUS_INFO, "Reading package…");

    info = g_new0(DebInfo, 1);
    info->deb_path = g_strdup(path);

    task = g_task_new(NULL, NULL, on_extract_done, s);
    g_task_set_task_data(task, info, (GDestroyNotify)deb_info_free);
    g_task_run_in_thread(task, extract_thread);
    g_object_unref(task);
}

static gchar *
ask_password(AppState *s)
{
    GtkWidget *dialog, *content, *hbox, *icon, *vbox, *label, *entry;
    gchar *markup, *pw = NULL;
    gint   resp;

    dialog = gtk_dialog_new_with_buttons("Authentication Required",
                 GTK_WINDOW(s->window),
                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                 "_Cancel",  GTK_RESPONSE_CANCEL,
                 "_Install", GTK_RESPONSE_OK,
                 NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 14);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_box_pack_start(GTK_BOX(content), hbox, TRUE, TRUE, 0);

    icon = gtk_image_new_from_icon_name("dialog-password",
                                        GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 0);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    markup = g_markup_printf_escaped(
        "<b>Enter your sudo password</b>\n"
        "Administrator rights are required to install “%s”.",
        s->display_name ? s->display_name : "this package");
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 44);
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
    g_free(markup);

    entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_invisible_char(GTK_ENTRY(entry), '*');
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Password");
    gtk_box_pack_start(GTK_BOX(vbox), entry, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(entry);

    resp = gtk_dialog_run(GTK_DIALOG(dialog));
    if (resp == GTK_RESPONSE_OK)
        pw = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));

    gtk_entry_set_text(GTK_ENTRY(entry), "");
    gtk_widget_destroy(dialog);
    return pw;
}

static void
wipe_and_free(gchar *secret)
{
    if (!secret) return;
    explicit_bzero(secret, strlen(secret));
    g_free(secret);
}

static gchar *
pick_detail(const gchar *text)
{
    gchar **lines, *result = NULL;
    gint i, n;

    if (!text) return NULL;
    lines = g_strsplit(text, "\n", -1);
    n = (gint)g_strv_length(lines);

    for (i = 0; i < n && !result; i++) {
        gchar *t = g_strstrip(lines[i]);
        if (strstr(t, " depends on "))
            result = g_strdup(t);
    }
    for (i = n - 1; i >= 0 && !result; i--) {
        gchar *t = g_strstrip(lines[i]);
        if (g_str_has_prefix(t, "E: "))
            result = g_strdup(t + 3);
    }
    for (i = n - 1; i >= 0 && !result; i--) {
        gchar *t = g_strstrip(lines[i]);
        if (*t)
            result = g_strdup(t);
    }
    g_strfreev(lines);

    if (result && g_utf8_strlen(result, -1) > 120) {
        gchar *cut = g_utf8_offset_to_pointer(result, 120);
        gchar *tmp;
        *cut = '\0';
        tmp = g_strconcat(result, "…", NULL);
        g_free(result);
        result = tmp;
    }
    return result;
}

static void
on_install_done(GObject *src, GAsyncResult *res, gpointer data)
{
    AppState   *s    = data;
    GSubprocess *proc = G_SUBPROCESS(src);
    gchar      *out  = NULL;
    GError     *err  = NULL;
    gchar      *msg;
    gboolean    ok   = FALSE;

    g_subprocess_communicate_utf8_finish(proc, res, &out, NULL, &err);
    set_busy(s, FALSE);

    if (!err && g_subprocess_get_if_exited(proc) &&
        g_subprocess_get_exit_status(proc) == 0)
        ok = TRUE;

    if (ok) {
        msg = g_strdup_printf("✓ “%s” was installed successfully",
                              s->display_name);
        set_status(s, STATUS_OK, msg);
    } else if (err) {
        msg = g_strdup_printf("✗ Installation failed: %s", err->message);
        set_status(s, STATUS_ERROR, msg);
    } else if (out && (strstr(out, "incorrect password") ||
                       strstr(out, "Sorry, try again") ||
                       strstr(out, "no password was provided") ||
                       strstr(out, "a password is required"))) {
        msg = g_strdup("✗ Authentication failed — incorrect sudo password");
        set_status(s, STATUS_ERROR, msg);
    } else if (out && strstr(out, "not in the sudoers")) {
        msg = g_strdup("✗ This user is not allowed to use sudo");
        set_status(s, STATUS_ERROR, msg);
    } else {
        gchar *detail = pick_detail(out);

        msg = g_strdup_printf("✗ Installation of “%s” failed%s%s",
                              s->display_name,
                              detail ? ": " : "",
                              detail ? detail : "");
        set_status(s, STATUS_ERROR, msg);
        g_free(detail);
    }

    g_free(msg);
    g_free(out);
    g_clear_error(&err);
    g_object_unref(proc);
}

static void
start_install(AppState *s, gchar *password)
{
    GError *err = NULL;
    GSubprocessLauncher *launcher;
    GSubprocess *proc;
    gchar *msg;

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);

    {
        static const gchar *script =
            "dpkg --force-confold -i \"$1\"\n"
            "rc=$?\n"
            "if [ $rc -ne 0 ]; then\n"
            "  echo '[cart] dpkg reported problems, resolving dependencies...'\n"
            "  apt-get install -f -y -o Dpkg::Options::=--force-confold || exit 1\n"
            "fi\n"
            "if dpkg -s \"$2\" 2>/dev/null | grep -q '^Status: install ok installed'; then\n"
            "  exit 0\n"
            "fi\n"
            "echo \"E: package $2 is not fully installed\"\n"
            "exit 1\n";

        proc = g_subprocess_launcher_spawn(launcher, &err,
                   "sudo", "-k", "-S", "-p", "",
                   "env", "DEBIAN_FRONTEND=noninteractive", "LC_ALL=C",
                   "sh", "-c", script, "sh", s->deb_path, s->pkg_name,
                   NULL);
    }
    g_object_unref(launcher);

    if (!proc) {
        msg = g_strdup_printf("✗ Could not start sudo: %s", err->message);
        set_status(s, STATUS_ERROR, msg);
        g_free(msg);
        g_clear_error(&err);
        wipe_and_free(password);
        return;
    }

    {
        GOutputStream *in = g_subprocess_get_stdin_pipe(proc);
        gchar *line = g_strconcat(password, "\n", NULL);

        g_output_stream_write_all(in, line, strlen(line), NULL, NULL, NULL);

        wipe_and_free(line);
        wipe_and_free(password);
    }

    set_busy(s, TRUE);
    msg = g_strdup_printf("Installing “%s” (dependencies are resolved automatically)…",
                          s->display_name);
    set_status(s, STATUS_INFO, msg);
    g_free(msg);

    g_subprocess_communicate_utf8_async(proc, NULL, NULL,
                                        on_install_done, s);
}

static gboolean
prompt_install_idle(gpointer data)
{
    AppState *s = data;
    gchar *pw;

    s->prompt_pending = FALSE;
    if (!s->deb_path || s->busy)
        return G_SOURCE_REMOVE;

    pw = ask_password(s);
    if (!pw) {
        set_status(s, STATUS_INFO, "Installation cancelled");
        return G_SOURCE_REMOVE;
    }
    start_install(s, pw);
    return G_SOURCE_REMOVE;
}

static void
on_drag_data_received(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                      GtkSelectionData *sel, guint info, guint time,
                      gpointer data)
{
    AppState *s = data;

    if (gtk_selection_data_get_length(sel) < 0)
        return;

    if (info == TARGET_URI_LIST) {
        gchar **uris = gtk_selection_data_get_uris(sel);
        gchar  *file = NULL;
        gint    i;

        for (i = 0; uris && uris[i]; i++) {
            gchar *f = g_filename_from_uri(uris[i], NULL, NULL);
            if (f) {
                file = f;
                break;
            }
        }
        if (file) {
            load_deb(s, file);
            g_free(file);
        }
        g_strfreev(uris);

    } else if (info == TARGET_CART && w == s->folder_slot) {
        if (s->deb_path && !s->busy && !s->prompt_pending) {
            s->prompt_pending = TRUE;
            g_idle_add(prompt_install_idle, s);
        }
    }
}

static void
on_app_drag_data_get(GtkWidget *w, GdkDragContext *ctx,
                     GtkSelectionData *sel, guint info, guint time,
                     gpointer data)
{
    gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8,
                           (const guchar *)"cart", 4);
}

static void
on_app_drag_begin(GtkWidget *w, GdkDragContext *ctx, gpointer data)
{
    AppState *s = data;

    if (s->app_pixbuf) {
        GdkPixbuf *small = gdk_pixbuf_scale_simple(s->app_pixbuf, 72, 72,
                                                   GDK_INTERP_BILINEAR);
        if (small) {
            gtk_drag_set_icon_pixbuf(ctx, small, 36, 36);
            g_object_unref(small);
        }
    }
}

static gboolean
on_folder_drag_motion(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                      guint time, gpointer data)
{
    AppState *s = data;

    gtk_style_context_add_class(gtk_widget_get_style_context(s->folder_box),
                                "drop-hover");
    return FALSE;
}

static void
on_folder_drag_leave(GtkWidget *w, GdkDragContext *ctx, guint time,
                     gpointer data)
{
    AppState *s = data;

    gtk_style_context_remove_class(gtk_widget_get_style_context(s->folder_box),
                                   "drop-hover");
}

static const gchar *CSS =
    "window.cart-window {"
    "  background-image: linear-gradient(160deg, #4b4f61 0%, #2b2d38 45%, #131419 100%);"
    "}"
    ".cart-window label { color: #f2f2f5; }"
    ".cart-window label.app-name { font-size: 15px; font-weight: 600; }"
    ".cart-window label.app-sub  { font-size: 11px; color: rgba(255,255,255,0.55); }"
    ".cart-window label.hint     { font-size: 13px; color: rgba(255,255,255,0.60); }"
    ".cart-window label.arrow    { font-size: 56px; color: rgba(255,255,255,0.55); }"
    ".slot { padding: 14px 18px; border-radius: 18px; }"
    ".slot.drop-hover { background-color: rgba(255,255,255,0.14); }"
    "statusbar { background-color: rgba(0,0,0,0.30);"
    "            border-top: 1px solid rgba(255,255,255,0.08);"
    "            padding: 3px 8px; }"
    "statusbar label { color: #c9cbd4; font-size: 12px; }"
    "statusbar.ok  label { color: #7ee787; }"
    "statusbar.err label { color: #ff7b72; }";

static GtkWidget *
make_slot(GtkWidget **eventbox, GtkWidget **image, GtkWidget **name,
          GtkWidget **sub)
{
    GtkWidget *eb  = gtk_event_box_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *img = gtk_image_new();
    GtkWidget *nm  = gtk_label_new("");
    GtkWidget *sb  = gtk_label_new("");

    gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "slot");
    gtk_widget_set_size_request(box, 180, -1);

    gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_SIZE);
    gtk_widget_set_size_request(img, ICON_SIZE, ICON_SIZE);

    gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(nm), 20);
    gtk_label_set_justify(GTK_LABEL(nm), GTK_JUSTIFY_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(nm), "app-name");

    gtk_label_set_ellipsize(GTK_LABEL(sb), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(sb), 24);
    gtk_label_set_justify(GTK_LABEL(sb), GTK_JUSTIFY_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(sb), "app-sub");

    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), nm,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sb,  FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(eb), box);

    *eventbox = eb;
    *image    = img;
    *name     = nm;
    *sub      = sb;
    return box;
}

static void
build_ui(AppState *s)
{
    GtkCssProvider *provider;
    GtkWidget *vbox, *center, *row, *arrow, *app_box, *msg_area;
    GtkWidget *folder_img, *folder_name, *folder_sub;
    GdkPixbuf *pb;
    const gchar *folder_names[] = {
        "folder-templates", "folder-applications", "folder", NULL
    };

    s->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(s->window), APP_NAME);
    gtk_window_set_position(GTK_WINDOW(s->window), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable(GTK_WINDOW(s->window), FALSE);
    gtk_widget_set_size_request(s->window, 640, 400);
    gtk_style_context_add_class(gtk_widget_get_style_context(s->window),
                                "cart-window");
    g_signal_connect(s->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(s->window), vbox);

    center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_valign(center, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), center, TRUE, TRUE, 0);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 28);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(center), row, FALSE, FALSE, 0);

    app_box = make_slot(&s->app_slot, &s->app_image,
                        &s->name_label, &s->sub_label);
    (void)app_box;
    gtk_box_pack_start(GTK_BOX(row), s->app_slot, FALSE, FALSE, 0);

    arrow = gtk_label_new("➔");
    gtk_style_context_add_class(gtk_widget_get_style_context(arrow), "arrow");
    gtk_widget_set_valign(arrow, GTK_ALIGN_START);
    gtk_widget_set_margin_top(arrow, 32);
    gtk_box_pack_start(GTK_BOX(row), arrow, FALSE, FALSE, 0);

    s->folder_box = make_slot(&s->folder_slot, &folder_img,
                              &folder_name, &folder_sub);
    gtk_label_set_text(GTK_LABEL(folder_name), "Applications");
    gtk_box_pack_start(GTK_BOX(row), s->folder_slot, FALSE, FALSE, 0);

    pb = load_bundled_folder_icon(ICON_SIZE);
    if (!pb)
        pb = load_themed_icon(folder_names, ICON_SIZE);
    if (pb) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(folder_img), pb);
        g_object_unref(pb);
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(folder_img), "folder",
                                     GTK_ICON_SIZE_DIALOG);
    }

    pb = load_default_app_icon();
    if (pb) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(s->app_image), pb);
        g_object_unref(pb);
    }
    gtk_widget_set_opacity(s->app_image, 0.45);
    gtk_label_set_text(GTK_LABEL(s->name_label), "No package");
    gtk_label_set_text(GTK_LABEL(s->sub_label), "");

    s->hint_label = gtk_label_new("Drop a .deb file anywhere in this window");
    gtk_style_context_add_class(gtk_widget_get_style_context(s->hint_label),
                                "hint");
    gtk_widget_set_margin_top(s->hint_label, 10);
    gtk_box_pack_start(GTK_BOX(center), s->hint_label, FALSE, FALSE, 0);

    s->statusbar = gtk_statusbar_new();
    s->status_ctx = gtk_statusbar_get_context_id(GTK_STATUSBAR(s->statusbar),
                                                 "cart");
    gtk_box_pack_end(GTK_BOX(vbox), s->statusbar, FALSE, FALSE, 0);

    s->spinner = gtk_spinner_new();
    gtk_widget_set_no_show_all(s->spinner, TRUE);
    msg_area = gtk_statusbar_get_message_area(GTK_STATUSBAR(s->statusbar));
    gtk_box_pack_end(GTK_BOX(msg_area), s->spinner, FALSE, FALSE, 4);

    gtk_drag_dest_set(s->window, GTK_DEST_DEFAULT_ALL, uri_targets,
                      G_N_ELEMENTS(uri_targets), GDK_ACTION_COPY);
    g_signal_connect(s->window, "drag-data-received",
                     G_CALLBACK(on_drag_data_received), s);

    gtk_drag_dest_set(s->folder_slot,
                      GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP,
                      folder_targets, G_N_ELEMENTS(folder_targets),
                      GDK_ACTION_COPY);
    g_signal_connect(s->folder_slot, "drag-data-received",
                     G_CALLBACK(on_drag_data_received), s);
    g_signal_connect(s->folder_slot, "drag-motion",
                     G_CALLBACK(on_folder_drag_motion), s);
    g_signal_connect(s->folder_slot, "drag-leave",
                     G_CALLBACK(on_folder_drag_leave), s);

    g_signal_connect(s->app_slot, "drag-data-get",
                     G_CALLBACK(on_app_drag_data_get), s);
    g_signal_connect(s->app_slot, "drag-begin",
                     G_CALLBACK(on_app_drag_begin), s);

    set_status(s, STATUS_INFO, "Ready — drop a .deb file to begin");
}

int
main(int argc, char **argv)
{
    AppState *s;

    signal(SIGPIPE, SIG_IGN);
    gtk_init(&argc, &argv);

    gtk_widget_set_default_direction(GTK_TEXT_DIR_LTR);

    s = g_new0(AppState, 1);
    build_ui(s);
    gtk_widget_show_all(s->window);

    if (argc > 1) {
        gchar *abs = g_canonicalize_filename(argv[1], NULL);
        load_deb(s, abs);
        g_free(abs);
    }

    gtk_main();

    g_clear_object(&s->app_pixbuf);
    g_free(s->deb_path);
    g_free(s->display_name);
    g_free(s->pkg_name);
    g_free(s);
    return 0;
}
