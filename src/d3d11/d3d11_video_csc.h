#pragma once

// The colour transform of the video processor blit: one affine 3x4 matrix from the
// stored values of the input view to the stored values of the output view. It
// covers the YCbCr matrix (BT.601 or BT.709), the range of YCbCr (studio 16-235 /
// 16-240 or full 0-255) and the range of RGB (16-235 or 0-255), in each direction:
// YCbCr to RGB, RGB to YCbCr, YCbCr to YCbCr and RGB to RGB. Primaries and transfer
// functions do not change: the legacy colour space of D3D11 states neither, so
// both sides are taken to have the same primaries and the same transfer.
//
// The vector order is the order of the blit shader: a YCbCr texel is (Cr, Y, Cb)
// in the shader's (r, g, b), as both the planar fetch and a packed 4:2:2 view give
// it, and an RGB texel is (R, G, B). The shader computes out = M * (in, 1).
//
// The coefficients are those of ITU-R BT.601 (Kr 0.299, Kb 0.114) and ITU-R BT.709
// (Kr 0.2126, Kb 0.0722). YCbCr codes have <bits> significant bits: studio range is
// Y 16..235 and chroma 16..240 times 2^(bits-8), with the chroma centre at
// 2^(bits-1); full range is 0..2^bits-1. The shader samples a UNORM view of
// <storageBits> per channel, and a format such as P010 keeps its 10-bit code in
// the high bits of a 16-bit word: the sampled value is code * 2^(storageBits-bits)
// / (2^storageBits - 1). The matrix decodes and encodes those codes exactly. An
// 8-bit fraction on a P010 sample is 2.2 8-bit codes off after the studio
// expansion (BD-223, kit-vddi2 lab run: bar 2 B 189 for 191.2). RGB views are
// read as fractions of their range.

#include <cstdint>

#include <d3d11.h>

namespace dxvk {

  struct D3D11VideoCscSpace {
    bool ycbcr  = false;  // the data is YCbCr
    bool bt709  = false;  // the YCbCr matrix is BT.709 (else BT.601)
    bool studio = false;  // limited range: Y and RGB 16..235, chroma 16..240
    uint32_t bits        = 8;  // significant bits of a YCbCr code
    uint32_t storageBits = 8;  // bits of the UNORM channel the shader samples
  };

  // The code width of a YCbCr format: P010 holds 10-bit codes in the high bits of
  // 16-bit channels; NV12, YUY2 and AYUV hold 8-bit codes.
  inline void D3D11VideoCscCodeBits(DXGI_FORMAT Format, uint32_t& Bits, uint32_t& StorageBits) {
    Bits = 8;
    StorageBits = 8;

    if (Format == DXGI_FORMAT_P010) {
      Bits = 10;
      StorageBits = 16;
    }
  }

  // The space of a view from its legacy colour space and the kind of its format.
  // For YCbCr, Nominal_Range 0-255 is full range, and both 16-235 and undefined
  // are studio range: video is studio range unless it says otherwise. For RGB,
  // RGB_Range 1 is studio range.
  inline D3D11VideoCscSpace D3D11VideoCscSpaceOf(
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE& ColorSpace,
          bool                              IsYCbCr,
          DXGI_FORMAT                       Format = DXGI_FORMAT_UNKNOWN) {
    D3D11VideoCscSpace result;
    result.ycbcr = IsYCbCr;
    if (IsYCbCr)
      D3D11VideoCscCodeBits(Format, result.bits, result.storageBits);
    result.bt709 = ColorSpace.YCbCr_Matrix != 0;
    result.studio = IsYCbCr
      ? ColorSpace.Nominal_Range != D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
      : ColorSpace.RGB_Range != 0;
    return result;
  }

  namespace video_csc {

    // 4x4 affine matrices, row-major: the last row is (0, 0, 0, 1).
    struct Affine {
      double m[4][4];
    };

    inline Affine Identity() {
      Affine a = { };
      for (int i = 0; i < 4; i++)
        a.m[i][i] = 1.0;
      return a;
    }

