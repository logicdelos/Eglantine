// SPDX-FileCopyrightText: Copyright 2026 BasicUpscaler contributors
// SPDX-License-Identifier: MIT

// BUF, the BasicUpscaler Formula (F-FABLE-44): FSR1's EASU with the sharpening folded into its
// one pass. Based on AMD FidelityFX Super Resolution 1.0 (MIT); AMD's notice is retained below.
// It is not FSR1. Differences from FsrEasuF (ffx_fsr1.h), in order:
//   1. the 12 taps' squared kernel distances come from one expanded quadratic form: the same
//      arithmetic with fewer operations (equal up to float rounding);
//   2. each cross also sums a "hardness": how close EASU's own ratio |D - B| / max(|D - C|, |C - B|)
//      is to 1, as it is on a one-pixel step (one side flat; a linear ramp gives 2);
//   3. after the kernel, unsharp masking at the INPUT pitch: the bilinear blend of the four
//      crosses' 5-point Laplacians, weight beta(s) = 0.288 (1 - 1/s^2) (0.16 at 1.5x, 0.22 at 2x)
//      times the strength, faded out on hard one-pixel steps; the 2x2 clamp then bounds the result;
//   4. no RCAS pass: the result is drawn straight onto the target.
// GLSL port of BasicUpscaler's shaders/buf.hlsl; same constant words as FsrEasuCon.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_GOOGLE_include_directive : enable

layout(push_constant) uniform constants {
    uvec4 Const0; // FsrEasuCon's words, unchanged
    uvec4 Const1;
    uvec4 Const2;
    uvec4 Const3;
    vec4 Params;  // x: strength (1 = the formula as tuned); yz: output size in pixels
};

layout(set = 0, binding = 0) uniform sampler2D InputTexture;

layout(location = 0) in vec2 frag_texcoord;
layout(location = 0) out vec4 frag_color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

AF4 BufGatherR(AF2 p) { return textureGather(InputTexture, p, 0); }
AF4 BufGatherG(AF2 p) { return textureGather(InputTexture, p, 1); }
AF4 BufGatherB(AF2 p) { return textureGather(InputTexture, p, 2); }

// One cross: FsrEasuSetF's operations in its order, plus the hardness sums
// (hard.x: |gradient|-weighted hardness, hard.y: the sum of |gradient|).
void BufSetF(inout AF2 dir, inout AF1 len, inout AF2 hard, AF1 w, AF1 lA, AF1 lB, AF1 lC, AF1 lD,
             AF1 lE) {
    AF1 dc = lD - lC;
    AF1 cb = lC - lB;
    AF1 lenX = max(abs(dc), abs(cb));
    lenX = APrxLoRcpF1(lenX);
    AF1 dirX = lD - lB;
    dir.x += dirX * w;
    AF1 rX = abs(dirX) * lenX;
    lenX = ASatF1(rX);
    lenX *= lenX;
    len += lenX * w;
    AF1 ec = lE - lC;
    AF1 ca = lC - lA;
    AF1 lenY = max(abs(ec), abs(ca));
    lenY = APrxLoRcpF1(lenY);
    AF1 dirY = lE - lA;
    dir.y += dirY * w;
    AF1 rY = abs(dirY) * lenY;
    lenY = ASatF1(rY);
    lenY *= lenY;
    len += lenY * w;
    // Hardness is 1 at ratio r = 1, a one-pixel step, and 0 at r = 1 +- 1/2 and beyond.
    AF1 hX = ASatF1(AF1_(1.0) - AF1_(2.0) * abs(rX - AF1_(1.0)));
    AF1 hY = ASatF1(AF1_(1.0) - AF1_(2.0) * abs(rY - AF1_(1.0)));
    hard.x += w * (abs(dirX) * hX + abs(dirY) * hY);
    hard.y += w * (abs(dirX) + abs(dirY));
}

// One tap, its squared kernel distance already known: FsrEasuTapF's kernel.
void BufTapF(inout AF3 aC, inout AF1 aW, AF1 d2, AF1 lob, AF1 clp, AF3 c) {
    d2 = min(d2, clp);
    AF1 wB = AF1_(2.0 / 5.0) * d2 + AF1_(-1.0);
    AF1 wA = lob * d2 + AF1_(-1.0);
    wB *= wB;
    wA *= wA;
    wB = AF1_(25.0 / 16.0) * wB + AF1_(-(25.0 / 16.0 - 1.0));
    AF1 w = wB * wA;
    aC += c * w;
    aW += w;
}

