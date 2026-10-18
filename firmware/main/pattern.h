#pragma once
#include <cstddef>
#include <cstdint>

namespace otp {
// Points are numbered 1..9, left to right, top to bottom.
bool valid_pattern(const char* pattern);

class PatternGesture {
public:
    static constexpr int Radius = 34;
    static constexpr int center_x(int point) { return 50 + ((point - 1) % 3) * 120; }
    static constexpr int center_y(int point) { return 50 + ((point - 1) / 3) * 120; }
    void clear();
    void start(int x, int y);
    void move(int x, int y);
    void add(int point);
    bool active() const { return active_; }
    bool valid() const { return !overflow_ && valid_pattern(points_); }
    bool overflow() const { return overflow_; }
    bool contains(int point) const;
    size_t size() const { return length_; }
    const char* points() const { return points_; }
private:
    char points_[7]{};
    size_t length_ = 0;
    bool active_ = false, overflow_ = false;
    int x_ = 0, y_ = 0;
};
}
