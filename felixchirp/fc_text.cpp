#ifndef _WIN32
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <fcntl.h>
#else
#  include <windows.h>
#  include <shlobj.h>
#  include <io.h>
#  include <fcntl.h>
#endif

#include <string>
#include <cstring>
#include "felixchirp.h"

#include "../gl_renderer.h"
#include "../sdl_renderer.h"
#include "../ft_font.h"
#include "../term_color.h"

extern int  g_font_size;
static const int IV_PAD = 8;

// Simple markdown line parser with enhanced formatting
TextLine iv_parse_markdown_line(const std::string &raw_line) {
    TextLine line{};
    std::string display_text = raw_line;
    line.is_header = false;
    line.header_level = 0;
    line.is_code_block = false;
    line.is_list_item = false;
    line.is_link = false;

    // Detect headers: # Header, ## Header2, etc.
    if (!raw_line.empty() && raw_line[0] == '#') {
        int level = 0;
        size_t i = 0;
        while (i < raw_line.size() && raw_line[i] == '#') {
            level++;
            i++;
        }
        if (level <= 6 && i < raw_line.size() && raw_line[i] == ' ') {
            line.is_header = true;
            line.header_level = level;
            display_text = raw_line.substr(i + 1);
        }
    }

    // Detect code blocks (4+ space indent)
    if (!raw_line.empty() && !line.is_header) {
        int spaces = 0;
        for (char c : raw_line) {
            if (c == ' ') spaces++;
            else if (c == '\t') spaces += 4;
            else break;
        }
        if (spaces >= 4) {
            line.is_code_block = true;
        }
    }

    // Detect list items
    if (!raw_line.empty() && !line.is_header && !line.is_code_block) {
        if ((raw_line[0] == '-' || raw_line[0] == '*' || raw_line[0] == '+') 
            && raw_line.size() > 1 && raw_line[1] == ' ') {
            line.is_list_item = true;
        }
    }

    // Extract images: ![alt](url)
    {
        size_t pos = display_text.find("![");
        if (pos != std::string::npos) {
            size_t end = display_text.find("](", pos);
            if (end != std::string::npos) {
                size_t close = display_text.find(")", end);
                if (close != std::string::npos) {
                    line.image_alt = display_text.substr(pos + 2, end - pos - 2);
                    line.image_url = display_text.substr(end + 2, close - end - 2);
                    // Remove the image syntax from display text, leave alt text
                    display_text.erase(pos, close - pos + 1);
                    if (!line.image_alt.empty()) {
                        display_text.insert(pos, "[" + line.image_alt + "]");
                    }
                }
            }
        }
    }

    // Extract links: [text](url)
    {
        size_t pos = display_text.find("[");
        if (pos != std::string::npos) {
            size_t close = display_text.find("]", pos);
            if (close != std::string::npos && close + 1 < display_text.size() && display_text[close+1] == '(') {
                size_t paren_close = display_text.find(")", close);
                if (paren_close != std::string::npos) {
                    std::string link_text = display_text.substr(pos + 1, close - pos - 1);
                    line.link_url = display_text.substr(close + 2, paren_close - close - 2);
                    line.is_link = true;
                    // Keep link text, remove the URL part
                    display_text.erase(close + 1, paren_close - close);
                }
            }
        }
    }

    // Remove HTML comments
    {
        size_t pos = 0;
        while ((pos = display_text.find("<!--", pos)) != std::string::npos) {
            size_t end = display_text.find("-->", pos);
            if (end != std::string::npos) {
                display_text.erase(pos, end - pos + 3);
            } else {
                display_text.erase(pos);
                break;
            }
        }
    }

    // Detect table separators (|---|---|) and skip them
    if (display_text.find("|") != std::string::npos && display_text.find("-") != std::string::npos) {
        int pipes = 0, dashes = 0;
        for (char c : display_text) {
            if (c == '|') pipes++;
            if (c == '-') dashes++;
        }
        if (pipes > 1 && dashes > 2) {
            line.text = "";
            return line;
        }
    }

    line.text = display_text;
    return line;
}

