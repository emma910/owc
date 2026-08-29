#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace owc {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<uint32_t> parseHexArray(const std::string& v) {
    std::vector<uint32_t> out;
    std::stringstream ss(v);
    std::string t;
    while (std::getline(ss, t, ',')) {
        t = trim(t);
        if (!t.empty()) out.push_back((uint32_t)strtoul(t.c_str(), nullptr, 16));
    }
    return out;
}

double parseNum(const std::string& v) { return strtod(v.c_str(), nullptr); }
int64_t parseInt64(const std::string& v) { return (int64_t)strtoll(v.c_str(), nullptr, 10); }

void apply(Config& c, const std::string& key, const std::string& raw) {
    const std::string v = trim(raw);
    if (key == "aim_key") c.aim_key = (int)parseInt64(v);
    else if (key == "aim_mode") c.aim_mode = (int)parseInt64(v);
    else if (key == "flick_shoot_pixels") c.flick_shoot_pixels = (int)parseInt64(v);
    else if (key == "flick_pause_duration") c.flick_pause_duration_ms = parseInt64(v);
    else if (key == "sensitivity") c.sensitivity = (float)parseNum(v);
    else if (key == "fps") c.fps = (float)parseNum(v);
    else if (key == "aim_duration_millis") c.aim_duration_millis = (float)parseNum(v);
    else if (key == "aim_duration_multiplier_base") c.aim_duration_multiplier_base = (float)parseNum(v);
    else if (key == "aim_duration_multiplier_max") c.aim_duration_multiplier_max = (float)parseNum(v);
    else if (key == "aim_max_move_pixels") c.aim_max_move_pixels = (int)parseInt64(v);
    else if (key == "aim_jitter_percent") c.aim_jitter_percent = (int)parseInt64(v);
    else if (key == "aim_min_target_width") c.aim_min_target_width = (int)parseInt64(v);
    else if (key == "aim_min_target_height") c.aim_min_target_height = (int)parseInt64(v);
    else if (key == "box_width") c.box_width = (int)parseInt64(v);
    else if (key == "box_height") c.box_height = (int)parseInt64(v);
    else if (key == "max_snap_divisor") c.max_snap_divisor = (float)parseNum(v);
    else if (key == "target_colors") c.target_colors = parseHexArray(v);
    else if (key == "target_color_tolerance") c.target_color_tolerance = (int)parseInt64(v);
    else if (key == "mouse_id") c.mouse_id = (int)parseInt64(v);
    else if (key == "keyboard_id") c.keyboard_id = (int)parseInt64(v);
    else if (key == "aim_offset_x") c.aim_offset_x = (float)parseNum(v);
    else if (key == "aim_offset_y") c.aim_offset_y = (float)parseNum(v);
    else if (key == "toggle_in_game_ui") c.toggle_in_game_ui = (v == "true" || v == "1" || v == "yes");
    else if (key == "toggle_key_codes") c.toggle_key_codes = parseHexArray(v);
    else if (key == "aim_precise_sleeper_type") c.aim_precise_sleeper_type = (int)parseInt64(v);
    else if (key == "aim_cpu_thread_affinity_index") c.aim_cpu_thread_affinity_index = (int)parseInt64(v);
}

} // namespace

Config loadConfig(const char* path) {
    Config c;

    if (c.target_colors.empty()) {
        c.target_colors = {
            0xd521cd,0xd722cf,0xd623ce,0xd722ce,0xd621cd,0xce19ca,0xd11ccb,0xd21dca,
            0xc818cf,0xd722cd,0xd722ce,0xcd19c9,0xc617d3,0xcb17c5,0xda25d3,0xce24cc,
            0xd328cc,0xdb32ef,0xbd15c4,0xdc5bea,0xda59eb,0xd959e9,0xf444fb,0xcf1ac9,
            0xd422d4,0xd923cd,0xe53af2,0xd321d3,0xe539f3,0xe035ed,0xd822cc,0xe83df5,
            0xd11fd1,0xd622d0,0xd21dcc,0xd429e2,0xe537ef,0xd923cd,0xe136ee,0xd321d3,
            0xe63bf3,0xd722cf,0xe036ee,0xd72ce6,0xd428e1,0xd321d3,0xd21dcc,0xdf34ed,
            0xd822cc,0xe434e6,0xd43ddf,0xde30e4,0xbe0dbe,0xd823d3,0xc814c4,0xc20ab7,
            0xde1ec1,0xca16c6,0xc30ebe,0xbb0fbf,0xc510bf,0xc10cbc,0xd21cb6,0xca14c5,
            0xb80cd1,0xae0ea8,0xbf0ec3,0xd415c1,0xbc22b7,0xd317c4,0xb1179d,0xbc0fb4,
            0xcc47c7,0xb834b5,0xdc2cd9,0xd727d5,0xde30da,0xc834c6
        };
    }
    if (c.toggle_key_codes.empty()) {
        c.toggle_key_codes = { 0x12, 0x5A }; // VK_ALT, VK_Z
    }

    std::ifstream f(path);
    if (!f) {
        fprintf(stderr, "[config] could not open '%s', using defaults\n", path);
        return c;
    }

    std::string line;
    while (std::getline(f, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        apply(c, trim(s.substr(0, eq)), s.substr(eq + 1));
    }
    return c;
}

} // namespace owc