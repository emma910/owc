#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace owc {

struct Config {
    int aim_key = 1;
    int aim_mode = 0;                       // 0 = TRACKING, 1 = FLICKING
    int flick_shoot_pixels = 5;
    int64_t flick_pause_duration_ms = 300;
    float sensitivity = 15.0f;
    float fps = 30.0f;
    float aim_duration_millis = 3.5f;
    float aim_duration_multiplier_base = 1.0f;
    float aim_duration_multiplier_max = 2.0f;
    int aim_max_move_pixels = 3;
    int aim_jitter_percent = 0;
    int aim_min_target_width = 8;
    int aim_min_target_height = 8;
    int box_width = 256;
    int box_height = 256;
    float max_snap_divisor = 2.0f;
    std::vector<uint32_t> target_colors;    // 0xRRGGBB
    int target_color_tolerance = 8;
    int mouse_id = 11;
    int keyboard_id = 1;
    float aim_offset_x = 1.0f;
    float aim_offset_y = 0.75f;
    bool toggle_in_game_ui = true;
    std::vector<uint32_t> toggle_key_codes; // VK codes
    int aim_precise_sleeper_type = 0;
    int aim_cpu_thread_affinity_index = -1;
};

Config loadConfig(const char* path);

} // namespace owc