// Parse text file into document
TextDocument iv_parse_text_document(const unsigned char *data, size_t len, TextFormatType format) {
    TextDocument doc{};
    doc.format = format;
    doc.scroll_line = 0;

    if (!data || len == 0) return doc;

    std::string text((const char *)data, len);
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == '\n') {
            std::string line_str = text.substr(start, i - start);
            if (!line_str.empty() && line_str.back() == '\r') {
                line_str.pop_back();
            }

            TextLine line{};
            // Only parse as markdown if format is explicitly TEXT_MARKDOWN
            if (format == TEXT_MARKDOWN) {
                line = iv_parse_markdown_line(line_str);
            } else {
                // Plain text: just store as-is, no markdown parsing
                line.text = line_str;
                line.is_header = false;
                line.header_level = 0;
                line.is_code_block = false;
                line.is_list_item = false;
                line.is_link = false;
            }
            doc.lines.push_back(line);
            start = i + 1;
        }
    }

    return doc;
}

// Render text document
void iv_render_text_document(const TextDocument &doc, int viewport_x, int viewport_y, int viewport_w, int viewport_h) {
    if (doc.lines.empty()) return;

    int base_px = g_font_size;
    int line_height = (int)(base_px * 1.6f);
    int visible_lines = (viewport_h - IV_PAD * 2) / line_height;
    
    int max_scroll = std::max(0, (int)doc.lines.size() - visible_lines);
    int scroll = std::min(doc.scroll_line, max_scroll);

    // Background
    glColor4f(0.08f, 0.08f, 0.1f, 0.95f);
    glBegin(GL_QUADS);
    glVertex2f((float)viewport_x, (float)viewport_y);
    glVertex2f((float)(viewport_x + viewport_w), (float)viewport_y);
    glVertex2f((float)(viewport_x + viewport_w), (float)(viewport_y + viewport_h));
    glVertex2f((float)viewport_x, (float)(viewport_y + viewport_h));
    glEnd();

    // Border
    glColor4f(0.3f, 0.3f, 0.35f, 0.8f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)viewport_x, (float)viewport_y);
    glVertex2f((float)(viewport_x + viewport_w - 1), (float)viewport_y);
    glVertex2f((float)(viewport_x + viewport_w - 1), (float)(viewport_y + viewport_h - 1));
    glVertex2f((float)viewport_x, (float)(viewport_y + viewport_h - 1));
    glEnd();

    // Render lines
    int y = viewport_y + IV_PAD + IV_PAD;
    for (int i = 0; i < visible_lines && (scroll + i) < (int)doc.lines.size(); i++) {
        const TextLine &line = doc.lines[scroll + i];

        int font_px = base_px;
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

        if (line.is_header) {
            font_px = base_px + (7 - std::min(line.header_level, 6)) * 2;
            switch (line.header_level) {
                case 1: r = 0.4f; g = 0.8f; b = 1.0f; break;
                case 2: r = 0.3f; g = 0.7f; b = 1.0f; break;
                default: r = 0.5f; g = 0.7f; b = 1.0f; break;
            }
        } else if (line.is_code_block) {
            r = 0.3f; g = 0.9f; b = 0.3f;
            font_px = (int)(base_px * 0.9f);
        } else if (line.is_link) {
            // Links in cyan/blue with underline hint
            r = 0.2f; g = 0.6f; b = 1.0f;
        }

        int indent = 0;
        if (line.is_code_block) indent = base_px;
        else if (line.is_list_item) indent = base_px / 2;

        // Render the text line
        if (!line.text.empty()) {
            draw_text(line.text.c_str(),
                     (float)(viewport_x + IV_PAD + indent),
                     (float)y,
                     font_px, font_px,
                     r, g, b, a);
        }
        y += line_height;

        // If this line has an image, try to load and display it
        if (!line.image_url.empty()) {
            // Try to load image relative to current document location
            // For now, just display placeholder
            std::string img_path = line.image_url;
            
            // Skip remote/absolute URLs (start with http:// or https://)
            bool is_url = (img_path.find("http://") == 0 || img_path.find("https://") == 0);
            
            if (!is_url && !img_path.empty()) {
                // Try to load as local/relative path
                // Note: This is a simplified version. Full implementation would:
                // 1. Resolve relative paths from the document's directory
                // 2. Support both local and remote paths
                // 3. Cache loaded images
                // 4. Display them with proper sizing
                
                // For now, just show the alt text as a placeholder
                float img_y = y;
                draw_text(("[Image: " + line.image_alt + "]").c_str(),
                         (float)(viewport_x + IV_PAD + indent),
                         img_y,
                         font_px * 0.8f, font_px * 0.8f,
                         0.6f, 0.6f, 0.6f, 0.7f);
                y += line_height / 2;
            } else if (is_url) {
                // Show URL placeholder
                draw_text(("[Image URL: " + line.image_url.substr(0, 40) + "...]").c_str(),
                         (float)(viewport_x + IV_PAD + indent),
                         (float)y,
                         font_px * 0.7f, font_px * 0.7f,
                         0.5f, 0.5f, 0.5f, 0.5f);
                y += line_height / 2;
            }
        }
    }

    // Scroll indicator
    if (max_scroll > 0) {
        float scroll_ratio = (float)scroll / (float)max_scroll;
        float scroll_height = (float)visible_lines / (float)doc.lines.size();
        if (scroll_height < 1.0f) {
            float bar_y = viewport_y + IV_PAD + (float)viewport_h * scroll_ratio * 0.9f;
            float bar_h = (float)viewport_h * scroll_height * 0.9f;
            glColor4f(0.5f, 0.5f, 0.55f, 0.7f);
            glBegin(GL_QUADS);
            glVertex2f((float)(viewport_x + viewport_w - 6), bar_y);
            glVertex2f((float)(viewport_x + viewport_w - 2), bar_y);
            glVertex2f((float)(viewport_x + viewport_w - 2), bar_y + bar_h);
            glVertex2f((float)(viewport_x + viewport_w - 6), bar_y + bar_h);
            glEnd();
        }
    }
}

