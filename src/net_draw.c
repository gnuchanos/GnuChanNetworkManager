/*
 * net_draw.c — painting the window from the state.
 *
 * One function, one pass, the whole window each time. The window is, from the
 * top down: a status bar (the title and a one-line summary of the machine), the
 * panel body, then three foot bands — the last message, the buttons, the help
 * line. Laying that out is a few rectangles and a call to the text helper per
 * string, and doing it whole keeps the picture a plain function of the state.
 *
 * The device panel's rows are: a kind mark on the left (a plug for a wired
 * link, a small aerial for a wireless one, a ring for a loopback), the
 * interface name, its state, and its address on the right. The chosen row is
 * filled with the selection colour and its name drawn in the accent.
 *
 * The DNS panel is: the servers as they are and where they came from, then the
 * four boxes Windows asks the same question with — IPv4 and IPv6, each with a
 * Preferred and an Alternate, drawn side by side under its family's heading.
 * Each box is a plain field with a caret when it has the focus, the same shape
 * the wifi manager's password line has.
 *
 * The vertical geometry is fixed and shared: net_visible_rows() measures with
 * the same numbers the drawing uses, so the scroll the input computes and the
 * rows the drawing paints are the same count.
 */
#include "net_draw.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "gnuchan_lang.h"

/* --- geometry, shared by the drawing and the input ------------------------ */

static int row_height(const NetUi *ui) {
    return ui->style.row_height;
}

/* The top of the panel body: below the status bar. */
static int body_top_y(const NetUi *ui) {
    return row_height(ui) + ui->style.padding;
}

/* The bottom of the panel body: above the message line, the button row and the
   help line, each a row tall. */
static int body_bottom_y(const NetUi *ui) {
    return ui->height - 3 * row_height(ui) - ui->style.padding;
}

int net_visible_rows(const NetUi *ui) {
    int room = (body_bottom_y(ui) - body_top_y(ui)) / row_height(ui);
    if (room < 1) {
        room = 1;
    }
    return room;
}

static int status_y(const NetUi *ui) {
    return ui->height - 3 * row_height(ui);
}
static int buttons_y(const NetUi *ui) {
    return ui->height - 2 * row_height(ui);
}
static int help_y(const NetUi *ui) {
    return ui->height - row_height(ui);
}

/* --- the primitives ------------------------------------------------------- */

static Drawable target(const NetUi *ui) {
    return ui->buffer ? ui->buffer : ui->window;
}

static void fill(NetUi *ui, unsigned long colour,
                 int x, int y, int width, int height) {
    XSetForeground(ui->display, ui->gc, colour);
    XFillRectangle(ui->display, target(ui), ui->gc,
                   x, y, (unsigned int)width, (unsigned int)height);
}

static void outline(NetUi *ui, unsigned long colour,
                    int x, int y, int width, int height) {
    XSetForeground(ui->display, ui->gc, colour);
    XDrawRectangle(ui->display, target(ui), ui->gc,
                   x, y, (unsigned int)(width - 1), (unsigned int)(height - 1));
}

static void text(NetUi *ui, int x, int baseline, const char *string,
                 unsigned long colour) {
    net_style_text(ui->display, ui->screen, target(ui), ui->style.font,
                   x, baseline, string, colour);
}

/* The baseline that centres a line of text in a band starting at `band_y`. */
static int band_baseline(const NetUi *ui, int band_y) {
    int line = ui->style.font ? (ui->style.font->ascent +
                                 ui->style.font->descent) : 16;
    return band_y + (row_height(ui) - line) / 2 +
           (ui->style.font ? ui->style.font->ascent : line);
}

/* --- the buttons ---------------------------------------------------------- */

/* One button in a table: what it says and what it does. A NAMED type, because
   the two tables below and the function that lays them out must agree on one
   type — two anonymous structs that merely look alike are different types in
   C, and passing one where the other is expected does not compile. */
typedef struct NetButtonDef {
    const char *label;
    NetAction action;
} NetButtonDef;

/* The buttons, per mode. Labels are localized at draw/layout time. */
static const NetButtonDef kDeviceButtons[] = {
    { "Up/Down",    NET_ACTION_TOGGLE  },
    { "Open Wi-Fi", NET_ACTION_WIFI    },
    { "DPI Settings", NET_ACTION_DPI_SETTINGS },
    { "Refresh",    NET_ACTION_REFRESH },
};
#define DEVICE_BUTTON_COUNT \
    ((int)(sizeof(kDeviceButtons) / sizeof(kDeviceButtons[0])))

