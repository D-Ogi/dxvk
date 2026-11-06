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
// (Kr 0.2126, Kb 0.0722). Codes are normalized as 8-bit codes: Y 16..235, chroma
// 16..240 with its centre at 128. A 10-bit or 16-bit format stores the same
// fractions of its range, to within one code of the 10-bit range.

#include <d3d11.h>

namespace dxvk {

  struct D3D11VideoCscSpace {
    bool ycbcr  = false;  // the data is YCbCr
    bool bt709  = false;  // the YCbCr matrix is BT.709 (else BT.601)
    bool studio = false;  // limited range: Y and RGB 16..235, chroma 16..240
  };

  // The space of a view from its legacy colour space and the kind of its format.
  // For YCbCr, Nominal_Range 0-255 is full range, and both 16-235 and undefined
  // are studio range: video is studio range unless it says otherwise. For RGB,
  // RGB_Range 1 is studio range.
  inline D3D11VideoCscSpace D3D11VideoCscSpaceOf(
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE& ColorSpace,
          bool                              IsYCbCr) {
    D3D11VideoCscSpace result;
    result.ycbcr = IsYCbCr;
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

    // Stored (Y, Cb, Cr) to Y' 0..1 and chroma -0.5..0.5.
    inline Affine DecodeYCbCr(bool Studio) {
      Affine a = Identity();
      const double ys = Studio ? 255.0 / 219.0 : 1.0;
      const double cs = Studio ? 255.0 / 224.0 : 1.0;
      const double yo = Studio ? 16.0 / 255.0 : 0.0;
      const double co = 128.0 / 255.0;
      a.m[0][0] = ys; a.m[0][3] = -yo * ys;
      a.m[1][1] = cs; a.m[1][3] = -co * cs;
      a.m[2][2] = cs; a.m[2][3] = -co * cs;
      return a;
    }

    inline Affine EncodeYCbCr(bool Studio) {
      Affine a = Identity();
      const double ys = Studio ? 219.0 / 255.0 : 1.0;
      const double cs = Studio ? 224.0 / 255.0 : 1.0;
      const double yo = Studio ? 16.0 / 255.0 : 0.0;
      const double co = 128.0 / 255.0;
      a.m[0][0] = ys; a.m[0][3] = yo;
      a.m[1][1] = cs; a.m[1][3] = co;
      a.m[2][2] = cs; a.m[2][3] = co;
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
        ? Multiply(YCbCrToRgb(Src.bt709), Multiply(DecodeYCbCr(Src.studio), FromShaderYCbCr()))
        : DecodeRgb(Src.studio);

      // R'G'B' 0..1 to stored output.
      Affine fromRgb = Dst.ycbcr
        ? Multiply(ToShaderYCbCr(), Multiply(EncodeYCbCr(Dst.studio), RgbToYCbCr(Dst.bt709)))
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
