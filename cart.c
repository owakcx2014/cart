/*
 * Cart - drag-and-drop .deb installer
 * Copyright (c) 2026 [Your Name]
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>

#define APP_NAME     "Cart"
#define ICON_SIZE    112
#define CART_MIME    "application/x-cart-package"
#define MAX_RESULTS  60

enum {
    TARGET_URI_LIST = 0,
    TARGET_CART     = 1
};

enum {
    RESP_DELETE         = 1001,
    RESP_INSTALL_ANYWAY = 1002,
    RESP_REFRESH        = 1003,
    RESP_INSTALL_SEL    = 1004,
    RESP_INSTALL_ALL    = 1005
};

enum {
    COL_NAME,
    COL_MARKUP,
    N_COLS
};

enum {
    UCOL_SEL,
    UCOL_NAME,
    UCOL_VERSIONS,
    UCOL_SIZE,
    UCOL_BYTES,
    UCOL_N
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

static const gchar *SCRIPT_DEB =
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

static const gchar *SCRIPT_REPO =
    "apt-get install -y -o Dpkg::Options::=--force-confold \"$1\" || exit 1\n"
    "if dpkg -s \"$1\" 2>/dev/null | grep -q '^Status: install ok installed'; then\n"
    "  exit 0\n"
    "fi\n"
    "echo \"E: package $1 is not fully installed\"\n"
    "exit 1\n";

static const gchar *SCRIPT_REMOVE =
    "dpkg --remove \"$1\" || exit 1\n"
    "if dpkg -s \"$1\" 2>/dev/null | grep -q '^Status: install ok installed'; then\n"
    "  echo \"E: package $1 is still installed\"\n"
    "  exit 1\n"
    "fi\n"
    "exit 0\n";

static const gchar *SCRIPT_UPDATE =
    "apt-get install -y --only-upgrade -o Dpkg::Options::=--force-confold \"$@\"\n";

static const gchar *SCRIPT_REFRESH =
    "apt-get update\n";

typedef enum { STATUS_INFO, STATUS_OK, STATUS_ERROR } StatusKind;
typedef enum { PENDING_NONE, PENDING_DEB, PENDING_REPO } PendingKind;

typedef struct {
    gchar  *name;
    gchar  *arch;
    gchar  *old_ver;
    gchar  *new_ver;
    gint64  size;
} UpdateInfo;

typedef struct {
    gchar    *name;
    gchar    *desc;
    gboolean  installed;
} SearchHit;

typedef struct {
    GtkWidget    *window;
    GtkWidget    *statusbar;
    GtkWidget    *spinner;
    GtkWidget    *search_entry;
    GtkWidget    *updates_btn;
    GtkWidget    *left_stack;
    GtkWidget    *app_slot;
    GtkWidget    *app_image;
    GtkWidget    *name_label;
    GtkWidget    *sub_label;
    GtkWidget    *hint_label;
    GtkWidget    *results_view;
    GtkListStore *results_store;
    GtkWidget    *folder_slot;
    GtkWidget    *folder_box;
    guint         status_ctx;

    GdkPixbuf    *app_pixbuf;
    gchar        *deb_path;
    gchar        *display_name;
    gchar        *pkg_name;
    gchar        *version;

    PendingKind   pending_kind;
    gchar        *pending_repo;

    gboolean      busy;
    gboolean      prompt_pending;
    gboolean      updates_checking;
    guint         search_gen;
    GPtrArray    *updates;
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

static void check_updates_async(AppState *s, gboolean open_dialog);
static void refresh_search_results(AppState *s);
static void update_hint(AppState *s);

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

static gchar *
query_installed_version(const gchar *pkg)
{
    gchar *argv[] = { "dpkg-query", "-W",
                      "-f=${db:Status-Abbrev}|${Version}\n",
                      (gchar *)pkg, NULL };
    gchar *out = NULL, *version = NULL;
    gchar **lines, **p;

    if (!g_spawn_sync(NULL, argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, &out, NULL, NULL, NULL))
        return NULL;

    lines = g_strsplit(out ? out : "", "\n", -1);
    for (p = lines; *p && !version; p++) {
        if (g_str_has_prefix(*p, "ii")) {
            const gchar *bar = strchr(*p, '|');
            if (bar && bar[1])
                version = g_strdup(bar + 1);
        }
    }
    g_strfreev(lines);
    g_free(out);
    return version;
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
    g_free(s->version);
    s->deb_path     = g_strdup(info->deb_path);
    s->display_name = g_strdup(info->display);
    s->pkg_name     = g_strdup(info->pkg);
    s->version      = g_strdup(info->version);

    pb = info->icon ? g_object_ref(info->icon) : load_default_app_icon();
    g_clear_object(&s->app_pixbuf);
    s->app_pixbuf = pb;

    gtk_image_set_from_pixbuf(GTK_IMAGE(s->app_image), pb);
    gtk_widget_set_opacity(s->app_image, 1.0);

    sub = g_strdup_printf("%s · %s", info->pkg,
                          info->version ? info->version : "?");
    gtk_label_set_text(GTK_LABEL(s->name_label), info->display);
    gtk_label_set_text(GTK_LABEL(s->sub_label), sub);

    gtk_entry_set_text(GTK_ENTRY(s->search_entry), "");
    gtk_stack_set_visible_child_name(GTK_STACK(s->left_stack), "app");
    update_hint(s);

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
ask_password(AppState *s, const gchar *reason)
{
    GtkWidget *dialog, *content, *hbox, *icon, *vbox, *label, *entry;
    gchar *markup, *pw = NULL;
    gint   resp;

    dialog = gtk_dialog_new_with_buttons("Authentication Required",
                 GTK_WINDOW(s->window),
                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                 "_Cancel",  GTK_RESPONSE_CANCEL,
                 "_Continue", GTK_RESPONSE_OK,
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
        "Administrator rights are required to %s.", reason);
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

static gchar *
failure_message(const gchar *what, const gchar *out)
{
    gchar *detail, *msg;

    if (out && (strstr(out, "incorrect password") ||
                strstr(out, "Sorry, try again") ||
                strstr(out, "no password was provided") ||
                strstr(out, "a password is required")))
        return g_strdup("✗ Authentication failed — incorrect sudo password");

    if (out && strstr(out, "not in the sudoers"))
        return g_strdup("✗ This user is not allowed to use sudo");

    detail = pick_detail(out);
    msg = g_strdup_printf("✗ %s failed%s%s", what,
                          detail ? ": " : "", detail ? detail : "");
    g_free(detail);
    return msg;
}

typedef void (*JobDoneFunc)(AppState *s, gboolean ok, const gchar *output,
                            gpointer user_data);

typedef struct {
    AppState      *s;
    JobDoneFunc    done;
    gpointer       user_data;
    GDestroyNotify free_user;
} Job;

static void
job_free(Job *j)
{
    if (j->free_user)
        j->free_user(j->user_data);
    g_free(j);
}

static void
job_finished(GObject *src, GAsyncResult *res, gpointer data)
{
    Job *job = data;
    GSubprocess *proc = G_SUBPROCESS(src);
    gchar  *out = NULL;
    GError *err = NULL;
    gboolean ok;

    g_subprocess_communicate_utf8_finish(proc, res, &out, NULL, &err);

    ok = !err && g_subprocess_get_if_exited(proc) &&
         g_subprocess_get_exit_status(proc) == 0;

    if (err) {
        g_free(out);
        out = g_strdup_printf("E: %s", err->message);
        g_clear_error(&err);
    }

    set_busy(job->s, FALSE);
    job->done(job->s, ok, out ? out : "", job->user_data);

    g_free(out);
    g_object_unref(proc);
    job_free(job);
}

static void
begin_privileged(AppState *s, const gchar *reason, const gchar *script,
                 GPtrArray *args, const gchar *busy_msg,
                 JobDoneFunc done, gpointer user_data,
                 GDestroyNotify free_user)
{
    Job *job;
    gchar *password;
    GPtrArray *argv;
    GSubprocessLauncher *launcher;
    GSubprocess *proc;
    GError *err = NULL;
    guint i;

    password = ask_password(s, reason);
    if (!password) {
        set_status(s, STATUS_INFO, "Cancelled");
        if (free_user)
            free_user(user_data);
        return;
    }

    argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gpointer)"sudo");
    g_ptr_array_add(argv, (gpointer)"-k");
    g_ptr_array_add(argv, (gpointer)"-S");
    g_ptr_array_add(argv, (gpointer)"-p");
    g_ptr_array_add(argv, (gpointer)"");
    g_ptr_array_add(argv, (gpointer)"env");
    g_ptr_array_add(argv, (gpointer)"DEBIAN_FRONTEND=noninteractive");
    g_ptr_array_add(argv, (gpointer)"LC_ALL=C");
    g_ptr_array_add(argv, (gpointer)"sh");
    g_ptr_array_add(argv, (gpointer)"-c");
    g_ptr_array_add(argv, (gpointer)script);
    g_ptr_array_add(argv, (gpointer)"sh");
    for (i = 0; args && i < args->len; i++)
        g_ptr_array_add(argv, g_ptr_array_index(args, i));
    g_ptr_array_add(argv, NULL);

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
    proc = g_subprocess_launcher_spawnv(launcher,
                                        (const gchar * const *)argv->pdata,
                                        &err);
    g_object_unref(launcher);
    g_ptr_array_free(argv, TRUE);

    job = g_new0(Job, 1);
    job->s         = s;
    job->done      = done;
    job->user_data = user_data;
    job->free_user = free_user;

    if (!proc) {
        gchar *out = g_strdup_printf("E: Could not start sudo: %s",
                                     err ? err->message : "unknown error");
        g_clear_error(&err);
        wipe_and_free(password);
        done(s, FALSE, out, user_data);
        g_free(out);
        job_free(job);
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
    set_status(s, STATUS_INFO, busy_msg);

    g_subprocess_communicate_utf8_async(proc, NULL, NULL, job_finished, job);
}

static void
install_done(AppState *s, gboolean ok, const gchar *out, gpointer data)
{
    const gchar *label = data;
    gchar *msg;

    if (ok) {
        msg = g_strdup_printf("✓ “%s” was installed successfully", label);
    } else {
        gchar *what = g_strdup_printf("Installation of “%s”", label);
        msg = failure_message(what, out);
        g_free(what);
    }
    set_status(s, ok ? STATUS_OK : STATUS_ERROR, msg);
    g_free(msg);

    if (ok)
        refresh_search_results(s);
}

static void
remove_done(AppState *s, gboolean ok, const gchar *out, gpointer data)
{
    const gchar *label = data;
    gchar *msg;

    if (ok) {
        msg = g_strdup_printf("✓ “%s” was removed", label);
    } else {
        gchar *what = g_strdup_printf("Removal of “%s”", label);
        msg = failure_message(what, out);
        g_free(what);
    }
    set_status(s, ok ? STATUS_OK : STATUS_ERROR, msg);
    g_free(msg);

    if (ok)
        refresh_search_results(s);
}

static void
update_done(AppState *s, gboolean ok, const gchar *out, gpointer data)
{
    guint n = GPOINTER_TO_UINT(data);
    gchar *msg;

    if (ok)
        msg = g_strdup_printf("✓ %u update%s installed", n, n == 1 ? "" : "s");
    else
        msg = failure_message("Update", out);

    set_status(s, ok ? STATUS_OK : STATUS_ERROR, msg);
    g_free(msg);

    check_updates_async(s, FALSE);
}

static void
refresh_done(AppState *s, gboolean ok, const gchar *out, gpointer data)
{
    if (ok) {
        set_status(s, STATUS_OK, "✓ Package lists refreshed");
    } else {
        gchar *msg = failure_message("Refreshing the package lists", out);
        set_status(s, STATUS_ERROR, msg);
        g_free(msg);
    }
    check_updates_async(s, ok);
}

static void
start_install(AppState *s, gboolean from_repo, const gchar *pkg,
              const gchar *label, const gchar *path)
{
    GPtrArray *args = g_ptr_array_new();
    gchar *reason = g_strdup_printf("install “%s”", label);
    gchar *busy   = g_strdup_printf("Installing “%s” "
                                    "(dependencies are resolved automatically)…",
                                    label);

    if (from_repo) {
        g_ptr_array_add(args, (gpointer)pkg);
    } else {
        g_ptr_array_add(args, (gpointer)path);
        g_ptr_array_add(args, (gpointer)pkg);
    }

    begin_privileged(s, reason, from_repo ? SCRIPT_REPO : SCRIPT_DEB, args,
                     busy, install_done, g_strdup(label), g_free);

    g_ptr_array_free(args, TRUE);
    g_free(reason);
    g_free(busy);
}

static void
start_remove(AppState *s, const gchar *pkg, const gchar *label)
{
    GPtrArray *args = g_ptr_array_new();
    gchar *reason = g_strdup_printf("remove “%s”", label);
    gchar *busy   = g_strdup_printf("Removing “%s”…", label);

    g_ptr_array_add(args, (gpointer)pkg);
    begin_privileged(s, reason, SCRIPT_REMOVE, args, busy,
                     remove_done, g_strdup(label), g_free);

    g_ptr_array_free(args, TRUE);
    g_free(reason);
    g_free(busy);
}

static gint
ask_already_installed(AppState *s, const gchar *label, const gchar *installed,
                      const gchar *deb_version)
{
    gboolean differs = deb_version && strcmp(installed, deb_version) != 0;
    GtkWidget *dlg, *del;
    gchar *secondary;
    gint resp;

    dlg = gtk_message_dialog_new(GTK_WINDOW(s->window),
                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                 GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                 "The package is already installed");

    if (differs)
        secondary = g_strdup_printf(
            "“%s” version %s is installed, but this package is version %s.\n"
            "Do you want to delete the installed version?",
            label, installed, deb_version);
    else
        secondary = g_strdup_printf(
            "“%s” (version %s) is already installed.\n"
            "Do you want to delete it?", label, installed);

    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
                                             "%s", secondary);
    g_free(secondary);

    gtk_dialog_add_button(GTK_DIALOG(dlg), "_Cancel", GTK_RESPONSE_CANCEL);
    if (differs)
        gtk_dialog_add_button(GTK_DIALOG(dlg), "_Install anyway",
                              RESP_INSTALL_ANYWAY);
    del = gtk_dialog_add_button(GTK_DIALOG(dlg), "_Delete", RESP_DELETE);
    gtk_style_context_add_class(gtk_widget_get_style_context(del),
                                "destructive-action");
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CANCEL);

    resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return resp;
}

static void
handle_install_request(AppState *s, gboolean from_repo, const gchar *pkg,
                       const gchar *label, const gchar *path,
                       const gchar *deb_version)
{
    gchar *installed = query_installed_version(pkg);

    if (installed) {
        gint resp = ask_already_installed(s, label, installed,
                                          from_repo ? NULL : deb_version);
        g_free(installed);

        if (resp == RESP_DELETE) {
            start_remove(s, pkg, label);
            return;
        }
        if (resp != RESP_INSTALL_ANYWAY) {
            set_status(s, STATUS_INFO, "Cancelled");
            return;
        }
    }

    start_install(s, from_repo, pkg, label, path);
}

static gboolean
prompt_install_idle(gpointer data)
{
    AppState *s = data;
    PendingKind kind = s->pending_kind;
    gchar *repo = s->pending_repo;

    s->pending_repo   = NULL;
    s->pending_kind   = PENDING_NONE;
    s->prompt_pending = FALSE;

    if (!s->busy) {
        if (kind == PENDING_DEB && s->deb_path)
            handle_install_request(s, FALSE, s->pkg_name, s->display_name,
                                   s->deb_path, s->version);
        else if (kind == PENDING_REPO && repo)
            handle_install_request(s, TRUE, repo, repo, NULL, NULL);
    }

    g_free(repo);
    return G_SOURCE_REMOVE;
}

static void
search_hit_free(gpointer p)
{
    SearchHit *h = p;

    if (!h) return;
    g_free(h->name);
    g_free(h->desc);
    g_free(h);
}

typedef struct {
    AppState  *s;
    guint      gen;
    gchar     *term;
    gboolean   silent;
    GPtrArray *hits;
} SearchJob;

static void
search_job_free(SearchJob *j)
{
    if (!j) return;
    g_free(j->term);
    if (j->hits)
        g_ptr_array_unref(j->hits);
    g_free(j);
}

static gint
hit_rank(const gchar *name, const gchar *term)
{
    gchar *low = g_ascii_strdown(name, -1);
    gint rank = 2;

    if (strcmp(low, term) == 0)
        rank = 0;
    else if (g_str_has_prefix(low, term))
        rank = 1;
    g_free(low);
    return rank;
}

static gint
compare_hits(gconstpointer a, gconstpointer b, gpointer term)
{
    const SearchHit *ha = *(SearchHit * const *)a;
    const SearchHit *hb = *(SearchHit * const *)b;
    gint ra = hit_rank(ha->name, term);
    gint rb = hit_rank(hb->name, term);

    if (ra != rb)
        return ra - rb;
    return strcmp(ha->name, hb->name);
}

static void
search_thread(GTask *task, gpointer src, gpointer data,
              GCancellable *cancellable)
{
    SearchJob *job = data;
    GPtrArray *argv = g_ptr_array_new();
    GPtrArray *keep = g_ptr_array_new_with_free_func(g_free);
    gchar **envp = g_environ_setenv(g_get_environ(), "LC_ALL", "C", TRUE);
    gchar **words, **lines, **p;
    gchar *out = NULL, *iout = NULL, *lower;
    GHashTable *installed;

    job->hits = g_ptr_array_new_with_free_func(search_hit_free);

    g_ptr_array_add(argv, (gpointer)"apt-cache");
    g_ptr_array_add(argv, (gpointer)"search");
    g_ptr_array_add(argv, (gpointer)"--names-only");

    words = g_strsplit_set(job->term, " \t", -1);
    for (p = words; *p; p++) {
        gchar *esc, *pattern;

        if (!**p) continue;
        esc = g_regex_escape_string(*p, -1);
        if (esc[0] == '-')
            pattern = g_strconcat("[-]", esc + 1, NULL);
        else
            pattern = g_strdup(esc);
        g_free(esc);
        g_ptr_array_add(keep, pattern);
        g_ptr_array_add(argv, pattern);
    }
    g_ptr_array_add(argv, NULL);

    if (argv->len > 4 &&
        g_spawn_sync(NULL, (gchar **)argv->pdata, envp,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, &out, NULL, NULL, NULL) && out) {

        gchar *iargv[] = { "dpkg-query", "-W",
                           "-f=${Package}|${db:Status-Abbrev}\n", NULL };

        installed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        if (g_spawn_sync(NULL, iargv, envp,
                         G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL, NULL, &iout, NULL, NULL, NULL) && iout) {
            lines = g_strsplit(iout, "\n", -1);
            for (p = lines; *p; p++) {
                gchar *bar = strchr(*p, '|');
                if (bar && g_str_has_prefix(bar + 1, "ii")) {
                    *bar = '\0';
                    g_hash_table_add(installed, g_strdup(*p));
                }
            }
            g_strfreev(lines);
        }

        lines = g_strsplit(out, "\n", -1);
        for (p = lines; *p; p++) {
            gchar *sep = strstr(*p, " - ");
            SearchHit *h;

            if (!sep) continue;
            h = g_new0(SearchHit, 1);
            h->name = g_strndup(*p, sep - *p);
            h->desc = g_strdup(sep + 3);
            h->installed = g_hash_table_contains(installed, h->name);
            g_ptr_array_add(job->hits, h);
        }
        g_strfreev(lines);
        g_hash_table_unref(installed);

        lower = g_ascii_strdown(job->term, -1);
        g_ptr_array_sort_with_data(job->hits, compare_hits, lower);
        if (job->hits->len > MAX_RESULTS)
            g_ptr_array_set_size(job->hits, MAX_RESULTS);
        g_free(lower);
    }

    g_strfreev(words);
    g_strfreev(envp);
    g_ptr_array_free(argv, TRUE);
    g_ptr_array_free(keep, TRUE);
    g_free(out);
    g_free(iout);

    g_task_return_boolean(task, TRUE);
}

static void
search_done(GObject *src, GAsyncResult *res, gpointer data)
{
    SearchJob *job = g_task_get_task_data(G_TASK(res));
    AppState *s = job->s;
    guint i;
    gchar *msg;

    if (job->gen != s->search_gen)
        return;

    gtk_list_store_clear(s->results_store);

    if (!job->hits || job->hits->len == 0) {
        gtk_stack_set_visible_child_name(GTK_STACK(s->left_stack), "none");
        update_hint(s);
        if (!job->silent) {
            msg = g_strdup_printf("No packages found for “%s”", job->term);
            set_status(s, STATUS_INFO, msg);
            g_free(msg);
        }
        return;
    }

    for (i = 0; i < job->hits->len; i++) {
        SearchHit *h = g_ptr_array_index(job->hits, i);
        gchar *name = g_markup_escape_text(h->name, -1);
        gchar *desc = g_markup_escape_text(h->desc, -1);
        gchar *markup = g_strdup_printf(
            "<b>%s</b>%s\n<span size=\"small\" alpha=\"65%%\">%s</span>",
            name, h->installed ? "  ✓" : "", desc);

        gtk_list_store_insert_with_values(s->results_store, NULL, -1,
                                          COL_NAME, h->name,
                                          COL_MARKUP, markup, -1);
        g_free(name);
        g_free(desc);
        g_free(markup);
    }

    gtk_stack_set_visible_child_name(GTK_STACK(s->left_stack), "results");
    update_hint(s);

    if (!job->silent) {
        msg = g_strdup_printf("%u package%s found — drag one onto Applications "
                              "to install it", job->hits->len,
                              job->hits->len == 1 ? "" : "s");
        set_status(s, STATUS_INFO, msg);
        g_free(msg);
    }
}

static void
run_search(AppState *s, const gchar *term, gboolean silent)
{
    SearchJob *job = g_new0(SearchJob, 1);
    GTask *task;

    s->search_gen++;
    job->s    = s;
    job->gen  = s->search_gen;
    job->term = g_strdup(term);
    job->silent = silent;

    task = g_task_new(NULL, NULL, search_done, NULL);
    g_task_set_task_data(task, job, (GDestroyNotify)search_job_free);
    g_task_run_in_thread(task, search_thread);
    g_object_unref(task);
}

static void
refresh_search_results(AppState *s)
{
    gchar *term = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(s->search_entry))));

    if (*term)
        run_search(s, term, TRUE);
    g_free(term);
}

static void
update_hint(AppState *s)
{
    const gchar *page = gtk_stack_get_visible_child_name(GTK_STACK(s->left_stack));
    const gchar *text;

    if (page && strcmp(page, "results") == 0)
        text = "Drag a package onto the Applications folder to install it";
    else if (page && strcmp(page, "none") == 0)
        text = "Try a different name";
    else if (s->deb_path)
        text = "Drag the app icon onto the Applications folder";
    else
        text = "Drop a .deb file anywhere in this window, or search for a package";

    gtk_label_set_text(GTK_LABEL(s->hint_label), text);
}

static void
on_search_changed(GtkSearchEntry *entry, gpointer data)
{
    AppState *s = data;
    gchar *term = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));

    if (!*term) {
        s->search_gen++;
        gtk_stack_set_visible_child_name(GTK_STACK(s->left_stack), "app");
        update_hint(s);
        if (!s->busy)
            set_status(s, STATUS_INFO, s->deb_path
                       ? "Ready — drag the app icon onto Applications to install"
                       : "Ready — drop a .deb file or search for a package");
    } else {
        run_search(s, term, FALSE);
    }
    g_free(term);
}

static void
on_search_stop(GtkSearchEntry *entry, gpointer data)
{
    gtk_entry_set_text(GTK_ENTRY(entry), "");
}

static void
on_results_drag_data_get(GtkWidget *w, GdkDragContext *ctx,
                         GtkSelectionData *sel, guint info, guint time,
                         gpointer data)
{
    GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected(ts, &model, &iter)) {
        gchar *name = NULL, *payload;

        gtk_tree_model_get(model, &iter, COL_NAME, &name, -1);
        payload = g_strconcat("repo:", name, NULL);
        gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8,
                               (const guchar *)payload, strlen(payload));
        g_free(payload);
        g_free(name);
    }
}

static void
update_info_free(gpointer p)
{
    UpdateInfo *u = p;

    if (!u) return;
    g_free(u->name);
    g_free(u->arch);
    g_free(u->old_ver);
    g_free(u->new_ver);
    g_free(u);
}

static gint
compare_updates(gconstpointer a, gconstpointer b)
{
    const UpdateInfo *ua = *(UpdateInfo * const *)a;
    const UpdateInfo *ub = *(UpdateInfo * const *)b;

    return strcmp(ua->name, ub->name);
}

typedef struct {
    gboolean   open_dialog;
    GPtrArray *list;
} UpdateCheck;

static void
update_check_free(UpdateCheck *c)
{
    if (!c) return;
    if (c->list)
        g_ptr_array_unref(c->list);
    g_free(c);
}

static void
collect_sizes(GPtrArray *list, GHashTable *sizes, gchar **envp)
{
    guint start;

    for (start = 0; start < list->len; start += 150) {
        GPtrArray *argv = g_ptr_array_new();
        guint end = MIN(start + 150, list->len), i;
        gchar *out = NULL;

        g_ptr_array_add(argv, (gpointer)"apt-cache");
        g_ptr_array_add(argv, (gpointer)"show");
        g_ptr_array_add(argv, (gpointer)"--no-all-versions");
        for (i = start; i < end; i++)
            g_ptr_array_add(argv, ((UpdateInfo *)g_ptr_array_index(list, i))->name);
        g_ptr_array_add(argv, NULL);

        if (g_spawn_sync(NULL, (gchar **)argv->pdata, envp,
                         G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL, NULL, &out, NULL, NULL, NULL) && out) {
            gchar **lines = g_strsplit(out, "\n", -1), **p;
            gchar *pkg = NULL, *arch = NULL, *size = NULL;

            for (p = lines;; p++) {
                gboolean last = (*p == NULL);

                if (last || !**p) {
                    if (pkg && size) {
                        if (arch)
                            g_hash_table_replace(sizes,
                                g_strdup_printf("%s/%s", pkg, arch),
                                g_strdup(size));
                        if (!g_hash_table_contains(sizes, pkg))
                            g_hash_table_replace(sizes, g_strdup(pkg),
                                                 g_strdup(size));
                    }
                    g_clear_pointer(&pkg, g_free);
                    g_clear_pointer(&arch, g_free);
                    g_clear_pointer(&size, g_free);
                    if (last) break;
                    continue;
                }
                if (g_str_has_prefix(*p, "Package: "))
                    pkg = g_strdup(*p + 9);
                else if (g_str_has_prefix(*p, "Architecture: "))
                    arch = g_strdup(*p + 14);
                else if (g_str_has_prefix(*p, "Size: "))
                    size = g_strdup(*p + 6);
            }
            g_strfreev(lines);
        }

        g_free(out);
        g_ptr_array_free(argv, TRUE);
    }
}

static void
check_thread(GTask *task, gpointer src, gpointer data,
             GCancellable *cancellable)
{
    UpdateCheck *chk = data;
    gchar *argv[] = { "apt", "list", "--upgradable", NULL };
    gchar **envp = g_environ_setenv(g_get_environ(), "LC_ALL", "C", TRUE);
    gchar *out = NULL;

    chk->list = g_ptr_array_new_with_free_func(update_info_free);

    if (g_spawn_sync(NULL, argv, envp,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, &out, NULL, NULL, NULL) && out) {
        GRegex *re = g_regex_new(
            "^([^/\\s]+)/(\\S+)\\s+(\\S+)\\s+(\\S+)\\s+"
            "\\[upgradable from:\\s*([^\\]]+)\\]", 0, 0, NULL);
        gchar **lines = g_strsplit(out, "\n", -1), **p;

        for (p = lines; *p; p++) {
            GMatchInfo *mi = NULL;

            if (re && g_regex_match(re, *p, 0, &mi)) {
                UpdateInfo *u = g_new0(UpdateInfo, 1);

                u->name    = g_match_info_fetch(mi, 1);
                u->new_ver = g_match_info_fetch(mi, 3);
                u->arch    = g_match_info_fetch(mi, 4);
                u->old_ver = g_match_info_fetch(mi, 5);
                u->size    = -1;
                g_ptr_array_add(chk->list, u);
            }
            if (mi)
                g_match_info_free(mi);
        }
        g_strfreev(lines);
        if (re)
            g_regex_unref(re);
    }

    if (chk->list->len > 0) {
        GHashTable *sizes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, g_free);
        guint i;

        collect_sizes(chk->list, sizes, envp);

        for (i = 0; i < chk->list->len; i++) {
            UpdateInfo *u = g_ptr_array_index(chk->list, i);
            gchar *key = g_strdup_printf("%s/%s", u->name, u->arch);
            const gchar *v = g_hash_table_lookup(sizes, key);

            if (!v)
                v = g_hash_table_lookup(sizes, u->name);
            if (v)
                u->size = g_ascii_strtoll(v, NULL, 10);
            g_free(key);
        }
        g_hash_table_unref(sizes);
        g_ptr_array_sort(chk->list, compare_updates);
    }

    g_free(out);
    g_strfreev(envp);
    g_task_return_boolean(task, TRUE);
}

typedef struct {
    GtkListStore *store;
    GtkWidget    *total_label;
    GtkWidget    *sel_button;
    gboolean      updating;
} UpdateDlg;

static void
refresh_totals(UpdateDlg *d)
{
    GtkTreeModel *m = GTK_TREE_MODEL(d->store);
    GtkTreeIter it;
    guint count = 0;
    gint64 bytes = 0;
    gchar *text, *size;

    if (gtk_tree_model_get_iter_first(m, &it)) {
        do {
            gboolean sel;
            gint64 b;

            gtk_tree_model_get(m, &it, UCOL_SEL, &sel, UCOL_BYTES, &b, -1);
            if (sel) {
                count++;
                if (b > 0)
                    bytes += b;
            }
        } while (gtk_tree_model_iter_next(m, &it));
    }

    if (count) {
        size = g_format_size((guint64)bytes);
        text = g_strdup_printf("Selected: %u · %s", count, size);
        g_free(size);
    } else {
        text = g_strdup("No updates selected");
    }
    gtk_label_set_text(GTK_LABEL(d->total_label), text);
    gtk_widget_set_sensitive(d->sel_button, count > 0);
    g_free(text);
}

static void
on_update_toggled(GtkCellRendererToggle *r, gchar *path, gpointer data)
{
    UpdateDlg *d = data;
    GtkTreeModel *m = GTK_TREE_MODEL(d->store);
    GtkTreeIter it;

    if (gtk_tree_model_get_iter_from_string(m, &it, path)) {
        gboolean cur;

        gtk_tree_model_get(m, &it, UCOL_SEL, &cur, -1);
        gtk_list_store_set(d->store, &it, UCOL_SEL, !cur, -1);
    }
    refresh_totals(d);
}

static void
on_select_all_toggled(GtkToggleButton *btn, gpointer data)
{
    UpdateDlg *d = data;
    GtkTreeModel *m = GTK_TREE_MODEL(d->store);
    GtkTreeIter it;
    gboolean active = gtk_toggle_button_get_active(btn);

    if (gtk_tree_model_get_iter_first(m, &it)) {
        do {
            gtk_list_store_set(d->store, &it, UCOL_SEL, active, -1);
        } while (gtk_tree_model_iter_next(m, &it));
    }
    refresh_totals(d);
}

static GPtrArray *
collect_update_names(GtkListStore *store, gboolean only_selected)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GtkTreeModel *m = GTK_TREE_MODEL(store);
    GtkTreeIter it;

    if (gtk_tree_model_get_iter_first(m, &it)) {
        do {
            gboolean sel;
            gchar *name;

            gtk_tree_model_get(m, &it, UCOL_SEL, &sel, UCOL_NAME, &name, -1);
            if (!only_selected || sel)
                g_ptr_array_add(names, name);
            else
                g_free(name);
        } while (gtk_tree_model_iter_next(m, &it));
    }
    return names;
}

static void
show_updates_dialog(AppState *s)
{
    GtkWidget *dialog, *content, *header, *scroll, *view, *check_all, *total;
    GtkWidget *sel_btn = NULL, *all_btn = NULL;
    GtkCellRenderer *r;
    GtkTreeViewColumn *col;
    UpdateDlg d;
    GPtrArray *list = s->updates;
    guint i, n = list ? list->len : 0;
    gint64 total_bytes = 0;
    gchar *text, *size;
    gint resp;

    dialog = gtk_dialog_new_with_buttons("Updates", GTK_WINDOW(s->window),
                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                 "_Close", GTK_RESPONSE_CLOSE,
                 "_Refresh lists", RESP_REFRESH,
                 NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 480);

    if (n > 0) {
        sel_btn = gtk_dialog_add_button(GTK_DIALOG(dialog),
                                        "Install _Selected", RESP_INSTALL_SEL);
        all_btn = gtk_dialog_add_button(GTK_DIALOG(dialog),
                                        "Install _All", RESP_INSTALL_ALL);
        gtk_style_context_add_class(gtk_widget_get_style_context(all_btn),
                                    "suggested-action");
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), RESP_INSTALL_ALL);
    }

    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    d.store = gtk_list_store_new(UCOL_N, G_TYPE_BOOLEAN, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64);
    d.updating = FALSE;

    for (i = 0; i < n; i++) {
        UpdateInfo *u = g_ptr_array_index(list, i);
        gchar *versions = g_strdup_printf("%s  →  %s", u->old_ver, u->new_ver);
        gchar *sz = u->size >= 0 ? g_format_size((guint64)u->size)
                                 : g_strdup("—");

        if (u->size > 0)
            total_bytes += u->size;
        gtk_list_store_insert_with_values(d.store, NULL, -1,
            UCOL_SEL, FALSE, UCOL_NAME, u->name, UCOL_VERSIONS, versions,
            UCOL_SIZE, sz, UCOL_BYTES, u->size, -1);
        g_free(versions);
        g_free(sz);
    }

    if (n > 0) {
        size = g_format_size((guint64)total_bytes);
        text = g_strdup_printf("<b>%u update%s available</b> · %s to download",
                               n, n == 1 ? "" : "s", size);
        g_free(size);
    } else {
        text = g_strdup("<b>Your system is up to date</b>");
    }
    header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header), text);
    gtk_label_set_xalign(GTK_LABEL(header), 0.0f);
    gtk_box_pack_start(GTK_BOX(content), header, FALSE, FALSE, 0);
    g_free(text);

    total = gtk_label_new("");
    d.total_label = total;
    d.sel_button  = sel_btn;

    if (n > 0) {
        check_all = gtk_check_button_new_with_label("Select all");
        g_signal_connect(check_all, "toggled",
                         G_CALLBACK(on_select_all_toggled), &d);
        gtk_box_pack_start(GTK_BOX(content), check_all, FALSE, FALSE, 0);

        scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),
                                            GTK_SHADOW_IN);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_vexpand(scroll, TRUE);
        gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

        view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d.store));
        gtk_container_add(GTK_CONTAINER(scroll), view);

        r = gtk_cell_renderer_toggle_new();
        g_signal_connect(r, "toggled", G_CALLBACK(on_update_toggled), &d);
        col = gtk_tree_view_column_new_with_attributes("", r,
                                                       "active", UCOL_SEL, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

        r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END,
                     "width-chars", 26, "max-width-chars", 30, NULL);
        col = gtk_tree_view_column_new_with_attributes("Package", r,
                                                       "text", UCOL_NAME, NULL);
        gtk_tree_view_column_set_expand(col, FALSE);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_sort_column_id(col, UCOL_NAME);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

        r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END,
                     "max-width-chars", 60, NULL);
        col = gtk_tree_view_column_new_with_attributes("Version", r,
                                                       "text", UCOL_VERSIONS, NULL);
        gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

        r = gtk_cell_renderer_text_new();
        g_object_set(r, "xalign", 1.0f, NULL);
        col = gtk_tree_view_column_new_with_attributes("Download", r,
                                                       "text", UCOL_SIZE, NULL);
        gtk_tree_view_column_set_alignment(col, 1.0f);
        gtk_tree_view_column_set_sort_column_id(col, UCOL_BYTES);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

        gtk_label_set_xalign(GTK_LABEL(total), 0.0f);
        gtk_box_pack_start(GTK_BOX(content), total, FALSE, FALSE, 0);
    } else {
        GtkWidget *hint = gtk_label_new(
            "Use “Refresh lists” to check the repositories for new updates.");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
        gtk_box_pack_start(GTK_BOX(content), hint, FALSE, FALSE, 0);
    }

    gtk_widget_show_all(dialog);
    if (n > 0)
        refresh_totals(&d);

    resp = gtk_dialog_run(GTK_DIALOG(dialog));

    {
        GPtrArray *names = NULL;

        if (resp == RESP_INSTALL_SEL)
            names = collect_update_names(d.store, TRUE);
        else if (resp == RESP_INSTALL_ALL)
            names = collect_update_names(d.store, FALSE);

        gtk_widget_destroy(dialog);
        g_object_unref(d.store);

        if (resp == RESP_REFRESH) {
            begin_privileged(s, "refresh the package lists", SCRIPT_REFRESH,
                             NULL, "Refreshing package lists…",
                             refresh_done, NULL, NULL);
        } else if (names && names->len > 0) {
            gchar *busy = g_strdup_printf("Installing %u update%s…",
                                          names->len,
                                          names->len == 1 ? "" : "s");

            begin_privileged(s, "install the updates", SCRIPT_UPDATE, names,
                             busy, update_done,
                             GUINT_TO_POINTER(names->len), NULL);
            g_free(busy);
        }
        if (names)
            g_ptr_array_free(names, TRUE);
    }
}

static void
update_badge(AppState *s)
{
    guint n = s->updates ? s->updates->len : 0;
    GtkStyleContext *ctx = gtk_widget_get_style_context(s->updates_btn);
    gchar *label;

    if (n > 0) {
        label = g_strdup_printf("Updates (%u)", n);
        gtk_style_context_add_class(ctx, "has-updates");
    } else {
        label = g_strdup("Updates");
        gtk_style_context_remove_class(ctx, "has-updates");
    }
    gtk_button_set_label(GTK_BUTTON(s->updates_btn), label);
    g_free(label);
}

static void
check_done(GObject *src, GAsyncResult *res, gpointer data)
{
    AppState *s = data;
    UpdateCheck *chk = g_task_get_task_data(G_TASK(res));

    s->updates_checking = FALSE;
    if (chk->open_dialog)
        set_busy(s, FALSE);

    if (s->updates)
        g_ptr_array_unref(s->updates);
    s->updates = chk->list;
    chk->list  = NULL;
    update_badge(s);

    if (chk->open_dialog) {
        set_status(s, STATUS_INFO, s->updates && s->updates->len
                   ? "Updates available"
                   : "Your system is up to date");
        show_updates_dialog(s);
    }
}

static void
check_updates_async(AppState *s, gboolean open_dialog)
{
    UpdateCheck *chk;
    GTask *task;

    if (open_dialog) {
        if (s->busy) {
            set_status(s, STATUS_INFO,
                       "Please wait — an operation is in progress…");
            return;
        }
        set_busy(s, TRUE);
        set_status(s, STATUS_INFO, "Checking for updates…");
    } else if (s->updates_checking) {
        return;
    }
    s->updates_checking = TRUE;

    chk = g_new0(UpdateCheck, 1);
    chk->open_dialog = open_dialog;

    task = g_task_new(NULL, NULL, check_done, s);
    g_task_set_task_data(task, chk, (GDestroyNotify)update_check_free);
    g_task_run_in_thread(task, check_thread);
    g_object_unref(task);
}

static void
on_updates_clicked(GtkButton *btn, gpointer data)
{
    check_updates_async(data, TRUE);
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
        const guchar *raw = gtk_selection_data_get_data(sel);
        gint len = gtk_selection_data_get_length(sel);
        gchar *payload = (raw && len > 0) ? g_strndup((const gchar *)raw, len)
                                          : NULL;

        if (payload && s->busy) {
            set_status(s, STATUS_INFO,
                       "Please wait — an operation is in progress…");
        } else if (payload && !s->prompt_pending) {
            gboolean accepted = FALSE;

            if (g_str_has_prefix(payload, "repo:") && payload[5]) {
                s->pending_kind = PENDING_REPO;
                g_free(s->pending_repo);
                s->pending_repo = g_strdup(payload + 5);
                accepted = TRUE;
            } else if (strcmp(payload, "deb") == 0 && s->deb_path) {
                s->pending_kind = PENDING_DEB;
                accepted = TRUE;
            }
            if (accepted) {
                s->prompt_pending = TRUE;
                g_idle_add(prompt_install_idle, s);
            }
        }
        g_free(payload);
    }
}

static void
on_app_drag_data_get(GtkWidget *w, GdkDragContext *ctx,
                     GtkSelectionData *sel, guint info, guint time,
                     gpointer data)
{
    gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8,
                           (const guchar *)"deb", 3);
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
    ".cart-window entry {"
    "  background-image: none; background-color: rgba(255,255,255,0.10);"
    "  color: #f2f2f5; border: 1px solid rgba(255,255,255,0.16);"
    "  border-radius: 8px; box-shadow: none; padding: 4px 8px; }"
    ".cart-window entry image { color: rgba(255,255,255,0.65); }"
    ".cart-window button.updates {"
    "  background-image: none; background-color: rgba(255,255,255,0.12);"
    "  border: 1px solid rgba(255,255,255,0.18); border-radius: 8px;"
    "  box-shadow: none; padding: 4px 12px; }"
    ".cart-window button.updates image { color: #ffffff; }"
    ".cart-window button.updates:hover { background-color: rgba(255,255,255,0.22); }"
    ".cart-window button.updates.has-updates {"
    "  background-color: rgba(52,152,219,0.60); border-color: rgba(120,190,255,0.7); }"
    ".cart-window scrolledwindow {"
    "  border: 1px solid rgba(255,255,255,0.12); border-radius: 8px; }"
    ".cart-window treeview.view {"
    "  background-color: rgba(0,0,0,0.30); color: #ececf1; }"
    ".cart-window treeview.view:selected, .cart-window treeview.view:selected:focus {"
    "  background-color: rgba(70,150,255,0.55); color: #ffffff; }"
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

static const gchar *
pick_icon_name(const gchar *candidates[])
{
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    gint i;

    for (i = 0; candidates[i]; i++)
        if (gtk_icon_theme_has_icon(theme, candidates[i]))
            return candidates[i];
    return candidates[0];
}

static void
build_ui(AppState *s)
{
    GtkCssProvider *provider;
    GtkWidget *vbox, *topbar, *center, *row, *arrow, *msg_area;
    GtkWidget *scroll, *none_label;
    GtkWidget *folder_img, *folder_name, *folder_sub;
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GdkPixbuf *pb;
    const gchar *folder_names[] = {
        "folder-templates", "folder-applications", "folder", NULL
    };
    const gchar *update_icons[] = {
        "software-update-available", "software-update-available-symbolic",
        "system-software-update", "view-refresh", NULL
    };

    s->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(s->window), APP_NAME);
    gtk_window_set_position(GTK_WINDOW(s->window), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable(GTK_WINDOW(s->window), FALSE);
    gtk_widget_set_size_request(s->window, 700, 450);
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

    topbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(topbar), 12);
    gtk_box_pack_start(GTK_BOX(vbox), topbar, FALSE, FALSE, 0);

    s->search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(s->search_entry, 280, -1);
    gtk_entry_set_placeholder_text(GTK_ENTRY(s->search_entry),
                                   "Search packages");
    gtk_box_pack_start(GTK_BOX(topbar), s->search_entry, FALSE, FALSE, 0);

    s->updates_btn = gtk_button_new_with_label("Updates");
    gtk_style_context_add_class(gtk_widget_get_style_context(s->updates_btn),
                                "updates");
    gtk_button_set_image(GTK_BUTTON(s->updates_btn),
        gtk_image_new_from_icon_name(pick_icon_name(update_icons),
                                     GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(s->updates_btn), TRUE);
    gtk_widget_set_tooltip_text(s->updates_btn, "Check for available updates");
    gtk_box_pack_end(GTK_BOX(topbar), s->updates_btn, FALSE, FALSE, 0);

    center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_valign(center, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), center, TRUE, TRUE, 0);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 28);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(center), row, FALSE, FALSE, 0);

    s->left_stack = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(s->left_stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(s->left_stack), TRUE);
    gtk_box_pack_start(GTK_BOX(row), s->left_stack, FALSE, FALSE, 0);

    make_slot(&s->app_slot, &s->app_image, &s->name_label, &s->sub_label);
    gtk_stack_add_named(GTK_STACK(s->left_stack), s->app_slot, "app");

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),
                                        GTK_SHADOW_NONE);
    gtk_widget_set_size_request(scroll, 340, 200);

    s->results_store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING);
    s->results_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s->results_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(s->results_view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(s->results_view), FALSE);

    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END,
                 "ypad", 4, NULL);
    column = gtk_tree_view_column_new_with_attributes("Package", renderer,
                                                      "markup", COL_MARKUP, NULL);
    gtk_tree_view_column_set_expand(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(s->results_view), column);

    gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(s->results_view),
                                           GDK_BUTTON1_MASK, cart_targets,
                                           G_N_ELEMENTS(cart_targets),
                                           GDK_ACTION_COPY);
    g_signal_connect(s->results_view, "drag-data-get",
                     G_CALLBACK(on_results_drag_data_get), s);

    gtk_container_add(GTK_CONTAINER(scroll), s->results_view);
    gtk_stack_add_named(GTK_STACK(s->left_stack), scroll, "results");

    none_label = gtk_label_new("No packages found");
    gtk_style_context_add_class(gtk_widget_get_style_context(none_label), "hint");
    gtk_widget_set_size_request(none_label, 340, -1);
    gtk_stack_add_named(GTK_STACK(s->left_stack), none_label, "none");

    arrow = gtk_label_new("➔");
    gtk_style_context_add_class(gtk_widget_get_style_context(arrow), "arrow");
    gtk_widget_set_valign(arrow, GTK_ALIGN_CENTER);
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

    s->hint_label = gtk_label_new("");
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

    g_signal_connect(s->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), s);
    g_signal_connect(s->search_entry, "stop-search",
                     G_CALLBACK(on_search_stop), s);
    g_signal_connect(s->updates_btn, "clicked",
                     G_CALLBACK(on_updates_clicked), s);

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

    update_hint(s);
    set_status(s, STATUS_INFO, "Ready — drop a .deb file or search for a package");
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
    gtk_window_set_focus(GTK_WINDOW(s->window), NULL);

    if (argc > 1) {
        gchar *abs = g_canonicalize_filename(argv[1], NULL);
        load_deb(s, abs);
        g_free(abs);
    }

    check_updates_async(s, FALSE);

    gtk_main();

    g_clear_object(&s->app_pixbuf);
    if (s->updates)
        g_ptr_array_unref(s->updates);
    g_free(s->deb_path);
    g_free(s->display_name);
    g_free(s->pkg_name);
    g_free(s->version);
    g_free(s->pending_repo);
    g_free(s);
    return 0;
}