static const NetButtonDef kDpiButtons[] = {
    { "Install Zapret", NET_ACTION_DPI_INSTALL },
    { "Remove Zapret", NET_ACTION_DPI_REMOVE },
    { "Enable DPI", NET_ACTION_DPI_TOGGLE },
    { "Refresh", NET_ACTION_REFRESH },
    { "Back", NET_ACTION_BACK },
};
#define DPI_BUTTON_COUNT \
    ((int)(sizeof(kDpiButtons) / sizeof(kDpiButtons[0])))

static const char *button_label(const NetUi *ui, const NetButtonDef *table,
                                int index) {
    if (table == kDpiButtons) {
        if (index == 2) {
            return ui->dpi_active ? gnuchan_lang_t("net_btn_disable_dpi")
                                  : gnuchan_lang_t("net_btn_enable_dpi");
        }
        switch (table[index].action) {
        case NET_ACTION_DPI_INSTALL: return gnuchan_lang_t("net_btn_install_zapret");
        case NET_ACTION_DPI_REMOVE:  return gnuchan_lang_t("net_btn_remove_zapret");
        case NET_ACTION_REFRESH:     return gnuchan_lang_t("net_btn_refresh");
        case NET_ACTION_BACK:        return gnuchan_lang_t("net_btn_back");
        default: break;
        }
    } else {
        switch (table[index].action) {
        case NET_ACTION_TOGGLE:      return gnuchan_lang_t("net_btn_up_down");
        case NET_ACTION_WIFI:        return gnuchan_lang_t("net_btn_open_wifi");
        case NET_ACTION_DPI_SETTINGS:return gnuchan_lang_t("net_btn_dpi_settings");
        case NET_ACTION_REFRESH:     return gnuchan_lang_t("net_btn_refresh");
        default: break;
        }
    }
    return table[index].label;
}

/* Lay out one row of buttons, centred, shrinking the air until it fits the
   window. `table` and `count` identify one panel's buttons above. */
static void layout_row(NetUi *ui, const NetButtonDef *table, int count) {
    if (count > NET_MAX_BUTTONS) {
        count = NET_MAX_BUTTONS;
    }

    int pad = ui->style.padding / 2;
    if (pad < 4) {
        pad = 4;
    }
    int gap = ui->style.padding / 2;
    if (gap < 4) {
        gap = 4;
    }

    int total = 0;
    for (int i = 0; i < count; i++) {
        total += net_style_text_width(ui->display, ui->style.font,
                                      button_label(ui, table, i)) + 2 * pad;
    }
    total += gap * (count - 1);

    int available = ui->width - 2 * ui->style.padding;
    while (total > available && (pad > 2 || gap > 2)) {
        if (pad > 2) {
            pad--;
            total -= 2 * count;
        } else if (gap > 2) {
            gap--;
            total -= (count - 1);
        }
    }

    int band = buttons_y(ui);
    int height = row_height(ui);
    int x = (ui->width - total) / 2;
    if (x < ui->style.padding) {
        x = ui->style.padding;
    }

    for (int i = 0; i < count; i++) {
        int width = net_style_text_width(ui->display, ui->style.font,
                                         button_label(ui, table, i)) + 2 * pad;
        ui->buttons[i].x = x;
        ui->buttons[i].y = band;
        ui->buttons[i].width = width;
        ui->buttons[i].height = height;
        ui->buttons[i].action = table[i].action;
        x += width + gap;
    }
    ui->button_count = count;
}

void net_layout_buttons(NetUi *ui) {
    if (ui->mode == NET_MODE_DPI) {
        layout_row(ui, kDpiButtons, DPI_BUTTON_COUNT);
    } else {
        layout_row(ui, kDeviceButtons, DEVICE_BUTTON_COUNT);
    }
}

static void draw_buttons(NetUi *ui) {
    net_layout_buttons(ui);
    const NetButtonDef *table = ui->mode == NET_MODE_DPI ? kDpiButtons
        : kDeviceButtons;

    for (int i = 0; i < ui->button_count; i++) {
        NetButton *button = &ui->buttons[i];
        fill(ui, ui->style.field, button->x, button->y,
             button->width, button->height);
        outline(ui, ui->style.panel_edge, button->x, button->y,
                button->width, button->height);
        const char *label = button_label(ui, table, i);
        int label_w = net_style_text_width(ui->display, ui->style.font, label);
        int text_x = button->x + (button->width - label_w) / 2;
        /* Active toggles are drawn in the connected colour, so their state is
           readable at a glance and not only by reading the word. */
        unsigned long colour =
            (table == kDpiButtons && i == 2 && ui->dpi_active)
                ? ui->style.connected
                : ui->style.text;
        text(ui, text_x, band_baseline(ui, button->y), label, colour);
    }
}

