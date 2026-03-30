#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <sys/kd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <csignal>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "lv_conf.h"
#include <lvgl/lvgl.h>

struct DrmBuffer {
    uint32_t handle = 0;
    uint32_t pitch = 0;
    uint32_t fb_id = 0;
    uint64_t size = 0;
    uint8_t *map = nullptr;
};

struct Framebuffer {
    int fd = -1;
    drmModeModeInfo mode{};
    drmModeCrtc *original_crtc = nullptr;
    uint32_t conn_id = 0;
    uint32_t crtc_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t visible_buffer = 0;
    uint32_t prepared_buffer = UINT32_MAX;
    bool page_flip_pending = false;
    DrmBuffer buffers[2]{};
};

static Framebuffer fb;
static uint8_t *buf1 = nullptr;
static uint8_t *buf2 = nullptr;
static int tty_fd = -1;
static int old_kd_mode = KD_TEXT;
static termios old_termios{};
static bool old_termios_valid = false;
static volatile std::sig_atomic_t exit_requested = 0;
static uint32_t logical_scr_w = 0;
static uint32_t logical_scr_h = 0;
static lv_obj_t *root_screen = nullptr;
static lv_obj_t *touch_crosshair_h = nullptr;
static lv_obj_t *touch_crosshair_v = nullptr;
static lv_obj_t *touch_crosshair_dot = nullptr;
static bool bg_pressed_visual = false;

struct Touchscreen {
    int fd = -1;
    int min_x = 0;
    int max_x = 4095;
    int min_y = 0;
    int max_y = 4095;
    int raw_x = 0;
    int raw_y = 0;
    bool pressed = false;
    bool has_x = false;
    bool has_y = false;
    bool invert_x = false;
    bool invert_y = false;
    bool swap_xy = false;
};

static Touchscreen ts;

/*
 * Final fine-tune after fixing the HDMI/Linux mode handoff. With the
 * panel now rendering sharply at native timing, the 200 px rulers measure
 * about 3.96 cm horizontally and 3.70 cm vertically, so only a small
 * residual X compression is needed.
 */
static constexpr bool kEnableAspectCorrection = true;
static constexpr uint32_t kPixelAspectX = 198;
static constexpr uint32_t kPixelAspectY = 185;

static_assert(LV_COLOR_DEPTH == 16, "This framebuffer backend currently expects LVGL to render RGB565.");

static void request_exit(int) {
    exit_requested = 1;
}

static void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = request_exit;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
}

static bool vt_enter_graphics(const char *tty = "/dev/tty1") {
    tty_fd = open(tty, O_RDWR | O_CLOEXEC);
    if (tty_fd < 0) {
        std::perror("open tty");
        return false;
    }

    if (ioctl(tty_fd, KDGETMODE, &old_kd_mode) < 0) {
        std::perror("ioctl KDGETMODE");
        close(tty_fd);
        tty_fd = -1;
        return false;
    }

    if (tcgetattr(tty_fd, &old_termios) == 0) {
        old_termios_valid = true;
        termios raw = old_termios;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(tty_fd, TCSANOW, &raw) < 0) {
            std::perror("tcsetattr");
        }
    } else {
        std::perror("tcgetattr");
    }

    if (ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) < 0) {
        std::perror("ioctl KDSETMODE KD_GRAPHICS");
        if (old_termios_valid) {
            tcsetattr(tty_fd, TCSANOW, &old_termios);
            old_termios_valid = false;
        }
        close(tty_fd);
        tty_fd = -1;
        return false;
    }

    return true;
}

static bool touch_init(const char *dev = "/dev/input/event0") {
    ts.fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (ts.fd < 0) {
        std::perror("open touch input");
        return false;
    }

    input_absinfo absinfo {};
    if (ioctl(ts.fd, EVIOCGABS(ABS_X), &absinfo) == 0) {
        ts.min_x = absinfo.minimum;
        ts.max_x = absinfo.maximum;
    } else if (ioctl(ts.fd, EVIOCGABS(ABS_MT_POSITION_X), &absinfo) == 0) {
        ts.min_x = absinfo.minimum;
        ts.max_x = absinfo.maximum;
    }

    if (ioctl(ts.fd, EVIOCGABS(ABS_Y), &absinfo) == 0) {
        ts.min_y = absinfo.minimum;
        ts.max_y = absinfo.maximum;
    } else if (ioctl(ts.fd, EVIOCGABS(ABS_MT_POSITION_Y), &absinfo) == 0) {
        ts.min_y = absinfo.minimum;
        ts.max_y = absinfo.maximum;
    }

    return true;
}

