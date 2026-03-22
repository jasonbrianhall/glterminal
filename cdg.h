#ifndef CDG_H
#define CDG_H

#include <stdint.h>
#include <stdbool.h>

// CD+G screen dimensions
#define CDG_WIDTH 300
#define CDG_HEIGHT 216
#define CDG_COLORS 16
#define CDG_PACKETS_PER_SECOND 75

// CD+G commands
#define CDG_COMMAND_MASK 0x3F
#define CDG_COMMAND_GRAPHICS 0x09

// CD+G instructions
#define CDG_INST_MEMORY_PRESET 1
#define CDG_INST_BORDER_PRESET 2
#define CDG_INST_TILE_BLOCK 6
#define CDG_INST_SCROLL_PRESET 20
#define CDG_INST_SCROLL_COPY 24
#define CDG_INST_DEFINE_TRANSPARENT 28
#define CDG_INST_LOAD_COLORMAP_LOW 30
#define CDG_INST_LOAD_COLORMAP_HIGH 31

// Tile dimensions
#define CDG_TILE_WIDTH 6
#define CDG_TILE_HEIGHT 12

typedef struct {
    uint8_t command;
    uint8_t instruction;
    uint8_t data[16];
} CDGPacket;

typedef struct {
    uint8_t screen[CDG_HEIGHT][CDG_WIDTH];  // Indexed color buffer
    uint32_t palette[CDG_COLORS];           // RGB888 colors
    uint8_t border_color;
    uint8_t transparent_color;
    
    CDGPacket *packets;
    int packet_count;
    int current_packet;
    
    // Karaoke ball state
    double ball_x;
    double ball_y;
    double ball_velocity_y;
    double ball_radius;
    double ball_trail_positions[20][2];  // Trail effect
    int ball_trail_index;
    
    // Timing
    double last_update_time;
} CDGDisplay;

// Function declarations
CDGDisplay* cdg_display_new(void);
void cdg_display_free(CDGDisplay *display);
bool cdg_load_file(CDGDisplay *display, const char *filename);
void cdg_update(CDGDisplay *display, double playTime);
void cdg_reset(CDGDisplay *display);
void cdg_process_packet(CDGDisplay *display, CDGPacket *packet);

// Drawing helpers
void cdg_draw_tile(CDGDisplay *display, int col, int row, uint8_t color0, uint8_t color1, uint8_t *tile_data);
void cdg_scroll_screen(CDGDisplay *display, int h_cmd, int v_cmd, uint8_t fill_color);
void cdg_load_colormap(CDGDisplay *display, uint8_t *data, bool high_colors);

#endif // CDG_H