/* --- the status bar ------------------------------------------------------- */

static void draw_status_bar(NetUi *ui) {
    int height = row_height(ui);
    fill(ui, ui->style.panel, 0, 0, ui->width, height);

    int x = ui->style.padding;
    const char *title = ui->config.title[0] ? ui->config.title : gnuchan_lang_t("net_title");
    text(ui, x, band_baseline(ui, 0), title, ui->style.accent);
    x += net_style_text_width(ui->display, ui->style.font, title)
         + ui->style.padding * 2;

    /* A one-line summary of the machine, so the state is readable without
       reading every row. */
    int connected = 0;
    for (int i = 0; i < ui->devices.count; i++) {
        if (ui->devices.items[i].state == 1) {
            connected++;
        }
    }
    char summary[NET_TEXT * 2];
    if (ui->devices.count == 0) {
        snprintf(summary, sizeof(summary), "%s", gnuchan_lang_t("net_no_interfaces_read"));
    } else if (connected == 0) {
        snprintf(summary, sizeof(summary), gnuchan_lang_t("net_interfaces_none_connected"),
                 ui->devices.count);
    } else {
        snprintf(summary, sizeof(summary), gnuchan_lang_t("net_interfaces_connected"),
                 ui->devices.count, connected);
    }
    text(ui, x, band_baseline(ui, 0), summary, ui->style.text_muted);

    fill(ui, ui->style.panel_edge, 0, height - 2, ui->width, 2);
}

/* --- the device panel ----------------------------------------------------- */

/* A small mark for the interface kind, drawn by hand so no glyph is needed:
   a plug for wired, an aerial for wireless, a ring for loopback, a dot
   otherwise. */
static void draw_kind(NetUi *ui, NetDeviceKind kind, int x, int y,
                      int size) {
    unsigned long colour = (kind == NET_DEVICE_WIFI) ? ui->style.accent
                                                     : ui->style.text_muted;
    int cy = y + size / 2;

    switch (kind) {
    case NET_DEVICE_ETHERNET:
        /* A plug: a body with a stub. */
        fill(ui, colour, x, cy - 3, size - 4, 6);
        fill(ui, colour, x + size - 4, cy - 2, 4, 4);
        break;
    case NET_DEVICE_WIFI:
        /* An aerial: a mast with two waves. */
        fill(ui, colour, x + size / 2 - 1, cy - size / 2, 3, size);
        fill(ui, colour, x, cy - size / 2 + 2, 3, 3);
        fill(ui, colour, x + size - 3, cy - size / 2 + 2, 3, 3);
        break;
    case NET_DEVICE_LOOPBACK:
        /* A ring. */
        XSetForeground(ui->display, ui->gc, colour);
        XDrawArc(ui->display, target(ui), ui->gc, x, cy - size / 2 + 2,
                 (unsigned)(size - 4), (unsigned)(size - 4), 0, 360 * 64);
        break;
    default:
        fill(ui, colour, x + size / 2 - 2, cy - 2, 4, 4);
        break;
    }
}

/* The state word for an interface: "connected", "down", "up", or nothing when
   it could not be read. */
static const char *state_word(const NetDevice *device) {
    if (device->state == 1) {
        return gnuchan_lang_t("net_state_connected");
    }
    if (device->state == 0 && device->up) {
        return gnuchan_lang_t("net_state_up");
    }
    if (device->state == 0) {
        return gnuchan_lang_t("net_state_disconnected");
    }
    return device->up ? gnuchan_lang_t("net_state_up") : "";
}

