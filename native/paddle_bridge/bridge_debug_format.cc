#include "bridge_debug_format.h"

#include <iomanip>
#include <sstream>

std::string quote_polygon(const std::array<FloatPoint, 4>& pts) {
    std::ostringstream s;
    s << "[";
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i > 0) {
            s << ",";
        }
        s << "[" << std::fixed << std::setprecision(6) << pts[i].x << "," << pts[i].y << "]";
    }
    s << "]";
    return s.str();
}

std::string quote_points(const std::vector<FloatPoint>& pts) {
    std::ostringstream s;
    s << "[";
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i > 0) {
            s << ",";
        }
        s << "[" << std::fixed << std::setprecision(3) << pts[i].x << "," << pts[i].y << "]";
    }
    s << "]";
    return s.str();
}
