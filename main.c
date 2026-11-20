#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gst/gst.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *canvas_area;
    GtkWidget *anim_image;
    GtkWidget *video_area;

    GMutex lock;
    GdkPixbuf *orig_pixbuf;
    GdkPixbufAnimation *anim;
    gboolean is_anim;
    gboolean is_video;
    gboolean closing;

    int win_w;
    int win_h;
    double zoom;       /* 0.0 = fit to window, >0 = explicit zoom factor */
    double pan_x;      /* screen-space offset in pixels */
    double pan_y;

    gboolean dragging;
    double drag_last_x;
    double drag_last_y;

    GstElement *pipeline;
} App;

typedef struct {
    App *app;
    char *path;
} LoadJob;

static gboolean is_webm(const char *path) {
    const char *ext = strrchr(path, '.');
    return ext && g_ascii_strcasecmp(ext, ".webm") == 0;
}

static double compute_fit_scale(int orig_w, int orig_h, int area_w, int area_h) {
    if (orig_w <= 0 || orig_h <= 0 || area_w <= 1 || area_h <= 1) return 1.0;
    double rw = (double)area_w / orig_w;
    double rh = (double)area_h / orig_h;
    double s = (rw < rh) ? rw : rh;
    return (s > 1.0) ? 1.0 : s;
}

static double compute_scale_locked(App *app, int area_w, int area_h) {
    if (!app->orig_pixbuf) return 1.0;
    if (app->zoom > 0.0) return app->zoom;
    int ow = gdk_pixbuf_get_width(app->orig_pixbuf);
    int oh = gdk_pixbuf_get_height(app->orig_pixbuf);
    return compute_fit_scale(ow, oh, area_w, area_h);
}

static void clamp_pan_locked(App *app, int area_w, int area_h, double scale) {
    if (!app->orig_pixbuf) return;
    int ow = gdk_pixbuf_get_width(app->orig_pixbuf);
    int oh = gdk_pixbuf_get_height(app->orig_pixbuf);

    double disp_w = ow * scale;
    double disp_h = oh * scale;

    /* Allow panning at any zoom, keeping at least 64px of the image inside viewport */
    double margin_x = (disp_w > 128.0) ? (disp_w * 0.5 - 64.0) : (disp_w * 0.5);
    if (margin_x < 0.0) margin_x = 0.0;
    double max_x = area_w * 0.5 + margin_x;
    if (app->pan_x < -max_x) app->pan_x = -max_x;
    if (app->pan_x >  max_x) app->pan_x =  max_x;

    double margin_y = (disp_h > 128.0) ? (disp_h * 0.5 - 64.0) : (disp_h * 0.5);
    if (margin_y < 0.0) margin_y = 0.0;
    double max_y = area_h * 0.5 + margin_y;
    if (app->pan_y < -max_y) app->pan_y = -max_y;
    if (app->pan_y >  max_y) app->pan_y =  max_y;
}

static void set_drag_cursor(App *app, gboolean grabbing) {
    if (!app->window) return;
    GdkWindow *gdk_win = gtk_widget_get_window(app->window);
    if (!gdk_win) return;

    GdkDisplay *display = gdk_window_get_display(gdk_win);
    GdkCursor *cursor = grabbing
        ? gdk_cursor_new_from_name(display, "grabbing")
        : NULL;
    gdk_window_set_cursor(gdk_win, cursor);
    if (cursor) g_object_unref(cursor);
}

static void redraw_canvas(App *app) {
    g_mutex_lock(&app->lock);
    gboolean closing = app->closing;
    GtkWidget *area = app->canvas_area;
    g_mutex_unlock(&app->lock);

    if (!closing && area) {
        gtk_widget_queue_draw(area);
    }
}