    inline Affine Multiply(const Affine& A, const Affine& B) {
      Affine r = { };
      for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
          for (int k = 0; k < 4; k++)
            r.m[i][j] += A.m[i][k] * B.m[k][j];
        }
      }
      return r;
    }

    // Shader order (Cr, Y, Cb) to (Y, Cb, Cr), and back.
    inline Affine FromShaderYCbCr() {
      Affine a = { };
      a.m[0][1] = 1.0;  // Y  = g
      a.m[1][2] = 1.0;  // Cb = b
      a.m[2][0] = 1.0;  // Cr = r
      a.m[3][3] = 1.0;
      return a;
    }

    inline Affine ToShaderYCbCr() {
      Affine a = { };
      a.m[0][2] = 1.0;  // r = Cr
      a.m[1][0] = 1.0;  // g = Y
      a.m[2][1] = 1.0;  // b = Cb
      a.m[3][3] = 1.0;
      return a;
    }

    // The code ranges of a YCbCr space: code = toCode * sampled value; Y' = (code - yLo) / ySpan,
    // chroma = (code - centre) / cSpan.
    struct CodeRange {
      double toCode, yLo, ySpan, centre, cSpan;
    };

    inline CodeRange RangeOf(const D3D11VideoCscSpace& S) {
      const double maxStored = double((1ull << S.storageBits) - 1u);
      const double step = double(1ull << (S.storageBits - S.bits));
      const double unit = double(1ull << (S.bits - 8u));
      CodeRange r;
      r.toCode = maxStored / step;
      r.centre = double(1ull << (S.bits - 1u));
      if (S.studio) {
        r.yLo = 16.0 * unit;
        r.ySpan = 219.0 * unit;
        r.cSpan = 224.0 * unit;
      } else {
        r.yLo = 0.0;
        r.ySpan = double((1ull << S.bits) - 1u);
        r.cSpan = r.ySpan;
      }
      return r;
    }

    // Stored (Y, Cb, Cr) to Y' 0..1 and chroma -0.5..0.5.
    inline Affine DecodeYCbCr(const D3D11VideoCscSpace& S) {
      const CodeRange r = RangeOf(S);
      Affine a = Identity();
      a.m[0][0] = r.toCode / r.ySpan; a.m[0][3] = -r.yLo / r.ySpan;
      a.m[1][1] = r.toCode / r.cSpan; a.m[1][3] = -r.centre / r.cSpan;
      a.m[2][2] = r.toCode / r.cSpan; a.m[2][3] = -r.centre / r.cSpan;
      return a;
    }

    inline Affine EncodeYCbCr(const D3D11VideoCscSpace& S) {
      const CodeRange r = RangeOf(S);
      Affine a = Identity();
      a.m[0][0] = r.ySpan / r.toCode; a.m[0][3] = r.yLo / r.toCode;
      a.m[1][1] = r.cSpan / r.toCode; a.m[1][3] = r.centre / r.toCode;
      a.m[2][2] = r.cSpan / r.toCode; a.m[2][3] = r.centre / r.toCode;
      return a;
    }

    // Stored R, G, B to 0..1, and back.
    inline Affine DecodeRgb(bool Studio) {
      Affine a = Identity();
      if (Studio) {
        for (int i = 0; i < 3; i++) {
          a.m[i][i] = 255.0 / 219.0;
          a.m[i][3] = -16.0 / 219.0;
        }
      }
      return a;
    }

    inline Affine EncodeRgb(bool Studio) {
      Affine a = Identity();
      if (Studio) {
        for (int i = 0; i < 3; i++) {
          a.m[i][i] = 219.0 / 255.0;
          a.m[i][3] = 16.0 / 255.0;
        }
      }
      return a;
    }

    inline void Coefficients(bool Bt709, double& Kr, double& Kb) {
      Kr = Bt709 ? 0.2126 : 0.299;
      Kb = Bt709 ? 0.0722 : 0.114;
    }

    // Y'CbCr (Y' 0..1, chroma -0.5..0.5) to R'G'B' 0..1.
    inline Affine YCbCrToRgb(bool Bt709) {
      double Kr, Kb;
      Coefficients(Bt709, Kr, Kb);
      const double Kg = 1.0 - Kr - Kb;
      Affine a = Identity();
      a.m[0][0] = 1.0; a.m[0][1] = 0.0;                          a.m[0][2] = 2.0 * (1.0 - Kr);
      a.m[1][0] = 1.0; a.m[1][1] = -2.0 * (1.0 - Kb) * Kb / Kg;  a.m[1][2] = -2.0 * (1.0 - Kr) * Kr / Kg;
      a.m[2][0] = 1.0; a.m[2][1] = 2.0 * (1.0 - Kb);             a.m[2][2] = 0.0;
      return a;
    }

    inline Affine RgbToYCbCr(bool Bt709) {
      double Kr, Kb;
      Coefficients(Bt709, Kr, Kb);
      const double Kg = 1.0 - Kr - Kb;
      Affine a = Identity();
      a.m[0][0] = Kr;                         a.m[0][1] = Kg;                          a.m[0][2] = Kb;
      a.m[1][0] = -Kr / (2.0 * (1.0 - Kb));   a.m[1][1] = -Kg / (2.0 * (1.0 - Kb));    a.m[1][2] = 0.5;
      a.m[2][0] = 0.5;                        a.m[2][1] = -Kg / (2.0 * (1.0 - Kr));    a.m[2][2] = -Kb / (2.0 * (1.0 - Kr));
      return a;
    }

    // The whole transform, in the shader's vector order on both sides.
    inline Affine Transform(const D3D11VideoCscSpace& Src, const D3D11VideoCscSpace& Dst) {
      // Stored input to R'G'B' 0..1.
      Affine toRgb = Src.ycbcr
        ? Multiply(YCbCrToRgb(Src.bt709), Multiply(DecodeYCbCr(Src), FromShaderYCbCr()))
        : DecodeRgb(Src.studio);

      // R'G'B' 0..1 to stored output.
      Affine fromRgb = Dst.ycbcr
        ? Multiply(ToShaderYCbCr(), Multiply(EncodeYCbCr(Dst), RgbToYCbCr(Dst.bt709)))
        : EncodeRgb(Dst.studio);

      return Multiply(fromRgb, toRgb);
    }

  }

  // The 3x4 matrix of the blit shader's uniform buffer: row i holds output channel i.
  inline void D3D11VideoCscMatrix(
    const D3D11VideoCscSpace& Src,
    const D3D11VideoCscSpace& Dst,
          float               pMatrix[3][4]) {
    video_csc::Affine a = video_csc::Transform(Src, Dst);

    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 4; j++)
        pMatrix[i][j] = float(a.m[i][j]);
    }
  }

}