static void draw_devices(NetUi *ui) {
    int top = body_top_y(ui);

    if (ui->devices.count == 0) {
        text(ui, ui->style.padding, band_baseline(ui, top),
             gnuchan_lang_t("net_no_interfaces"),
             ui->style.text_muted);
        return;
    }

    int left = ui->style.padding;
    int right = ui->width - ui->style.padding;
    int room = net_visible_rows(ui);

    if (ui->selected < ui->scroll) {
        ui->scroll = ui->selected;
    }
    if (ui->selected >= ui->scroll + room) {
        ui->scroll = ui->selected - room + 1;
    }
    if (ui->scroll < 0) {
        ui->scroll = 0;
    }

    for (int row = 0; row < room; row++) {
        int index = ui->scroll + row;
        if (index >= ui->devices.count) {
            break;
        }
        NetDevice *device = &ui->devices.items[index];
        int row_y = top + row * row_height(ui);

        int is_selected = (index == ui->selected);
        if (is_selected) {
            fill(ui, ui->style.selection, left, row_y, right - left,
                 row_height(ui));
            fill(ui, ui->style.accent, left, row_y, 4, row_height(ui));
        } else if (index == ui->hover) {
            fill(ui, ui->style.field, left, row_y, right - left,
                 row_height(ui));
        }

        int x = left + ui->style.padding + 4;
        int mark = row_height(ui) - 2 * ui->style.padding;
        if (mark < 10) {
            mark = 10;
        }
        draw_kind(ui, device->kind, x, row_y + ui->style.padding, mark);
        x += mark + ui->style.padding;

        unsigned long name_colour = ui->style.text;
        if (is_selected) {
            name_colour = ui->style.accent;
        }
        text(ui, x, band_baseline(ui, row_y), device->name, name_colour);
        x += net_style_text_width(ui->display, ui->style.font, device->name)
             + ui->style.padding * 2;

        /* The state, right after the name, in the state colour. */
        const char *state = state_word(device);
        unsigned long state_colour = (device->state == 1) ? ui->style.connected
                                                          : ui->style.text_muted;
        text(ui, x, band_baseline(ui, row_y), state, state_colour);

        /* The address on the right; the connection name before it when the
           interface is managed and has one. */
        int mark_right = right - ui->style.padding;
        if (device->address[0]) {
            int address_w = net_style_text_width(ui->display, ui->style.font,
                                                 device->address);
            mark_right -= address_w;
            text(ui, mark_right, band_baseline(ui, row_y), device->address,
                 ui->style.text);
            mark_right -= ui->style.padding * 2;
        }

        /* The connection name, clipped to the room between the state word and
           the address so it never overruns either. */
        if (device->connection[0]) {
            int from = x + net_style_text_width(ui->display, ui->style.font,
                                                state) + ui->style.padding;
            int room_for = mark_right - from;
            if (room_for > 0) {
                char fitted[NET_TEXT];
                net_style_fit(ui->display, ui->style.font, device->connection,
                              room_for, fitted, sizeof(fitted));
                text(ui, from, band_baseline(ui, row_y), fitted,
                     ui->style.text_muted);
            }
        }
    }
}

/* --- the foot ------------------------------------------------------------- */

static void draw_status_line(NetUi *ui) {
    if (!ui->status[0]) {
        return;
    }
    char fitted[NET_TEXT * 4];
    net_style_fit(ui->display, ui->style.font, ui->status,
                  ui->width - 2 * ui->style.padding, fitted, sizeof(fitted));
    text(ui, ui->style.padding, band_baseline(ui, status_y(ui)), fitted,
         ui->style.text);
}

static void draw_help(NetUi *ui) {
    const char *help = (ui->mode == NET_MODE_DPI)
        ? gnuchan_lang_t("net_help_dpi")
        : gnuchan_lang_t("net_help_devices");
    int descent = ui->style.font ? ui->style.font->descent : 4;
    text(ui, ui->style.padding, help_y(ui) + row_height(ui) - descent - 2,
         help, ui->style.text_muted);
}

static int dpi_busy_draw(void) {
    int fd = open("/var/lock/gnuchan-zapret-manager.lock", O_RDWR);
    if (fd < 0) {
        return 0;
    }
    int locked = (flock(fd, LOCK_EX | LOCK_NB) != 0);
    if (!locked) {
        flock(fd, LOCK_UN);
    }
    close(fd);
    return locked;
}

static void draw_log_panel(NetUi *ui, int x, int y, int width, int height) {
    fill(ui, ui->style.field, x, y, width, height);
    outline(ui, ui->style.panel_edge, x, y, width, height);

    int line = row_height(ui);
    int inner_x = x + ui->style.padding;
    int title_y = y + ui->style.padding / 2;
    int inner_w = width - 2 * ui->style.padding;
    if (inner_w < 20) inner_w = 20;

    char title[256];
    snprintf(title, sizeof(title), "%s  (/var/log/gnuchan-zapret-manager.log)",
             gnuchan_lang_t("net_live_output"));
    text(ui, inner_x, band_baseline(ui, title_y), title, ui->style.accent);
    int sep_y = title_y + line;
    fill(ui, ui->style.panel_edge, x + 1, sep_y, width - 2, 2);

    int inner_y = sep_y + ui->style.padding / 2;
    const char *path = "/var/log/gnuchan-zapret-manager.log";
    FILE *f = fopen(path, "r");
    if (!f) {
        text(ui, inner_x, band_baseline(ui, inner_y),
             gnuchan_lang_t("net_log_not_created"), ui->style.text_muted);
        return;
    }
    char lines[20][512];
    int count = 0;
    while (fgets(lines[count % 20], (int)sizeof(lines[0]), f)) {
        char *nl = strchr(lines[count % 20], '\n');
        if (nl) *nl = '\0';
        count++;
    }
    fclose(f);

    int max_lines = (y + height - inner_y - ui->style.padding / 2) / line;
    if (max_lines < 1) max_lines = 1;
    if (max_lines > 14) max_lines = 14;
    int show = count < max_lines ? count : max_lines;
    int start = count - show;
    if (show == 0) {
        text(ui, inner_x, band_baseline(ui, inner_y),
             gnuchan_lang_t("net_waiting_output"), ui->style.text_muted);
        return;
    }
    for (int i = 0; i < show; i++) {
        char fitted[512];
        const char *src = lines[(start + i) % 20];
        net_style_fit(ui->display, ui->style.font, src, inner_w, fitted, sizeof(fitted));
        text(ui, inner_x, band_baseline(ui, inner_y), fitted, ui->style.text);
        inner_y += line;
    }
}

