#pragma once
/* Scene -> SVG.  A second consumer of the same primitive list, useful for the
   things a window cannot do: put a frame in a document, diff two frames, or
   keep a failing run as evidence.  It also means the geometry is verifiable
   without opening a window at all, which is what makes the drawing code
   testable offline. */

#include "scene.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

namespace simcore_view {

namespace svg_detail {

inline std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char character : text) {
        switch (character) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += character; break;
        }
    }
    return out;
}

inline std::string rgb(const Rgb& color) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", color.r, color.g, color.b);
    return text;
}

inline std::string number(float value) {
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(value));
    std::string out = text;
    /* Trim the noise a fixed-precision float leaves behind. */
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
    return out.empty() ? "0" : out;
}

}  // namespace svg_detail

inline std::string toSvg(const Scene& scene, float legend_width = 360.0f) {
    const float legend_x = scene.width + 16.0f;
    const float width = scene.width + legend_width;
    const float height = std::max(scene.height, 60.0f + 16.0f * static_cast<float>(scene.notes.size()) + 40.0f);
    std::string out;
    out += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + svg_detail::number(width) + "\" height=\"" +
           svg_detail::number(height) + "\" viewBox=\"0 0 " + svg_detail::number(width) + " " +
           svg_detail::number(height) + "\">\n";
    out += "<rect x=\"0\" y=\"0\" width=\"" + svg_detail::number(width) + "\" height=\"" +
           svg_detail::number(height) + "\" fill=\"#ffffff\"/>\n";

    for (const SceneLine& line : scene.lines) {
        out += "<line x1=\"" + svg_detail::number(line.x1) + "\" y1=\"" + svg_detail::number(line.y1) + "\" x2=\"" +
               svg_detail::number(line.x2) + "\" y2=\"" + svg_detail::number(line.y2) + "\" stroke=\"" +
               svg_detail::rgb(line.color) + "\" stroke-width=\"" + svg_detail::number(line.width) +
               "\" stroke-linecap=\"round\"/>\n";
    }
    for (const SceneRect& rect : scene.rects) {
        out += "<rect x=\"" + svg_detail::number(rect.x) + "\" y=\"" + svg_detail::number(rect.y) + "\" width=\"" +
               svg_detail::number(rect.w) + "\" height=\"" + svg_detail::number(rect.h) + "\" rx=\"4\" fill=\"" +
               svg_detail::rgb(rect.fill) + "\" stroke=\"" + svg_detail::rgb(rect.stroke) +
               "\" stroke-width=\"1.5\"/>\n";
    }
    for (const SceneDot& dot : scene.dots) {
        out += "<circle cx=\"" + svg_detail::number(dot.x) + "\" cy=\"" + svg_detail::number(dot.y) + "\" r=\"" +
               svg_detail::number(dot.r) + "\" fill=\"" + svg_detail::rgb(dot.fill) + "\" stroke=\"" +
               svg_detail::rgb(dot.stroke) + "\" stroke-width=\"1\"/>\n";
    }
    for (const SceneRect& rect : scene.rects) {
        if (rect.label.empty()) continue;
        const float centre_x = rect.x + rect.w * 0.5f;
        const float base_y = rect.y + rect.h * 0.5f + 4.0f;
        out += "<text x=\"" + svg_detail::number(centre_x) + "\" y=\"" + svg_detail::number(base_y) +
               "\" font-family=\"Consolas,monospace\" font-size=\"12\" fill=\"" + svg_detail::rgb(kLabel) +
               "\" text-anchor=\"middle\">" + svg_detail::escape(rect.label) + "</text>\n";
        if (!rect.sub_label.empty()) {
            out += "<text x=\"" + svg_detail::number(centre_x) + "\" y=\"" +
                   svg_detail::number(base_y + 14.0f) +
                   "\" font-family=\"Consolas,monospace\" font-size=\"10\" fill=\"#77777f\" "
                   "text-anchor=\"middle\">" +
                   svg_detail::escape(rect.sub_label) + "</text>\n";
        }
    }

    float note_y = 40.0f;
    out += "<text x=\"" + svg_detail::number(legend_x) + "\" y=\"20\" font-family=\"Consolas,monospace\" "
           "font-size=\"13\" fill=\"" + svg_detail::rgb(kLabel) + "\">simcore view</text>\n";
    const char* legend_lines[] = {"0", "1", "x", "z"};
    const tcsim::Logic legend_values[] = {tcsim::Logic::kZero, tcsim::Logic::kOne, tcsim::Logic::kX,
                                          tcsim::Logic::kZ};
    for (int index = 0; index < 4; ++index) {
        const float x = legend_x + static_cast<float>(index) * 34.0f;
        out += "<rect x=\"" + svg_detail::number(x) + "\" y=\"" + svg_detail::number(note_y - 10.0f) +
               "\" width=\"18\" height=\"12\" fill=\"" + svg_detail::rgb(valueColor(legend_values[index])) +
               "\"/>\n";
        out += "<text x=\"" + svg_detail::number(x + 24.0f) + "\" y=\"" + svg_detail::number(note_y) +
               "\" font-family=\"Consolas,monospace\" font-size=\"11\" fill=\"" +
               svg_detail::rgb(kLabel) + "\">" + legend_lines[index] + "</text>\n";
    }
    note_y += 30.0f;
    if (scene.notes.empty()) {
        out += "<text x=\"" + svg_detail::number(legend_x) + "\" y=\"" + svg_detail::number(note_y) +
               "\" font-family=\"Consolas,monospace\" font-size=\"11\" fill=\"#3a7a3a\">no notes</text>\n";
    }
    for (const std::string& note : scene.notes) {
        out += "<text x=\"" + svg_detail::number(legend_x) + "\" y=\"" + svg_detail::number(note_y) +
               "\" font-family=\"Consolas,monospace\" font-size=\"11\" fill=\"" + svg_detail::rgb(kNote) + "\">" +
               svg_detail::escape(note) + "</text>\n";
        note_y += 16.0f;
    }
    out += "</svg>\n";
    return out;
}

inline bool writeSvg(const std::string& path, const Scene& scene, float legend_width = 360.0f) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file << toSvg(scene, legend_width);
    return static_cast<bool>(file);
}

}  // namespace simcore_view
