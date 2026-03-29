#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <sys/kd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

#include <csignal>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include "lv_conf.h"
#include <lvgl/lvgl.h>

struct Framebuffer {
    int fd = -1;
    uint8_t *ptr = nullptr;
    size_t length = 0;
    fb_var_screeninfo vinfo{};
    fb_fix_screeninfo finfo{};
};

static Framebuffer fb;
static uint8_t *buf1 = nullptr;
static uint8_t *buf2 = nullptr;
static int tty_fd = -1;
static int old_kd_mode = KD_TEXT;
static termios old_termios{};
static bool old_termios_valid = false;
static volatile std::sig_atomic_t exit_requested = 0;

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

static bool fb_init(const char *dev = "/dev/fb0") {
    fb.fd = open(dev, O_RDWR);
    if (fb.fd < 0) {
        std::perror("open framebuffer");
        return false;
    }

    if (ioctl(fb.fd, FBIOGET_FSCREENINFO, &fb.finfo)) {
        std::perror("ioctl FBIOGET_FSCREENINFO");
        close(fb.fd);
        fb.fd = -1;
        return false;
    }
    if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.vinfo)) {
        std::perror("ioctl FBIOGET_VSCREENINFO");
        close(fb.fd);
        fb.fd = -1;
        return false;
    }

    fb.length = fb.finfo.smem_len;
    fb.ptr = static_cast<uint8_t *>(mmap(nullptr, fb.length, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0));
    if (fb.ptr == MAP_FAILED) {
        std::perror("mmap framebuffer");
        fb.ptr = nullptr;
        close(fb.fd);
        fb.fd = -1;
        return false;
    }

    if (fb.vinfo.bits_per_pixel != 32 && fb.vinfo.bits_per_pixel != 16) {
        std::cerr << "Unsupported framebuffer bpp: " << fb.vinfo.bits_per_pixel << " (only 16/32 supported).\n";
        munmap(fb.ptr, fb.length);
        close(fb.fd);
        fb.fd = -1;
        fb.ptr = nullptr;
        return false;
    }

    return true;
}

static void fb_deinit() {
    if (fb.ptr) {
        munmap(fb.ptr, fb.length);
        fb.ptr = nullptr;
    }
    if (fb.fd >= 0) {
        close(fb.fd);
        fb.fd = -1;
    }
}

static void app_cleanup() {
    std::free(buf1);
    std::free(buf2);
    buf1 = nullptr;
    buf2 = nullptr;
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

static uint32_t scale_8bit_to_channel(uint8_t value, uint32_t bits) {
    if (bits == 0) {
        return 0;
    }

    const uint32_t max_value = (1u << bits) - 1u;
    return (static_cast<uint32_t>(value) * max_value + 127u) / 255u;
}

static uint32_t pack_fb_pixel(uint8_t red, uint8_t green, uint8_t blue) {
    uint32_t pixel = 0;

    pixel |= scale_8bit_to_channel(red, fb.vinfo.red.length) << fb.vinfo.red.offset;
    pixel |= scale_8bit_to_channel(green, fb.vinfo.green.length) << fb.vinfo.green.offset;
    pixel |= scale_8bit_to_channel(blue, fb.vinfo.blue.length) << fb.vinfo.blue.offset;

    if (fb.vinfo.transp.length != 0) {
        pixel |= ((1u << fb.vinfo.transp.length) - 1u) << fb.vinfo.transp.offset;
    }

    return pixel;
}

static void rgb565_to_rgb888(uint16_t pixel, uint8_t &red, uint8_t &green, uint8_t &blue) {
    red = static_cast<uint8_t>((((pixel >> 11) & 0x1Fu) * 255u) / 31u);
    green = static_cast<uint8_t>((((pixel >> 5) & 0x3Fu) * 255u) / 63u);
    blue = static_cast<uint8_t>(((pixel & 0x1Fu) * 255u) / 31u);
}

static void fb_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const int area_width = area->x2 - area->x1 + 1;
    const int area_height = area->y2 - area->y1 + 1;
    const int fb_bytes_per_pixel = fb.vinfo.bits_per_pixel / 8;
    const auto *src = reinterpret_cast<const uint16_t *>(px_map);

    for (int row_index = 0; row_index < area_height; row_index++) {
        uint8_t *dst_row = fb.ptr + ((area->y1 + row_index) * fb.finfo.line_length) + (area->x1 * fb_bytes_per_pixel);

        for (int col_index = 0; col_index < area_width; col_index++) {
            const uint16_t src_pixel = src[row_index * area_width + col_index];

            uint8_t red = 0;
            uint8_t green = 0;
            uint8_t blue = 0;
            rgb565_to_rgb888(src_pixel, red, green, blue);

            const uint32_t dst_pixel = pack_fb_pixel(red, green, blue);
            if (fb.vinfo.bits_per_pixel == 32) {
                reinterpret_cast<uint32_t *>(dst_row)[col_index] = dst_pixel;
            } else {
                reinterpret_cast<uint16_t *>(dst_row)[col_index] = static_cast<uint16_t>(dst_pixel);
            }
        }
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

    const uint32_t scr_w = fb.vinfo.xres;
    const uint32_t scr_h = fb.vinfo.yres;

    const int buf_pixels = scr_w * 30;
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

    lv_display_t *display = lv_display_create(scr_w, scr_h);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_NATIVE);
    lv_display_set_flush_cb(display, fb_flush);
    lv_display_set_buffers(display, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "Hello LVGL framebuffer!");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    while (!exit_requested) {
        handle_debug_input();
        lv_tick_inc(10);
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return 0;
}
