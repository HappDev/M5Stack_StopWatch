#include "pattern.h"
#include "model.h"
#include <cmath>
#include <cstring>

namespace otp {
static int midpoint(int a, int b) {
    int ax = (a-1)%3, ay = (a-1)/3, bx = (b-1)%3, by = (b-1)/3;
    if (((ax+bx)%2) || ((ay+by)%2)) return 0;
    if (std::abs(ax-bx) != 2 && std::abs(ay-by) != 2) return 0;
    return (ay+by)/2*3 + (ax+bx)/2 + 1;
}
bool valid_pattern(const char* pattern) {
    if (!pattern) return false;
    size_t n = strlen(pattern);
    if (n < 4 || n > 6) return false;
    uint16_t seen = 0;
    for (size_t i=0; i<n; ++i) {
        int point = pattern[i]-'0';
        if (point < 1 || point > 9 || (seen & (1u<<point))) return false;
        int middle = i ? midpoint(pattern[i-1]-'0',point) : 0;
        if (middle && !(seen & (1u<<middle))) return false;
        seen |= 1u<<point;
    }
    return true;
}
void PatternGesture::clear() {
    wipe(points_,sizeof points_); length_=0; active_=overflow_=false; x_=y_=0;
}
bool PatternGesture::contains(int point) const {
    for (size_t i=0;i<length_;++i) if (points_[i]=='0'+point) return true;
    return false;
}
void PatternGesture::add(int point) {
    if (overflow_ || point<1 || point>9 || contains(point)) return;
    int middle=length_ ? midpoint(points_[length_-1]-'0',point) : 0;
    if (middle && !contains(middle)) add(middle);
    if (overflow_) return;
    if (length_==6) { overflow_=true; return; }
    points_[length_++]='0'+point; points_[length_]=0;
}
void PatternGesture::start(int x,int y) {
    clear(); active_=true; x_=x; y_=y; move(x,y);
}
void PatternGesture::move(int x,int y) {
    if (!active_ || overflow_) return;
    // Intersect the whole sampled segment, in travel order: a fast swipe must
    // select the same nodes as a slow swipe, even when samples skip a node.
    struct Hit { double t; int point; } hits[9]; size_t count=0;
    double dx=x-x_,dy=y-y_,a=dx*dx+dy*dy;
    for (int p=1;p<=9;++p) {
        if (contains(p)) continue;
        double ox=x_-center_x(p),oy=y_-center_y(p),c=ox*ox+oy*oy-Radius*Radius;
        double t=0;
        if (c>0) {
            if (!a) continue;
            double b=2*(ox*dx+oy*dy),disc=b*b-4*a*c;
            if (disc<0) continue;
            t=(-b-std::sqrt(disc))/(2*a);
            if (t<0 || t>1) continue;
        }
        hits[count++]={t,p};
    }
    // At most nine intersections: insertion sort avoids a general sort's
    // extra stack/code and GCC's small-array bounds warning on ESP32.
    for (size_t i=1;i<count;++i) {
        Hit hit=hits[i];size_t j=i;
        while (j && hits[j-1].t>hit.t) {hits[j]=hits[j-1];--j;}
        hits[j]=hit;
    }
    for (size_t i=0;i<count;++i) add(hits[i].point);
    x_=x; y_=y;
}
}