AF3 BufF(AU2 ip, AU4 con0, AU4 con1, AU4 con2, AU4 con3, AF1 strength, AF2 out_size) {
    // FsrEasuF's sample positions, 12 taps (b c / e f g h / i j k l / n o) and luma.
    AF2 pp = AF2(ip) * AF2_AU2(con0.xy) + AF2_AU2(con0.zw);
    AF2 fp = floor(pp);
    pp -= fp;
    AF2 p0 = fp * AF2_AU2(con1.xy) + AF2_AU2(con1.zw);
    AF2 p1 = p0 + AF2_AU2(con2.xy);
    AF2 p2 = p0 + AF2_AU2(con2.zw);
    AF2 p3 = p0 + AF2_AU2(con3.xy);
    AF4 bczzR = BufGatherR(p0);
    AF4 bczzG = BufGatherG(p0);
    AF4 bczzB = BufGatherB(p0);
    AF4 ijfeR = BufGatherR(p1);
    AF4 ijfeG = BufGatherG(p1);
    AF4 ijfeB = BufGatherB(p1);
    AF4 klhgR = BufGatherR(p2);
    AF4 klhgG = BufGatherG(p2);
    AF4 klhgB = BufGatherB(p2);
    AF4 zzonR = BufGatherR(p3);
    AF4 zzonG = BufGatherG(p3);
    AF4 zzonB = BufGatherB(p3);
    AF4 bczzL = bczzB * AF4_(0.5) + (bczzR * AF4_(0.5) + bczzG);
    AF4 ijfeL = ijfeB * AF4_(0.5) + (ijfeR * AF4_(0.5) + ijfeG);
    AF4 klhgL = klhgB * AF4_(0.5) + (klhgR * AF4_(0.5) + klhgG);
    AF4 zzonL = zzonB * AF4_(0.5) + (zzonR * AF4_(0.5) + zzonG);
    AF3 b = AF3(bczzR.x, bczzG.x, bczzB.x), c = AF3(bczzR.y, bczzG.y, bczzB.y);
    AF3 i = AF3(ijfeR.x, ijfeG.x, ijfeB.x), j = AF3(ijfeR.y, ijfeG.y, ijfeB.y);
    AF3 f = AF3(ijfeR.z, ijfeG.z, ijfeB.z), e = AF3(ijfeR.w, ijfeG.w, ijfeB.w);
    AF3 k = AF3(klhgR.x, klhgG.x, klhgB.x), l = AF3(klhgR.y, klhgG.y, klhgB.y);
    AF3 h = AF3(klhgR.z, klhgG.z, klhgB.z), g = AF3(klhgR.w, klhgG.w, klhgB.w);
    AF3 o = AF3(zzonR.z, zzonG.z, zzonB.z), n = AF3(zzonR.w, zzonG.w, zzonB.w);
    AF1 bL = bczzL.x, cL = bczzL.y, iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
    AF1 kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w, oL = zzonL.z, nL = zzonL.w;
    AF2 dir = AF2_(0.0);
    AF1 len = AF1_(0.0);
    AF2 hard = AF2_(0.0);
    AF1 wS = (AF1_(1.0) - pp.x) * (AF1_(1.0) - pp.y), wT = pp.x * (AF1_(1.0) - pp.y);
    AF1 wU = (AF1_(1.0) - pp.x) * pp.y, wV = pp.x * pp.y;
    BufSetF(dir, len, hard, wS, bL, eL, fL, gL, jL);
    BufSetF(dir, len, hard, wT, cL, fL, gL, hL, kL);
    BufSetF(dir, len, hard, wU, fL, iL, jL, kL, nL);
    BufSetF(dir, len, hard, wV, gL, jL, kL, lL, oL);
    AF1 lRaw = len;
    AF2 dir2 = dir * dir;
    AF1 dirR = dir2.x + dir2.y;
    AP1 zro = dirR < AF1_(1.0 / 32768.0);
    dirR = APrxLoRsqF1(dirR);
    dirR = zro ? AF1_(1.0) : dirR;
    dir.x = zro ? AF1_(1.0) : dir.x;
    dir *= AF2_(dirR);
    len = len * AF1_(0.5);
    len *= len;
    AF1 stretch = (dir.x * dir.x + dir.y * dir.y) * APrxLoRcpF1(max(abs(dir.x), abs(dir.y)));
    AF2 len2 = AF2(AF1_(1.0) + (stretch - AF1_(1.0)) * len, AF1_(1.0) + AF1_(-0.5) * len);
    AF1 lob = AF1_(0.5) + AF1_((1.0 / 4.0 - 0.04) - 0.5) * len;
    AF1 clp = APrxLoRcpF1(lob);
    AF3 min4 = min(AMin3F3(f, g, j), k);
    AF3 max4 = max(AMax3F3(f, g, j), k);
    AF3 aC = AF3_(0.0);
    AF1 aW = AF1_(0.0);
    {
        // EASU's squared distance is a quadratic form in the tap offset: d2 = off^T M off with
        // M = len2.x^2 d d^T + len2.y^2 t t^T (d = dir, t = its perpendicular). Expanded around
        // tap f, the 12 rotations become sums of 6 shared terms.
        AF2 l2 = len2 * len2;
        AF3 m = AF3(l2.x * dir.x * dir.x + l2.y * dir.y * dir.y, (l2.x - l2.y) * dir.x * dir.y,
                    l2.x * dir.y * dir.y + l2.y * dir.x * dir.x);
        AF1 u = -pp.x, v = -pp.y;
        AF1 F = m.x * u * u + AF1_(2.0) * m.y * u * v + m.z * v * v;
        AF1 Dx = AF1_(2.0) * (m.x * u + m.y * v);
        AF1 Dy = AF1_(2.0) * (m.y * u + m.z * v);
        AF1 Fx = F + m.x, Fy = F + m.z;
        AF1 Fxy = Fx + m.z;
        AF1 m2 = AF1_(2.0) * m.y;
        BufTapF(aC, aW, Fy - Dy, lob, clp, b);
        BufTapF(aC, aW, Fxy + Dx - Dy - m2, lob, clp, c);
        BufTapF(aC, aW, Fxy - Dx + Dy - m2, lob, clp, i);
        BufTapF(aC, aW, Fy + Dy, lob, clp, j);
        BufTapF(aC, aW, F, lob, clp, f);
        BufTapF(aC, aW, Fx - Dx, lob, clp, e);
        BufTapF(aC, aW, Fxy + Dx + Dy + m2, lob, clp, k);
        BufTapF(aC, aW, Fy + AF1_(4.0) * m.x + AF1_(2.0) * (Dx + m2) + Dy, lob, clp, l);
        BufTapF(aC, aW, F + AF1_(4.0) * m.x + AF1_(2.0) * Dx, lob, clp, h);
        BufTapF(aC, aW, Fx + Dx, lob, clp, g);
        BufTapF(aC, aW, Fx + AF1_(4.0) * m.z + AF1_(2.0) * (Dy + m2) + Dx, lob, clp, o);
        BufTapF(aC, aW, F + AF1_(4.0) * m.z + AF1_(2.0) * Dy, lob, clp, n);
    }
    AF1 rW = ARcpF1(aW);
    AF3 raw = aC * AF3_(rW);
    {
        // Unsharp masking at the input pitch: the bilinear blend of the four crosses' 5-point
        // Laplacians, weight beta(s) = 0.288 (1 - 1/s^2) with s^2 = output pixels / input pixels,
        // faded out on hard one-pixel steps. The 2x2 clamp below bounds it.
        AF3 lap = wS * (b + e + g + j - AF3_(4.0) * f) + wT * (c + f + h + k - AF3_(4.0) * g) +
                  wU * (f + i + k + n - AF3_(4.0) * j) + wV * (g + j + l + o - AF3_(4.0) * k);
        AF1 s2 = (out_size.x * AF1_AU1(con1.x)) * (out_size.y * AF1_AU1(con1.y));
        AF1 beta = AF1_(0.288) * strength * max(AF1_(0.0), AF1_(1.0) - AF1_(1.0) / s2);
        AF1 uHard = (hard.x / max(hard.y, AF1_(1.0e-6))) * ASatF1(lRaw);
        uHard *= uHard;
        raw -= (beta * (AF1_(1.0) - uHard)) * lap;
    }
    return min(max4, max(min4, raw));
}

void main() {
    AF3 c = BufF(AU2(gl_FragCoord.xy), Const0, Const1, Const2, Const3, Params.x, Params.yz);
    frag_color = AF4(c, texture(InputTexture, frag_texcoord).a);
}