static void touch_deinit() {
    if (ts.fd >= 0) {
        close(ts.fd);
        ts.fd = -1;
    }
}

static void vt_leave_graphics() {
    if (tty_fd < 0) {
        return;
    }

    if (old_termios_valid) {
        tcsetattr(tty_fd, TCSANOW, &old_termios);
        old_termios_valid = false;
    }

    ioctl(tty_fd, KDSETMODE, old_kd_mode);

    static constexpr char clear_and_home[] = "\033[2J\033[H";
    write(tty_fd, clear_and_home, sizeof(clear_and_home) - 1);

    close(tty_fd);
    tty_fd = -1;
}

static bool drm_find_connector_and_mode(drmModeRes *resources, drmModeConnector **out_connector) {
    for (int index = 0; index < resources->count_connectors; index++) {
        drmModeConnector *connector = drmModeGetConnector(fb.fd, resources->connectors[index]);
        if (!connector) {
            continue;
        }

        if (connector->connection != DRM_MODE_CONNECTED || connector->count_modes == 0) {
            drmModeFreeConnector(connector);
            continue;
        }

        int preferred_mode = 0;
        for (int mode_index = 0; mode_index < connector->count_modes; mode_index++) {
            if (connector->modes[mode_index].type & DRM_MODE_TYPE_PREFERRED) {
                preferred_mode = mode_index;
                break;
            }
        }

        fb.conn_id = connector->connector_id;
        fb.mode = connector->modes[preferred_mode];
        fb.width = fb.mode.hdisplay;
        fb.height = fb.mode.vdisplay;
        *out_connector = connector;
        return true;
    }

    return false;
}

static bool drm_find_crtc(drmModeRes *resources, drmModeConnector *connector) {
    if (connector->encoder_id != 0) {
        drmModeEncoder *encoder = drmModeGetEncoder(fb.fd, connector->encoder_id);
        if (encoder) {
            fb.crtc_id = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
            if (fb.crtc_id != 0) {
                return true;
            }
        }
    }

    for (int enc_index = 0; enc_index < connector->count_encoders; enc_index++) {
        drmModeEncoder *encoder = drmModeGetEncoder(fb.fd, connector->encoders[enc_index]);
        if (!encoder) {
            continue;
        }

        for (int crtc_index = 0; crtc_index < resources->count_crtcs; crtc_index++) {
            if ((encoder->possible_crtcs & (1u << crtc_index)) != 0u) {
                fb.crtc_id = resources->crtcs[crtc_index];
                drmModeFreeEncoder(encoder);
                return true;
            }
        }

        drmModeFreeEncoder(encoder);
    }

    return false;
}

static bool drm_create_buffer(DrmBuffer &buffer) {
    drm_mode_create_dumb create_request {};
    create_request.width = fb.width;
    create_request.height = fb.height;
    create_request.bpp = 32;

    if (drmIoctl(fb.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_request) != 0) {
        std::perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return false;
    }

    buffer.handle = create_request.handle;
    buffer.pitch = create_request.pitch;
    buffer.size = create_request.size;

    if (drmModeAddFB(fb.fd, fb.width, fb.height, 24, 32, buffer.pitch, buffer.handle, &buffer.fb_id) != 0) {
        std::perror("drmModeAddFB");
        return false;
    }

    drm_mode_map_dumb map_request {};
    map_request.handle = buffer.handle;
    if (drmIoctl(fb.fd, DRM_IOCTL_MODE_MAP_DUMB, &map_request) != 0) {
        std::perror("DRM_IOCTL_MODE_MAP_DUMB");
        return false;
    }

    buffer.map = static_cast<uint8_t *>(mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, map_request.offset));
    if (buffer.map == MAP_FAILED) {
        std::perror("mmap DRM dumb buffer");
        buffer.map = nullptr;
        return false;
    }

    std::memset(buffer.map, 0, buffer.size);
    return true;
}

