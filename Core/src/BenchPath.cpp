#include "ftcore/BenchPath.h"

#include <charconv>
#include <cmath>
#include <system_error>

namespace ftcore {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

void skipSpace(std::string_view line, std::size_t& pos) {
    while (pos < line.size() && isSpace(line[pos])) {
        ++pos;
    }
}

// Reads the next number from line at pos; false when there is none, it runs into another word, or it is not finite.
bool nextNumber(std::string_view line, std::size_t& pos, double& out) {
    skipSpace(line, pos);
    if (pos >= line.size()) {
        return false;
    }
    const char* first = line.data() + pos;
    const char* last = line.data() + line.size();
    const std::from_chars_result r = std::from_chars(first, last, out);
    if (r.ec != std::errc() || (r.ptr != last && !isSpace(*r.ptr)) || !std::isfinite(out)) {
        return false;
    }
    pos = static_cast<std::size_t>(r.ptr - line.data());
    return true;
}

double catmull(double p0, double p1, double p2, double p3, double u) {
    const double u2 = u * u;
    const double u3 = u2 * u;
    return 0.5 * ((2.0 * p1) + (-p0 + p2) * u + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * u2 +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * u3);
}

} // namespace

bool BenchPath::parse(std::string_view text, int* badLine) {
    count_ = 0;
    int lineNo = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        ++lineNo;
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        const std::size_t hash = line.find('#');
        if (hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        std::size_t pos = 0;
        skipSpace(line, pos);
        if (pos >= line.size()) {
            continue;
        }
        double v[7] = {};
        bool ok = count_ < kMaxKeys;
        for (int i = 0; ok && i < 7; ++i) {
            ok = nextNumber(line, pos, v[i]);
        }
        if (ok) {
            skipSpace(line, pos);
            ok = pos == line.size() && (count_ == 0 || v[0] > keys_[count_ - 1].t);
        }
        if (!ok) {
            count_ = 0;
            if (badLine) {
                *badLine = lineNo;
            }
            return false;
        }
        BenchKey& k = keys_[count_++];
        k.t = v[0];
        for (int i = 0; i < 3; ++i) {
            k.pose.eye[i] = v[1 + i];
            k.pose.at[i] = v[4 + i];
        }
    }
    if (count_ < 2) {
        count_ = 0;
        if (badLine) {
            *badLine = 0;
        }
        return false;
    }
    return true;
}

BenchPose BenchPath::sample(double t) const {
    if (count_ == 0) {
        return BenchPose{};
    }
    const double at = keys_[0].t + t;
    if (count_ == 1 || at <= keys_[0].t) {
        return keys_[0].pose;
    }
    if (at >= keys_[count_ - 1].t) {
        return keys_[count_ - 1].pose;
    }
    std::size_t i = 0;
    while (i + 2 < count_ && keys_[i + 1].t <= at) {
        ++i;
    }
    const BenchKey& k1 = keys_[i];
    const BenchKey& k2 = keys_[i + 1];
    const BenchKey& k0 = i == 0 ? k1 : keys_[i - 1];
    const BenchKey& k3 = i + 2 < count_ ? keys_[i + 2] : k2;
    const double u = (at - k1.t) / (k2.t - k1.t);
    BenchPose out;
    for (int c = 0; c < 3; ++c) {
        out.eye[c] = catmull(k0.pose.eye[c], k1.pose.eye[c], k2.pose.eye[c], k3.pose.eye[c], u);
        out.at[c] = catmull(k0.pose.at[c], k1.pose.at[c], k2.pose.at[c], k3.pose.at[c], u);
    }
    return out;
}

} // namespace ftcore
