#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dense {

constexpr float pi = 3.14159265358979323846f;

struct Vec3 {
    float x{}, y{}, z{};
    constexpr Vec3 operator+(Vec3 b) const { return {x+b.x, y+b.y, z+b.z}; }
    constexpr Vec3 operator-(Vec3 b) const { return {x-b.x, y-b.y, z-b.z}; }
    constexpr Vec3 operator*(float s) const { return {x*s, y*s, z*s}; }
    constexpr Vec3 operator/(float s) const { return {x/s, y/s, z/s}; }
    Vec3& operator+=(Vec3 b) { x+=b.x; y+=b.y; z+=b.z; return *this; }
};

inline float dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
inline float lengthSq(Vec3 v) { return dot(v,v); }
inline float length(Vec3 v) { return std::sqrt(lengthSq(v)); }
inline Vec3 normalize(Vec3 v) { const float n=length(v); return n>1e-6f ? v/n : Vec3{0,1,0}; }
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a+(b-a)*t; }
inline float clamp(float v, float lo, float hi) { return std::max(lo,std::min(hi,v)); }

struct Mat4 { float m[16]{}; };

inline Mat4 identity() { Mat4 r{}; r.m[0]=r.m[5]=r.m[10]=r.m[15]=1; return r; }
inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for(int row=0;row<4;++row) for(int col=0;col<4;++col)
        for(int k=0;k<4;++k) r.m[row*4+col]+=a.m[row*4+k]*b.m[k*4+col];
    return r;
}
inline Mat4 perspective(float fovY, float aspect, float zn, float zf) {
    Mat4 r{}; const float y=1/std::tan(fovY*0.5f), x=y/aspect;
    r.m[0]=x; r.m[5]=y; r.m[10]=zf/(zf-zn); r.m[11]=1; r.m[14]=-zn*zf/(zf-zn); return r;
}
inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 z=normalize(target-eye), x=normalize(cross(up,z)), y=cross(z,x);
    Mat4 r=identity();
    r.m[0]=x.x; r.m[1]=y.x; r.m[2]=z.x;
    r.m[4]=x.y; r.m[5]=y.y; r.m[6]=z.y;
    r.m[8]=x.z; r.m[9]=y.z; r.m[10]=z.z;
    r.m[12]=-dot(x,eye); r.m[13]=-dot(y,eye); r.m[14]=-dot(z,eye); return r;
}

class Rng {
public:
    explicit Rng(uint32_t seed): state_(seed ? seed : 1u) {}
    uint32_t next() { uint32_t x=state_; x^=x<<13; x^=x>>17; x^=x<<5; return state_=x; }
    float unit() { return static_cast<float>(next()>>8)*(1.0f/16777216.0f); }
    float range(float a,float b) { return a+(b-a)*unit(); }
private: uint32_t state_;
};

}