static void drm_destroy_buffer(DrmBuffer &buffer) {
    if (buffer.map) {
        munmap(buffer.map, buffer.size);
        buffer.map = nullptr;
    }

    if (buffer.fb_id != 0) {
        drmModeRmFB(fb.fd, buffer.fb_id);
        buffer.fb_id = 0;
    }

    if (buffer.handle != 0) {
        drm_mode_destroy_dumb destroy_request {};
        destroy_request.handle = buffer.handle;
        drmIoctl(fb.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_request);
        buffer.handle = 0;
    }

    buffer.pitch = 0;
    buffer.size = 0;
}

static void drm_page_flip_handler(int, unsigned int, unsigned int, unsigned int, void *) {
    fb.page_flip_pending = false;
}

static bool drm_wait_for_page_flip() {
    drmEventContext event_context {};
    event_context.version = DRM_EVENT_CONTEXT_VERSION;
    event_context.page_flip_handler = drm_page_flip_handler;

    while (fb.page_flip_pending) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fb.fd, &read_fds);

        if (select(fb.fd + 1, &read_fds, nullptr, nullptr, nullptr) < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("select DRM page flip");
            fb.page_flip_pending = false;
            return false;
        }

        if (drmHandleEvent(fb.fd, &event_context) != 0) {
            std::perror("drmHandleEvent");
            fb.page_flip_pending = false;
            return false;
        }
    }

    return true;
}

static bool fb_init(const char *dev = "/dev/dri/card0") {
    fb.fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fb.fd < 0) {
        std::perror("open DRM device");
        return false;
    }

    drmModeRes *resources = drmModeGetResources(fb.fd);
    if (!resources) {
        std::perror("drmModeGetResources");
        close(fb.fd);
        fb.fd = -1;
        return false;
    }

    drmModeConnector *connector = nullptr;
    if (!drm_find_connector_and_mode(resources, &connector) || !drm_find_crtc(resources, connector)) {
        std::cerr << "Failed to find a connected DRM connector/CRTC.\n";
        if (connector) {
            drmModeFreeConnector(connector);
        }
        drmModeFreeResources(resources);
        close(fb.fd);
        fb.fd = -1;
        return false;
    }

    fb.original_crtc = drmModeGetCrtc(fb.fd, fb.crtc_id);
    if (!fb.original_crtc) {
        std::perror("drmModeGetCrtc");
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fb.fd);
        fb.fd = -1;
        return false;
    }

    const bool buffer_ok = drm_create_buffer(fb.buffers[0]) && drm_create_buffer(fb.buffers[1]);
    if (!buffer_ok) {
        drm_destroy_buffer(fb.buffers[1]);
        drm_destroy_buffer(fb.buffers[0]);
        drmModeFreeCrtc(fb.original_crtc);
        fb.original_crtc = nullptr;
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fb.fd);
        fb.fd = -1;
        return false;
    }

    if (drmModeSetCrtc(fb.fd, fb.crtc_id, fb.buffers[0].fb_id, 0, 0, &fb.conn_id, 1, &fb.mode) != 0) {
        std::perror("drmModeSetCrtc");
        drm_destroy_buffer(fb.buffers[1]);
        drm_destroy_buffer(fb.buffers[0]);
        drmModeFreeCrtc(fb.original_crtc);
        fb.original_crtc = nullptr;
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fb.fd);
        fb.fd = -1;
        return false;
    }

    fb.visible_buffer = 0;
    fb.prepared_buffer = UINT32_MAX;

    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    return true;
}

static void fb_deinit() {
    if (fb.fd >= 0 && fb.original_crtc) {
        drmModeSetCrtc(
            fb.fd,
            fb.original_crtc->crtc_id,
            fb.original_crtc->buffer_id,
            fb.original_crtc->x,
            fb.original_crtc->y,
            &fb.conn_id,
            1,
            &fb.original_crtc->mode);
    }

    drm_destroy_buffer(fb.buffers[1]);
    drm_destroy_buffer(fb.buffers[0]);

    if (fb.original_crtc) {
        drmModeFreeCrtc(fb.original_crtc);
        fb.original_crtc = nullptr;
    }

    if (fb.fd >= 0) {
        close(fb.fd);
        fb.fd = -1;
    }
}

static bool fb_present_buffer(uint32_t buffer_index) {
    fb.page_flip_pending = true;
    if (drmModePageFlip(fb.fd, fb.crtc_id, fb.buffers[buffer_index].fb_id, DRM_MODE_PAGE_FLIP_EVENT, nullptr) != 0) {
        std::perror("drmModePageFlip");
        fb.page_flip_pending = false;
        return false;
    }

    if (!drm_wait_for_page_flip()) {
        return false;
    }

    fb.visible_buffer = buffer_index;
    return true;
}