static void draw_dpi(NetUi *ui) {
    int pad = ui->style.padding;
    int x = pad;
    int y = body_top_y(ui);
    int line = row_height(ui);
    int busy = dpi_busy_draw();
    int right = ui->width - pad;
    int content_w = right - x;
    if (content_w < 100) content_w = 100;

    /* Header/status card */
    int header_h = line * 3 + pad * 2;
    fill(ui, ui->style.panel, x, y, content_w, header_h);
    outline(ui, ui->style.panel_edge, x, y, content_w, header_h);

    int tx = x + pad;
    int ty = y + pad / 2;
    text(ui, tx, band_baseline(ui, ty), gnuchan_lang_t("net_dpi_pro_title"),
         ui->style.accent);

    char state[256];
    snprintf(state, sizeof(state), "%s: %s   |   %s: %s",
             gnuchan_lang_t("net_zapret"),
             gnuchan_lang_t(ui->dpi_available ? "net_installed" : "net_not_installed"),
             gnuchan_lang_t("net_bypass"),
             gnuchan_lang_t(ui->dpi_active ? "net_on" : "net_off"));
    ty += line;
    text(ui, tx, band_baseline(ui, ty), state,
         (ui->dpi_available && ui->dpi_active) ? ui->style.connected : ui->style.text);

    char op[256];
    snprintf(op, sizeof(op), "%s: %s %s",
             gnuchan_lang_t("net_operation"),
             gnuchan_lang_t(busy ? "net_running" : "net_idle"),
             busy ? gnuchan_lang_t("net_auto_refreshing") : "");
    ty += line;
    text(ui, tx, band_baseline(ui, ty), op,
         busy ? ui->style.accent : ui->style.text_muted);

    y += header_h + pad;

    /* Log panel occupies remaining body area, never overflows into foot */
    int log_h = body_bottom_y(ui) - y - pad / 2;
    if (log_h < line * 5) log_h = line * 5;
    if (y + log_h > body_bottom_y(ui)) log_h = body_bottom_y(ui) - y;
    if (log_h > 0) {
        draw_log_panel(ui, x, y, content_w, log_h);
    }
}

/* --- the frame ------------------------------------------------------------ */

void net_draw(NetUi *ui) {
    if (!ui->display || ui->window == None) {
        return;
    }

    /* The off-screen buffer, made when the window opens and remade whenever
       the window is resized. Everything below is drawn into it, and the
       finished frame is copied to the window in one go at the end. */
    if (!ui->buffer || ui->buffer_width != ui->width ||
        ui->buffer_height != ui->height) {
        if (ui->buffer) {
            XFreePixmap(ui->display, ui->buffer);
        }
        ui->buffer = XCreatePixmap(ui->display, ui->root,
                                   (unsigned int)ui->width,
                                   (unsigned int)ui->height,
                                   (unsigned int)DefaultDepth(ui->display,
                                                              ui->screen));
        ui->buffer_width = ui->width;
        ui->buffer_height = ui->height;
    }

    fill(ui, ui->style.background, 0, 0, ui->width, ui->height);

    draw_status_bar(ui);

    if (ui->mode == NET_MODE_DPI) {
        draw_dpi(ui);
    } else {
        draw_devices(ui);
    }

    draw_status_line(ui);
    draw_buttons(ui);
    draw_help(ui);

    XCopyArea(ui->display, target(ui), ui->window, ui->gc, 0, 0,
              (unsigned int)ui->width, (unsigned int)ui->height, 0, 0);
    XFlush(ui->display);
}