// Text keyboard navigation
void iv_text_keyboard(TextDocument &doc, SDL_Keycode sym) {
    int max_scroll = std::max(0, (int)doc.lines.size() - 20);
    switch (sym) {
        case SDLK_UP:
            doc.scroll_line = std::max(0, doc.scroll_line - 1);
            break;
        case SDLK_DOWN:
            doc.scroll_line = std::min(max_scroll, doc.scroll_line + 1);
            break;
        case SDLK_PAGEUP:
            doc.scroll_line = std::max(0, doc.scroll_line - 20);
            break;
        case SDLK_PAGEDOWN:
            doc.scroll_line = std::min(max_scroll, doc.scroll_line + 20);
            break;
        case SDLK_HOME:
            doc.scroll_line = 0;
            break;
        case SDLK_END:
            doc.scroll_line = max_scroll;
            break;
        default:
            break;
    }
}

// Text scroll
void iv_text_scroll(TextDocument &doc, int delta_y) {
    int max_scroll = std::max(0, (int)doc.lines.size() - 20);
    int step = (delta_y > 0) ? -3 : 3;  // wheel up = scroll up (negative step)
    doc.scroll_line = std::max(0, std::min(doc.scroll_line + step, max_scroll));
}

// ============================================================================
// SIZE HELPER
// ============================================================================

void fmt_size_iv(uint64_t sz, char *buf, int n) {
    if      (sz >= 1024*1024*1024) snprintf(buf, n, "%.1fG", sz/(1024.0*1024*1024));
    else if (sz >= 1024*1024)      snprintf(buf, n, "%.1fM", sz/(1024.0*1024));
    else if (sz >= 1024)           snprintf(buf, n, "%.1fK", sz/1024.0);
    else                           snprintf(buf, n, "%lluB", (unsigned long long)sz);
}