static void app_cleanup() {
    std::free(buf1);
    std::free(buf2);
    buf1 = nullptr;
    buf2 = nullptr;
    touch_deinit();
    fb_deinit();
    vt_leave_graphics();
}

static void handle_debug_input() {
    if (tty_fd < 0) {
        return;
    }

    unsigned char ch = 0;
    const ssize_t bytes_read = read(tty_fd, &ch, 1);
    if (bytes_read == 1 && (ch == 'q' || ch == 'Q' || ch == 27)) {
        exit_requested = 1;
    }
}

static void sync_touch_visual() {
    if (!root_screen || bg_pressed_visual == ts.pressed) {
        return;
    }

    bg_pressed_visual = ts.pressed;
    lv_obj_set_style_bg_color(root_screen, bg_pressed_visual ? lv_color_hex(0x404060) : lv_color_hex(0x000000), 0);
}

static int logical_x_to_physical(int logical_x) {
    return static_cast<int>((static_cast<int64_t>(logical_x) * fb.width) / logical_scr_w);
}

static int logical_y_to_physical(int logical_y) {
    return static_cast<int>((static_cast<int64_t>(logical_y) * fb.height) / logical_scr_h);
}

static int physical_y_to_logical(int physical_y) {
    return static_cast<int>(((static_cast<int64_t>(physical_y) * logical_scr_h) + (fb.height / 2)) / fb.height);
}

static int scale_touch_axis(int value, int min_value, int max_value, int out_max, bool invert_axis) {
    if (max_value <= min_value) {
        return 0;
    }

    int64_t scaled = (static_cast<int64_t>(value - min_value) * out_max) / (max_value - min_value);
    if (scaled < 0) {
        scaled = 0;
    }
    if (scaled > out_max) {
        scaled = out_max;
    }

    int result = static_cast<int>(scaled);
    if (invert_axis) {
        result = out_max - result;
    }
    return result;
}

static void touch_poll() {
    if (ts.fd < 0) {
        return;
    }

    input_event ev {};
    while (read(ts.fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
        if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            ts.pressed = (ev.value != 0);
        } else if (ev.type == EV_ABS) {
            switch (ev.code) {
            case ABS_X:
            case ABS_MT_POSITION_X:
                ts.raw_x = ev.value;
                ts.has_x = true;
                break;
            case ABS_Y:
            case ABS_MT_POSITION_Y:
                ts.raw_y = ev.value;
                ts.has_y = true;
                break;
            default:
                break;
            }
        }
    }
}

static void touch_read(lv_indev_t *, lv_indev_data_t *data) {
    data->state = ts.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

    lv_point_t point {};
    if (ts.has_x && ts.has_y) {
        int physical_x = scale_touch_axis(ts.raw_x, ts.min_x, ts.max_x, static_cast<int>(fb.width) - 1, ts.invert_x);
        int physical_y = scale_touch_axis(ts.raw_y, ts.min_y, ts.max_y, static_cast<int>(fb.height) - 1, ts.invert_y);

        if (ts.swap_xy) {
            const int tmp = physical_x;
            physical_x = physical_y;
            physical_y = tmp;
        }

        point.x = static_cast<lv_coord_t>((static_cast<int64_t>(physical_x) * logical_scr_w) / fb.width);
        point.y = static_cast<lv_coord_t>((static_cast<int64_t>(physical_y) * logical_scr_h) / fb.height);
        data->point = point;
    } else {
        data->point.x = 0;
        data->point.y = 0;
    }
}

static bool touch_get_logical_point(lv_point_t *point) {
    if (!point || !ts.pressed || !ts.has_x || !ts.has_y) {
        return false;
    }

    int physical_x = scale_touch_axis(ts.raw_x, ts.min_x, ts.max_x, static_cast<int>(fb.width) - 1, ts.invert_x);
    int physical_y = scale_touch_axis(ts.raw_y, ts.min_y, ts.max_y, static_cast<int>(fb.height) - 1, ts.invert_y);

    if (ts.swap_xy) {
        const int tmp = physical_x;
        physical_x = physical_y;
        physical_y = tmp;
    }

    point->x = static_cast<lv_coord_t>((static_cast<int64_t>(physical_x) * logical_scr_w) / fb.width);
    point->y = static_cast<lv_coord_t>((static_cast<int64_t>(physical_y) * logical_scr_h) / fb.height);
    return true;
}