static gboolean on_canvas_draw(GtkWidget *widget, cairo_t *cr, App *app) {
    int area_w = gtk_widget_get_allocated_width(widget);
    int area_h = gtk_widget_get_allocated_height(widget);
    if (area_w <= 1) area_w = app->win_w;
    if (area_h <= 1) area_h = app->win_h;

    cairo_set_source_rgb(cr, 0.094, 0.094, 0.094); /* #181818 */
    cairo_paint(cr);

    g_mutex_lock(&app->lock);
    if (app->closing || !app->orig_pixbuf) {
        g_mutex_unlock(&app->lock);
        return TRUE;
    }
    GdkPixbuf *orig = g_object_ref(app->orig_pixbuf);
    int ow = gdk_pixbuf_get_width(orig);
    int oh = gdk_pixbuf_get_height(orig);
    double scale = compute_scale_locked(app, area_w, area_h);
    clamp_pan_locked(app, area_w, area_h, scale);
    double pan_x = app->pan_x;
    double pan_y = app->pan_y;
    g_mutex_unlock(&app->lock);

    double disp_w = ow * scale;
    double disp_h = oh * scale;
    double x = (area_w - disp_w) * 0.5 + pan_x;
    double y = (area_h - disp_h) * 0.5 + pan_y;

    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, orig, 0.0, 0.0);

    cairo_pattern_t *pat = cairo_get_source(cr);
    if (pat) {
        cairo_pattern_set_filter(pat, scale > 1.5 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_BILINEAR);
    }

    cairo_paint(cr);
    cairo_restore(cr);

    g_object_unref(orig);
    return TRUE;
}

