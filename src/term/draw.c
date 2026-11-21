#include "mc/term/draw.h"

#include "mc/term/ansi.h"
#include "mc/text/utf8.h"

enum {
    BOX_HORIZONTAL = 0x2500, // ─
    BOX_VERTICAL = 0x2502,   // │
    BOX_TOP_LEFT = 0x256D,   // ╭
    BOX_TOP_RIGHT = 0x256E,  // ╮
    BOX_BOTTOM_RIGHT = 0x256F,
    BOX_BOTTOM_LEFT = 0x2570,
    TITLE_OPEN = 0x2524,  // ┤
    TITLE_CLOSE = 0x251C, // ├
    METER_SQUARE = 0x25A0, // ■
    BRAILLE_BLANK = 0x2800,
    BLOCK_LOWEST = 0x2581, // ▁, then seven taller eighths
    SPARK_LEVELS_MAX = 16,
};

static uint32_t or_default(uint32_t glyph, uint32_t fallback)
{
    return glyph != 0 ? glyph : fallback;
}

static BoxGlyphs box_glyphs(BoxGlyphs glyphs)
{
    return (BoxGlyphs){
        .horizontal = or_default(glyphs.horizontal, BOX_HORIZONTAL),
        .vertical = or_default(glyphs.vertical, BOX_VERTICAL),
        .top_left = or_default(glyphs.top_left, BOX_TOP_LEFT),
        .top_right = or_default(glyphs.top_right, BOX_TOP_RIGHT),
        .bottom_left = or_default(glyphs.bottom_left, BOX_BOTTOM_LEFT),
        .bottom_right = or_default(glyphs.bottom_right, BOX_BOTTOM_RIGHT),
        .title_open = or_default(glyphs.title_open, TITLE_OPEN),
        .title_close = or_default(glyphs.title_close, TITLE_CLOSE),
    };
}

static float fraction(int index, int count)
{
    return count <= 1 ? 0.0f : (float)index / (float)(count - 1);
}

// edge_color lights the top and left edges from the corner.
static Rgb edge_color(Panel panel, float distance)
{
    return rgb_mix(panel.glow, panel.border, distance);
}

// draw_title puts "┤ text ├" at x and returns the columns it took.
static int draw_title(Screen *screen, int x, int y, int max_columns, String text, Rgb color, Rgb border,
                      const BoxGlyphs *glyphs)
{
    if (max_columns < 5 || text.len == 0) {
        return 0;
    }
    String fitted = ansi_truncate(text, (size_t)(max_columns - 4));
    screen_glyph(screen, x, y, glyphs->title_open, border, 0);
    screen_glyph(screen, x + 1, y, ' ', color, 0);
    int columns = screen_text(screen, x + 2, y, max_columns - 4, fitted, color, ATTR_BOLD);
    screen_glyph(screen, x + 2 + columns, y, ' ', color, 0);
    screen_glyph(screen, x + 3 + columns, y, glyphs->title_close, border, 0);
    return columns + 4;
}

void draw_panel(Screen *screen, Rect rect, Panel panel)
{
    if (rect.width < 2 || rect.height < 2) {
        return;
    }
    BoxGlyphs glyphs = box_glyphs(panel.glyphs);
    int right = rect.x + rect.width - 1;
    int bottom = rect.y + rect.height - 1;
    for (int x = rect.x + 1; x < right; x++) {
        float along = fraction(x - rect.x, rect.width);
        screen_glyph(screen, x, rect.y, glyphs.horizontal, edge_color(panel, along), 0);
        screen_glyph(screen, x, bottom, glyphs.horizontal, edge_color(panel, 0.5f + along * 0.5f), 0);
    }
    for (int y = rect.y + 1; y < bottom; y++) {
        float along = fraction(y - rect.y, rect.height);
        screen_glyph(screen, rect.x, y, glyphs.vertical, edge_color(panel, along), 0);
        screen_glyph(screen, right, y, glyphs.vertical, edge_color(panel, 0.5f + along * 0.5f), 0);
    }
    screen_glyph(screen, rect.x, rect.y, glyphs.top_left, panel.glow, 0);
    screen_glyph(screen, right, rect.y, glyphs.top_right, edge_color(panel, 1.0f), 0);
    screen_glyph(screen, rect.x, bottom, glyphs.bottom_left, edge_color(panel, 1.0f), 0);
    screen_glyph(screen, right, bottom, glyphs.bottom_right, panel.border, 0);

    int title_columns = draw_title(screen, rect.x + 1, rect.y, rect.width - 2, panel.title, panel.title_color,
                                   edge_color(panel, fraction(1, rect.width)), &glyphs);
    int info_room = rect.width - 4 - title_columns;
    if (panel.info.len > 0 && info_room >= 5) {
        String info = ansi_truncate(panel.info, (size_t)(info_room - 4));
        int info_x = right - 1 - ((int)utf8_width(info) + 4);
        draw_title(screen, info_x, rect.y, info_room, info, panel.info_color,
                   edge_color(panel, fraction(info_x - rect.x, rect.width)), &glyphs);
    }
}