static void update_touch_crosshair() {
    if (!touch_crosshair_h || !touch_crosshair_v || !touch_crosshair_dot) {
        return;
    }

    lv_point_t point {};
    if (!touch_get_logical_point(&point)) {
        lv_obj_add_flag(touch_crosshair_h, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(touch_crosshair_v, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(touch_crosshair_dot, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    static constexpr lv_coord_t crosshair_span = 28;
    static constexpr lv_coord_t crosshair_thickness = 3;
    static constexpr lv_coord_t dot_size = 7;

    lv_obj_clear_flag(touch_crosshair_h, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(touch_crosshair_v, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(touch_crosshair_dot, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_pos(touch_crosshair_h, point.x - (crosshair_span / 2), point.y - (crosshair_thickness / 2));
    lv_obj_set_pos(touch_crosshair_v, point.x - (crosshair_thickness / 2), point.y - (crosshair_span / 2));
    lv_obj_set_pos(touch_crosshair_dot, point.x - (dot_size / 2), point.y - (dot_size / 2));
}

static uint32_t pack_fb_pixel(uint8_t red, uint8_t green, uint8_t blue) {
    return 0xFF000000u |
           (static_cast<uint32_t>(red) << 16) |
           (static_cast<uint32_t>(green) << 8) |
           static_cast<uint32_t>(blue);
}

static void rgb565_to_rgb888(uint16_t pixel, uint8_t &red, uint8_t &green, uint8_t &blue) {
    red = static_cast<uint8_t>((((pixel >> 11) & 0x1Fu) * 255u) / 31u);
    green = static_cast<uint8_t>((((pixel >> 5) & 0x3Fu) * 255u) / 63u);
    blue = static_cast<uint8_t>(((pixel & 0x1Fu) * 255u) / 31u);
}

static uint32_t sample_row_box_filtered(const uint16_t *row, uint32_t src_start_fp, uint32_t src_end_fp) {
    uint64_t red_acc = 0;
    uint64_t green_acc = 0;
    uint64_t blue_acc = 0;
    uint32_t weight_acc = 0;

    int src_x = static_cast<int>(src_start_fp >> 16);
    const int src_x_last = static_cast<int>((src_end_fp - 1) >> 16);

    for (; src_x <= src_x_last; src_x++) {
        const uint32_t pixel_start_fp = static_cast<uint32_t>(src_x) << 16;
        const uint32_t pixel_end_fp = static_cast<uint32_t>(src_x + 1) << 16;
        const uint32_t overlap_start = src_start_fp > pixel_start_fp ? src_start_fp : pixel_start_fp;
        const uint32_t overlap_end = src_end_fp < pixel_end_fp ? src_end_fp : pixel_end_fp;
        const uint32_t weight = overlap_end - overlap_start;

        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        rgb565_to_rgb888(row[src_x], red, green, blue);

        red_acc += static_cast<uint64_t>(red) * weight;
        green_acc += static_cast<uint64_t>(green) * weight;
        blue_acc += static_cast<uint64_t>(blue) * weight;
        weight_acc += weight;
    }

    const uint8_t red = static_cast<uint8_t>((red_acc + (weight_acc / 2)) / weight_acc);
    const uint8_t green = static_cast<uint8_t>((green_acc + (weight_acc / 2)) / weight_acc);
    const uint8_t blue = static_cast<uint8_t>((blue_acc + (weight_acc / 2)) / weight_acc);

    return pack_fb_pixel(red, green, blue);
}

static void fb_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const bool is_last_flush = lv_display_flush_is_last(disp);
    const int fb_bytes_per_pixel = 4;
    const auto *src = reinterpret_cast<const uint16_t *>(px_map);
    const int dst_x1 = logical_x_to_physical(area->x1);
    const int dst_x2 = logical_x_to_physical(area->x2 + 1) - 1;
    const int dst_y1 = logical_y_to_physical(area->y1);
    const int dst_y2 = logical_y_to_physical(area->y2 + 1) - 1;

    if (dst_x1 > dst_x2 || dst_y1 > dst_y2) {
        lv_display_flush_ready(disp);
        return;
    }

    const uint32_t draw_buffer = 1u - fb.visible_buffer;
    DrmBuffer &back_buffer = fb.buffers[draw_buffer];

    if (fb.prepared_buffer != draw_buffer) {
        std::memcpy(back_buffer.map, fb.buffers[fb.visible_buffer].map, fb.buffers[fb.visible_buffer].size);
        fb.prepared_buffer = draw_buffer;
    }

    for (int dst_y = dst_y1; dst_y <= dst_y2; dst_y++) {
        const int src_y = physical_y_to_logical(dst_y);
        const uint16_t *src_row = src + (static_cast<size_t>(src_y) * logical_scr_w);
        uint8_t *dst_row = back_buffer.map + (static_cast<size_t>(dst_y) * back_buffer.pitch) + (dst_x1 * fb_bytes_per_pixel);

        for (int dst_x = dst_x1; dst_x <= dst_x2; dst_x++) {
            const uint32_t src_start_fp = static_cast<uint32_t>(((static_cast<uint64_t>(dst_x) * logical_scr_w) << 16) / fb.width);
            const uint32_t src_end_fp = static_cast<uint32_t>(((static_cast<uint64_t>(dst_x + 1) * logical_scr_w) << 16) / fb.width);

            const uint32_t dst_pixel = sample_row_box_filtered(src_row, src_start_fp, src_end_fp);
            reinterpret_cast<uint32_t *>(dst_row)[dst_x - dst_x1] = dst_pixel;
        }
    }

    if (is_last_flush) {
        fb_present_buffer(draw_buffer);
    }

    lv_display_flush_ready(disp);
}

int main() {
    install_signal_handlers();

    if (!vt_enter_graphics("/dev/tty1")) {
        return 1;
    }

    if (!fb_init()) {
        vt_leave_graphics();
        return 1;
    }

    std::atexit(app_cleanup);

    lv_init();

    const uint32_t scr_w = fb.width;
    const uint32_t scr_h = fb.height;
    if (kEnableAspectCorrection) {
        logical_scr_w = static_cast<uint32_t>((static_cast<uint64_t>(scr_w) * kPixelAspectX + (kPixelAspectY / 2)) / kPixelAspectY);
    } else {
        logical_scr_w = scr_w;
    }
    logical_scr_h = scr_h;

    const int buf_pixels = logical_scr_w * logical_scr_h;
    const size_t buf_size = static_cast<size_t>(buf_pixels) * LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_NATIVE);
    buf1 = static_cast<uint8_t *>(std::malloc(buf_size));
    buf2 = static_cast<uint8_t *>(std::malloc(buf_size));
    if (!buf1 || !buf2) {
        std::cerr << "Failed to allocate LVGL buffers\n";
        std::free(buf1);
        std::free(buf2);
        fb_deinit();
        return 1;
    }

    lv_display_t *display = lv_display_create(logical_scr_w, logical_scr_h);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_NATIVE);
    lv_display_set_flush_cb(display, fb_flush);
    lv_display_set_buffers(display, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_DIRECT);

    if (touch_init()) {
        lv_indev_t *touch_indev = lv_indev_create();
        lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(touch_indev, touch_read);
        lv_indev_set_display(touch_indev, display);
    }

    lv_obj_t *screen = lv_scr_act();
    root_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Display calibration");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    char info_text[96];
    std::snprintf(info_text, sizeof(info_text), "logical %ux%u -> physical %ux%u", logical_scr_w, logical_scr_h, scr_w, scr_h);
    lv_obj_t *info = lv_label_create(screen);
    lv_label_set_text(info, info_text);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *screen_border = lv_obj_create(screen);
    lv_obj_remove_style_all(screen_border);
    lv_obj_set_size(screen_border, logical_scr_w - 8, logical_scr_h - 8);
    lv_obj_set_style_border_width(screen_border, 2, 0);
    lv_obj_set_style_border_color(screen_border, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(screen_border, LV_OPA_TRANSP, 0);
    lv_obj_align(screen_border, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *test_box = lv_obj_create(screen);
    lv_obj_remove_style_all(test_box);
    lv_obj_set_size(test_box, 160, 160);
    lv_obj_set_style_border_width(test_box, 3, 0);
    lv_obj_set_style_border_color(test_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(test_box, LV_OPA_TRANSP, 0);
    lv_obj_align(test_box, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *h_rule = lv_obj_create(screen);
    lv_obj_remove_style_all(h_rule);
    lv_obj_set_size(h_rule, 200, 3);
    lv_obj_set_style_bg_color(h_rule, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(h_rule, LV_OPA_COVER, 0);
    lv_obj_align(h_rule, LV_ALIGN_BOTTOM_MID, 0, -44);

    lv_obj_t *h_tick_left = lv_obj_create(screen);
    lv_obj_remove_style_all(h_tick_left);
    lv_obj_set_size(h_tick_left, 3, 18);
    lv_obj_set_style_bg_color(h_tick_left, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(h_tick_left, LV_OPA_COVER, 0);
    lv_obj_align_to(h_tick_left, h_rule, LV_ALIGN_OUT_LEFT_MID, 0, 0);

    lv_obj_t *h_tick_right = lv_obj_create(screen);
    lv_obj_remove_style_all(h_tick_right);
    lv_obj_set_size(h_tick_right, 3, 18);
    lv_obj_set_style_bg_color(h_tick_right, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(h_tick_right, LV_OPA_COVER, 0);
    lv_obj_align_to(h_tick_right, h_rule, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

    lv_obj_t *h_label = lv_label_create(screen);
    lv_label_set_text(h_label, "200 px");
    lv_obj_align_to(h_label, h_rule, LV_ALIGN_OUT_TOP_MID, 0, -6);

    lv_obj_t *v_rule = lv_obj_create(screen);
    lv_obj_remove_style_all(v_rule);
    lv_obj_set_size(v_rule, 3, 200);
    lv_obj_set_style_bg_color(v_rule, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(v_rule, LV_OPA_COVER, 0);
    lv_obj_align(v_rule, LV_ALIGN_LEFT_MID, 44, 0);

    lv_obj_t *v_tick_top = lv_obj_create(screen);
    lv_obj_remove_style_all(v_tick_top);
    lv_obj_set_size(v_tick_top, 18, 3);
    lv_obj_set_style_bg_color(v_tick_top, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(v_tick_top, LV_OPA_COVER, 0);
    lv_obj_align_to(v_tick_top, v_rule, LV_ALIGN_OUT_TOP_MID, 0, 0);

    lv_obj_t *v_tick_bottom = lv_obj_create(screen);
    lv_obj_remove_style_all(v_tick_bottom);
    lv_obj_set_size(v_tick_bottom, 18, 3);
    lv_obj_set_style_bg_color(v_tick_bottom, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(v_tick_bottom, LV_OPA_COVER, 0);
    lv_obj_align_to(v_tick_bottom, v_rule, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

    lv_obj_t *v_label = lv_label_create(screen);
    lv_label_set_text(v_label, "200 px");
    lv_obj_align_to(v_label, v_rule, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    touch_crosshair_h = lv_obj_create(screen);
    lv_obj_remove_style_all(touch_crosshair_h);
    lv_obj_set_size(touch_crosshair_h, 28, 3);
    lv_obj_set_style_bg_color(touch_crosshair_h, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_bg_opa(touch_crosshair_h, LV_OPA_COVER, 0);
    lv_obj_add_flag(touch_crosshair_h, LV_OBJ_FLAG_HIDDEN);

    touch_crosshair_v = lv_obj_create(screen);
    lv_obj_remove_style_all(touch_crosshair_v);
    lv_obj_set_size(touch_crosshair_v, 3, 28);
    lv_obj_set_style_bg_color(touch_crosshair_v, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_bg_opa(touch_crosshair_v, LV_OPA_COVER, 0);
    lv_obj_add_flag(touch_crosshair_v, LV_OBJ_FLAG_HIDDEN);

    touch_crosshair_dot = lv_obj_create(screen);
    lv_obj_remove_style_all(touch_crosshair_dot);
    lv_obj_set_size(touch_crosshair_dot, 7, 7);
    lv_obj_set_style_radius(touch_crosshair_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(touch_crosshair_dot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(touch_crosshair_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(touch_crosshair_dot, LV_OBJ_FLAG_HIDDEN);

    auto last_tick = std::chrono::steady_clock::now();

    while (!exit_requested) {
        handle_debug_input();
        touch_poll();
        sync_touch_visual();
        update_touch_crosshair();

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick).count();
        if (elapsed_ms > 0) {
            lv_tick_inc(static_cast<uint32_t>(elapsed_ms));
            last_tick = now;
        }

        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    return 0;
}