static void set_zoom_at(App *app, double new_zoom, double focus_x, double focus_y) {
    g_mutex_lock(&app->lock);
    if (app->is_video || app->is_anim || !app->orig_pixbuf) {
        g_mutex_unlock(&app->lock);
        return;
    }

    int ow = gdk_pixbuf_get_width(app->orig_pixbuf);
    int oh = gdk_pixbuf_get_height(app->orig_pixbuf);
    int area_w = app->canvas_area ? gtk_widget_get_allocated_width(app->canvas_area) : app->win_w;
    int area_h = app->canvas_area ? gtk_widget_get_allocated_height(app->canvas_area) : app->win_h;
    if (area_w <= 1) area_w = app->win_w;
    if (area_h <= 1) area_h = app->win_h;

    if (new_zoom <= 0.0) {
        app->zoom = 0.0;
        app->pan_x = 0.0;
        app->pan_y = 0.0;
    } else {
        if (new_zoom < 0.05) new_zoom = 0.05;
        if (new_zoom > 32.0) new_zoom = 32.0;

        double old_scale = compute_scale_locked(app, area_w, area_h);
        double old_left = (area_w - ow * old_scale) * 0.5 + app->pan_x;
        double old_top  = (area_h - oh * old_scale) * 0.5 + app->pan_y;

        double img_x = (focus_x - old_left) / old_scale;
        double img_y = (focus_y - old_top)  / old_scale;

        double new_left = focus_x - img_x * new_zoom;
        double new_top  = focus_y - img_y * new_zoom;

        app->zoom = new_zoom;
        app->pan_x = new_left - (area_w - ow * new_zoom) * 0.5;
        app->pan_y = new_top  - (area_h - oh * new_zoom) * 0.5;
        clamp_pan_locked(app, area_w, area_h, new_zoom);
    }
    g_mutex_unlock(&app->lock);

    redraw_canvas(app);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

static gboolean on_image_loaded_ui(gpointer user_data) {
    LoadJob *job = user_data;
    App *app = job->app;

    g_mutex_lock(&app->lock);
    gboolean closing = app->closing;
    gboolean is_anim = app->is_anim;
    GdkPixbufAnimation *anim = app->anim ? g_object_ref(app->anim) : NULL;
    GdkPixbuf *orig = app->orig_pixbuf ? g_object_ref(app->orig_pixbuf) : NULL;
    g_mutex_unlock(&app->lock);

    if (!closing) {
        if (is_anim && anim) {
            if (app->canvas_area) {
                gtk_widget_hide(app->canvas_area);
            }
            app->anim_image = gtk_image_new_from_animation(anim);
            gtk_container_add(GTK_CONTAINER(app->window), app->anim_image);
            int aw = gdk_pixbuf_animation_get_width(anim);
            int ah = gdk_pixbuf_animation_get_height(anim);
            gtk_window_resize(GTK_WINDOW(app->window),
                aw > 1920 ? 1920 : (aw < 200 ? 200 : aw),
                ah > 1080 ? 1080 : (ah < 200 ? 200 : ah));
            gtk_widget_show_all(app->window);
        } else if (orig) {
            int ow = gdk_pixbuf_get_width(orig);
            int oh = gdk_pixbuf_get_height(orig);
            int dw = ow > 1280 ? 1280 : (ow < 200 ? 200 : ow);
            int dh = oh > 720  ? 720  : (oh < 200 ? 200 : oh);
            gtk_window_resize(GTK_WINDOW(app->window), dw, dh);
            g_mutex_lock(&app->lock);
            app->win_w = dw;
            app->win_h = dh;
            g_mutex_unlock(&app->lock);
            redraw_canvas(app);
        }
    }

    if (anim) g_object_unref(anim);
    if (orig) g_object_unref(orig);
    g_free(job->path);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer load_worker_thread(gpointer user_data) {
    LoadJob *job = user_data;
    App *app = job->app;
    GError *err = NULL;

    GdkPixbufAnimation *anim = gdk_pixbuf_animation_new_from_file(job->path, &err);
    if (!anim) {
        g_printerr("Ошибка загрузки: %s\n", err ? err->message : "неизвестная ошибка");
        if (err) g_error_free(err);
        g_free(job->path);
        g_free(job);
        return NULL;
    }

    gboolean is_static = gdk_pixbuf_animation_is_static_image(anim);
    GdkPixbuf *orig = is_static ? g_object_ref(gdk_pixbuf_animation_get_static_image(anim)) : NULL;

    g_mutex_lock(&app->lock);
    if (app->closing) {
        g_mutex_unlock(&app->lock);
        if (orig) g_object_unref(orig);
        g_object_unref(anim);
        g_free(job->path);
        g_free(job);
        return NULL;
    }

    app->is_anim = !is_static;
    if (is_static) {
        app->orig_pixbuf = orig;
        g_object_unref(anim);
    } else {
        app->anim = anim;
    }
    g_mutex_unlock(&app->lock);

    g_idle_add(on_image_loaded_ui, job);
    return NULL;
}

#pragma GCC diagnostic pop

static gboolean on_configure(GtkWidget *widget, GdkEventConfigure *event, App *app) {
    (void)widget;
    g_mutex_lock(&app->lock);
    app->win_w = event->width;
    app->win_h = event->height;
    g_mutex_unlock(&app->lock);

    if (!app->is_video) {
        redraw_canvas(app);
    }
    return FALSE;
}

static gboolean on_scroll(GtkWidget *widget, GdkEventScroll *event, App *app) {
    (void)widget;
    if (app->is_video || app->is_anim) return FALSE;

    int area_w = app->canvas_area ? gtk_widget_get_allocated_width(app->canvas_area) : app->win_w;
    int area_h = app->canvas_area ? gtk_widget_get_allocated_height(app->canvas_area) : app->win_h;
    if (area_w <= 1) area_w = app->win_w;
    if (area_h <= 1) area_h = app->win_h;

    g_mutex_lock(&app->lock);
    double cur = compute_scale_locked(app, area_w, area_h);
    g_mutex_unlock(&app->lock);

    double factor = 1.15;
    double next = cur;

    if (event->direction == GDK_SCROLL_UP) {
        next = cur * factor;
    } else if (event->direction == GDK_SCROLL_DOWN) {
        next = cur / factor;
    } else if (event->direction == GDK_SCROLL_SMOOTH) {
        double dx = 0.0, dy = 0.0;
        gdk_event_get_scroll_deltas((GdkEvent *)event, &dx, &dy);
        if (dy < 0.0) next = cur * pow(factor, -dy);
        else if (dy > 0.0) next = cur / pow(factor, dy);
    }

    set_zoom_at(app, next, event->x, event->y);
    return TRUE;
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, App *app) {
    (void)widget;
    if (event->button == 1 && !app->is_video && !app->is_anim) {
        app->dragging = TRUE;
        app->drag_last_x = event->x;
        app->drag_last_y = event->y;
        set_drag_cursor(app, TRUE);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_button_release(GtkWidget *widget, GdkEventButton *event, App *app) {
    (void)widget;
    if (event->button == 1) {
        if (app->dragging) {
            app->dragging = FALSE;
            set_drag_cursor(app, FALSE);
        }
        return TRUE;
    }
    return FALSE;
}

static gboolean on_motion_notify(GtkWidget *widget, GdkEventMotion *event, App *app) {
    (void)widget;
    if (!app->dragging || app->is_video || app->is_anim) return FALSE;

    double dx = event->x - app->drag_last_x;
    double dy = event->y - app->drag_last_y;
    app->drag_last_x = event->x;
    app->drag_last_y = event->y;

    int area_w = app->canvas_area ? gtk_widget_get_allocated_width(app->canvas_area) : app->win_w;
    int area_h = app->canvas_area ? gtk_widget_get_allocated_height(app->canvas_area) : app->win_h;
    if (area_w <= 1) area_w = app->win_w;
    if (area_h <= 1) area_h = app->win_h;

    g_mutex_lock(&app->lock);
    double scale = compute_scale_locked(app, area_w, area_h);
    app->pan_x += dx;
    app->pan_y += dy;
    clamp_pan_locked(app, area_w, area_h, scale);
    g_mutex_unlock(&app->lock);

    redraw_canvas(app);
    return TRUE;
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, App *app) {
    (void)widget;
    if (event->keyval == GDK_KEY_Escape || event->keyval == GDK_KEY_q) {
        gtk_main_quit();
        return TRUE;
    }

    if (app->is_video && event->keyval == GDK_KEY_space && app->pipeline) {
        GstState state;
        gst_element_get_state(app->pipeline, &state, NULL, 0);
        gst_element_set_state(app->pipeline,
            state == GST_STATE_PLAYING ? GST_STATE_PAUSED : GST_STATE_PLAYING);
        return TRUE;
    }

    if (!app->is_video && !app->is_anim) {
        int area_w = app->canvas_area ? gtk_widget_get_allocated_width(app->canvas_area) : app->win_w;
        int area_h = app->canvas_area ? gtk_widget_get_allocated_height(app->canvas_area) : app->win_h;
        if (area_w <= 1) area_w = app->win_w;
        if (area_h <= 1) area_h = app->win_h;

        g_mutex_lock(&app->lock);
        double cur = compute_scale_locked(app, area_w, area_h);
        g_mutex_unlock(&app->lock);

        if (event->keyval == GDK_KEY_plus || event->keyval == GDK_KEY_equal || event->keyval == GDK_KEY_KP_Add) {
            set_zoom_at(app, cur * 1.2, area_w * 0.5, area_h * 0.5);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_minus || event->keyval == GDK_KEY_KP_Subtract) {
            set_zoom_at(app, cur / 1.2, area_w * 0.5, area_h * 0.5);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_0 || event->keyval == GDK_KEY_r) {
            set_zoom_at(app, 0.0, area_w * 0.5, area_h * 0.5);
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, App *app) {
    (void)bus;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS && app->pipeline) {
        gst_element_seek_simple(app->pipeline, GST_FORMAT_TIME,
            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
    }
    return TRUE;
}

static void setup_video(App *app, const char *path) {
    char *uri = g_filename_to_uri(path, NULL, NULL);
    app->pipeline = gst_element_factory_make("playbin", "player");
    GstElement *sink = gst_element_factory_make("gtksink", "sink");

    if (!app->pipeline || !sink) {
        g_printerr("Не удалось инициализировать GStreamer (нужен gst-plugins-good/base)\n");
        g_free(uri);
        return;
    }

    g_object_get(sink, "widget", &app->video_area, NULL);
    g_object_set(app->pipeline, "uri", uri, "video-sink", sink, NULL);
    g_free(uri);

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(app->pipeline));
    gst_bus_add_watch(bus, (GstBusFunc)on_bus_message, app);
    gst_object_unref(bus);

    gtk_container_add(GTK_CONTAINER(app->window), app->video_area);
    gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
}

static void setup_image_async(App *app, const char *path) {
    app->canvas_area = gtk_drawing_area_new();
    gtk_widget_add_events(app->canvas_area,
        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK);

    g_signal_connect(app->canvas_area, "draw", G_CALLBACK(on_canvas_draw), app);
    g_signal_connect(app->canvas_area, "scroll-event", G_CALLBACK(on_scroll), app);
    g_signal_connect(app->canvas_area, "button-press-event", G_CALLBACK(on_button_press), app);
    g_signal_connect(app->canvas_area, "button-release-event", G_CALLBACK(on_button_release), app);
    g_signal_connect(app->canvas_area, "motion-notify-event", G_CALLBACK(on_motion_notify), app);
    gtk_container_add(GTK_CONTAINER(app->window), app->canvas_area);

    LoadJob *job = g_new0(LoadJob, 1);
    job->app = app;
    job->path = g_strdup(path);

    GThread *t = g_thread_try_new("mg-load", load_worker_thread, job, NULL);
    if (!t) {
        load_worker_thread(job);
    } else {
        g_thread_unref(t);
    }
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    if (argc < 2) {
        g_printerr("Использование: %s <файл>\n", argv[0]);
        return 1;
    }

    char *abs_path = realpath(argv[1], NULL);
    if (!abs_path) {
        perror("realpath");
        return 1;
    }

    gtk_init(&argc, &argv);
    gst_init(&argc, &argv);

    App app = {0};
    g_mutex_init(&app.lock);
    app.win_w = 800;
    app.win_h = 600;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), argv[1]);
    gtk_window_set_default_size(GTK_WINDOW(app.window), app.win_w, app.win_h);

    gtk_widget_add_events(app.window,
        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "window { background-color: #181818; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(css);

    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_key_press), &app);
    g_signal_connect(app.window, "configure-event", G_CALLBACK(on_configure), &app);
    g_signal_connect(app.window, "scroll-event", G_CALLBACK(on_scroll), &app);
    g_signal_connect(app.window, "button-press-event", G_CALLBACK(on_button_press), &app);
    g_signal_connect(app.window, "button-release-event", G_CALLBACK(on_button_release), &app);
    g_signal_connect(app.window, "motion-notify-event", G_CALLBACK(on_motion_notify), &app);

    app.is_video = is_webm(abs_path);
    if (app.is_video) {
        setup_video(&app, abs_path);
    } else {
        setup_image_async(&app, abs_path);
    }

    gtk_widget_show_all(app.window);
    gtk_main();

    g_mutex_lock(&app.lock);
    app.closing = TRUE;
    g_mutex_unlock(&app.lock);

    if (app.pipeline) {
        gst_element_set_state(app.pipeline, GST_STATE_NULL);
        gst_object_unref(app.pipeline);
    }
    if (app.orig_pixbuf) g_object_unref(app.orig_pixbuf);
    if (app.anim) g_object_unref(app.anim);
    g_mutex_clear(&app.lock);
    free(abs_path);

    return 0;
}