void draw_meter(Screen *screen, int x, int y, int width, float value, Gradient gradient, Rgb empty,
                MeterGlyphs glyphs)
{
    uint32_t filled_glyph = or_default(glyphs.filled, METER_SQUARE);
    uint32_t empty_glyph = or_default(glyphs.empty, METER_SQUARE);
    int filled = (int)(value * (float)width + 0.5f);
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            screen_glyph(screen, x + i, y, filled_glyph, gradient_at(gradient, fraction(i, width)), 0);
        } else {
            screen_glyph(screen, x + i, y, empty_glyph, empty, 0);
        }
    }
}

// Braille dots by column, from the bottom of a cell to its top.
static const uint8_t LEFT_DOTS_UP[4] = { 0x40, 0x04, 0x02, 0x01 };
static const uint8_t RIGHT_DOTS_UP[4] = { 0x80, 0x20, 0x10, 0x08 };

static int dots_for(float value, float scale, int total)
{
    if (value <= 0.0f || scale <= 0.0f) {
        return 0;
    }
    int dots = (int)(value / scale * (float)total + 0.5f);
    return dots < 1 ? 1 : dots > total ? total : dots;
}

// cell_dots is the dot pattern of one column of a cell that holds the dots
// from depth (counted from the baseline) to depth + 4.
static uint8_t cell_dots(const uint8_t dots_up[4], int filled, int depth, bool downward)
{
    int count = filled - depth;
    count = count < 0 ? 0 : count > 4 ? 4 : count;
    uint8_t pattern = 0;
    for (int i = 0; i < count; i++) {
        pattern |= dots_up[downward ? 3 - i : i];
    }
    return pattern;
}

void draw_graph(Screen *screen, Rect rect, const History *history, float scale, Gradient gradient, bool downward)
{
    int total = rect.height * 4;
    for (int column = 0; column < rect.width; column++) {
        size_t right_age = (size_t)(rect.width - 1 - column) * 2;
        int right_filled = dots_for(history_at(history, right_age), scale, total);
        int left_filled = dots_for(history_at(history, right_age + 1), scale, total);
        for (int row = 0; row < rect.height; row++) {
            int depth_rows = downward ? row : rect.height - 1 - row;
            int depth = depth_rows * 4;
            uint8_t pattern = (uint8_t)(cell_dots(LEFT_DOTS_UP, left_filled, depth, downward) |
                                        cell_dots(RIGHT_DOTS_UP, right_filled, depth, downward));
            if (pattern == 0) {
                continue;
            }
            Rgb color = gradient_at(gradient, ((float)depth_rows + 0.5f) / (float)rect.height);
            screen_glyph(screen, rect.x + column, rect.y + row, BRAILLE_BLANK + pattern, color, 0);
        }
    }
}

void draw_sparkline(Screen *screen, int x, int y, int width, const History *history, float scale, Gradient gradient,
                    Rgb baseline, String levels)
{
    uint32_t glyphs[SPARK_LEVELS_MAX];
    int count = 0;
    for (size_t position = 0; position < levels.len && count < SPARK_LEVELS_MAX;) {
        utf8_decode(levels, &position, &glyphs[count++]);
    }
    if (count == 0) {
        for (; count < 8; count++) {
            glyphs[count] = (uint32_t)(BLOCK_LOWEST + count);
        }
    }
    for (int i = 0; i < width; i++) {
        float value = history_at(history, (size_t)(width - 1 - i));
        float level = scale > 0.0f ? value / scale : 0.0f;
        level = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
        int steps = (int)(level * (float)count + 0.5f);
        if (steps == 0) {
            screen_glyph(screen, x + i, y, glyphs[0], baseline, 0);
            continue;
        }
        screen_glyph(screen, x + i, y, glyphs[steps - 1], gradient_at(gradient, level), 0);
    }
}

int draw_text_right(Screen *screen, int x_end, int y, String text, Rgb fg, uint8_t attrs)
{
    int start = x_end - (int)utf8_width(text);
    screen_text(screen, start, y, x_end - start, text, fg, attrs);
    return start;
}
