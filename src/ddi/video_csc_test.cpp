// PROVENANCE: DXVK fork D-Ogi/dxvk, MIT.
//
// Host test of the colour transform of the video processor blit (src/d3d11/d3d11_video_csc.h).
// No GPU: the transform is a pure function of the two colour spaces. The test applies the matrix
// the shader gets to stored texels, in the shader's channel order, and compares the result with
// the codes of ITU-R BT.601 and BT.709. Two mutants (601 and 709 exchanged, the chroma range
// left out) must each fail at least one check, and the old range reading must differ from the new.
// P010: the vpblt bars at 10-bit codes, sampled as R16_UNORM (code << 6) / 65535 as the shader
// sees them, against vpblt's reference; the 8-bit reading of the same sample must reproduce the
// lab value of kit-vddi2 (BD-223: p010-709s-bgra bar 2 B 189 for 191.2).
// Prints "PASSED: 0 failure(s)" or "FAILED: <n> failure(s)" and returns 0 or 1.

#include <cmath>
#include <cstdio>

#include "../d3d11/d3d11_video_csc.h"

using dxvk::D3D11VideoCscSpace;

static int g_failures = 0;

static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) g_failures++;
}

struct Texel { double r, g, b; };

// 8-bit codes as the shader sees them: a YCbCr texel is (Cr, Y, Cb), an RGB texel (R, G, B).
static Texel YCbCr(double y, double cb, double cr) { return { cr / 255.0, y / 255.0, cb / 255.0 }; }
static Texel Rgb(double r, double g, double b) { return { r / 255.0, g / 255.0, b / 255.0 }; }

static Texel Apply(const float m[3][4], const Texel& t) {
  const double in[4] = { t.r, t.g, t.b, 1.0 };
  double out[3];
  for (int i = 0; i < 3; i++) {
    out[i] = 0.0;
    for (int j = 0; j < 4; j++)
      out[i] += double(m[i][j]) * in[j];
  }
  return { out[0], out[1], out[2] };
}

// Within half an 8-bit code.
static bool Near(const Texel& a, const Texel& b) {
  const double tol = 0.5 / 255.0;
  return std::fabs(a.r - b.r) <= tol && std::fabs(a.g - b.g) <= tol && std::fabs(a.b - b.b) <= tol;
}

static D3D11VideoCscSpace Space(bool ycbcr, bool bt709, bool studio) {
  D3D11VideoCscSpace s;
  s.ycbcr = ycbcr; s.bt709 = bt709; s.studio = studio;
  return s;
}

using Transform = void (*)(const D3D11VideoCscSpace&, const D3D11VideoCscSpace&, float[3][4]);

static bool Maps(Transform f, const D3D11VideoCscSpace& src, const D3D11VideoCscSpace& dst,
                 const Texel& in, const Texel& want) {
  float m[3][4];
  f(src, dst, m);
  return Near(Apply(m, in), want);
}

// The reference codes. BT.709 red (R'=1, G'=B'=0): Y'=0.2126, Cb=-0.1146, Cr=0.5;
// studio: Y 16+219*0.2126=62.56, Cb 128-224*0.1146=102.33, Cr 240.
// BT.601 red: Y'=0.299, Cb=-0.1687, Cr=0.5; studio: Y 81.48, Cb 90.21, Cr 240.
static int RunChecks(Transform f, bool print) {
  const int before = g_failures;
  auto check = [&] (bool ok, const char* what) {
    if (print) Check(ok, what);
    else if (!ok) g_failures++;
  };
  const auto ycc709s = Space(true, true, true), ycc709f = Space(true, true, false);
  const auto ycc601s = Space(true, false, true);
  const auto rgbFull = Space(false, false, false), rgbStudio = Space(false, false, true);

  check(Maps(f, ycc709s, rgbFull, YCbCr(235, 128, 128), Rgb(255, 255, 255)), "709 studio white -> RGB 255");
  check(Maps(f, ycc709s, rgbFull, YCbCr(16, 128, 128), Rgb(0, 0, 0)), "709 studio black -> RGB 0");
  check(Maps(f, ycc709s, rgbFull, YCbCr(62.56, 102.33, 240), Rgb(255, 0, 0)), "709 studio red -> RGB red");
  check(Maps(f, ycc601s, rgbFull, YCbCr(81.48, 90.21, 240), Rgb(255, 0, 0)), "601 studio red -> RGB red");
  check(Maps(f, ycc709f, rgbFull, YCbCr(255, 128, 128), Rgb(255, 255, 255)), "709 full white -> RGB 255");
  // Half red (R'=0.5): Y'=0.1063, Cb=-0.0573, Cr=0.25; full-range codes 27.11, 113.39, 191.75.
  check(Maps(f, ycc709f, rgbFull, YCbCr(27.11, 113.39, 191.75), Rgb(127.5, 0, 0)), "709 full half red -> RGB half red");
  check(Maps(f, ycc709s, rgbStudio, YCbCr(235, 128, 128), Rgb(235, 235, 235)), "709 studio white -> RGB 235");
  check(Maps(f, ycc709s, rgbStudio, YCbCr(16, 128, 128), Rgb(16, 16, 16)), "709 studio black -> RGB 16");
  check(Maps(f, rgbFull, ycc709s, Rgb(255, 0, 0), YCbCr(62.56, 102.33, 240)), "RGB red -> 709 studio");
  check(Maps(f, rgbFull, ycc709s, Rgb(255, 255, 255), YCbCr(235, 128, 128)), "RGB white -> 709 studio");
  check(Maps(f, rgbFull, ycc601s, Rgb(255, 0, 0), YCbCr(81.48, 90.21, 240)), "RGB red -> 601 studio");
  check(Maps(f, rgbStudio, rgbFull, Rgb(235, 16, 125.5), Rgb(255, 0, 127.5)), "RGB studio -> RGB full");
  check(Maps(f, rgbFull, rgbStudio, Rgb(255, 0, 127.5), Rgb(235, 16, 125.5)), "RGB full -> RGB studio");
  check(Maps(f, ycc709s, ycc709s, YCbCr(62.56, 102.33, 240), YCbCr(62.56, 102.33, 240)), "709 studio -> 709 studio is identity");
  check(Maps(f, ycc601s, ycc709s, YCbCr(81.48, 90.21, 240), YCbCr(62.56, 102.33, 240)), "601 red -> 709 red");
  check(Maps(f, ycc709f, ycc709s, YCbCr(255, 128, 128), YCbCr(235, 128, 128)), "709 full white -> 709 studio white");
  // Full-range chroma 0 and 255 sit 128 and 127 codes from the centre: studio 15.56 and 239.56.
  check(Maps(f, ycc709f, ycc709s, YCbCr(128, 0, 255), YCbCr(128.0 * 219 / 255 + 16, 128.0 - 128.0 * 224 / 255, 128.0 + 127.0 * 224 / 255)),
        "709 full chroma extremes -> studio 16 and 240");
  // A round trip through NV12 returns the colour.
  {
    float a[3][4], b[3][4];
    f(rgbFull, ycc709s, a);
    f(ycc709s, rgbFull, b);
    const Texel in = Rgb(40, 200, 90);
    check(Near(Apply(b, Apply(a, in)), in), "RGB -> 709 studio -> RGB");
  }
  return g_failures - before;
}

// The mutants.
static void Swapped(const D3D11VideoCscSpace& s, const D3D11VideoCscSpace& d, float m[3][4]) {
  auto s2 = s, d2 = d;
  s2.bt709 = !s2.bt709; d2.bt709 = !d2.bt709;
  dxvk::D3D11VideoCscMatrix(s2, d2, m);
}

static void NoChromaRange(const D3D11VideoCscSpace& s, const D3D11VideoCscSpace& d, float m[3][4]) {
  dxvk::D3D11VideoCscMatrix(s, d, m);
  if (!s.ycbcr || !s.studio) return;
  // Undo the chroma scale of the decode: columns r and b (Cr, Cb) times 224/255, as a blit
  // that expands Y only would compute.
  for (int i = 0; i < 3; i++) {
    const double k = 224.0 / 255.0;
    m[i][3] += float((m[i][0] + m[i][2]) * (128.0 / 255.0) * (1.0 - k));
    m[i][0] = float(m[i][0] * k);
    m[i][2] = float(m[i][2] * k);
  }
}

// P010: 10-bit codes in the high bits of 16-bit UNORM channels, in the shader's order (Cr, Y, Cb).
static Texel YCbCr10(double y, double cb, double cr) {
  auto s = [] (double code) { return double(unsigned(code) << 6) / 65535.0; };
  return { s(cr), s(y), s(cb) };
}

static double Round8(double v) {
  v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
  return std::floor(v * 255.0 + 0.5);
}

// The vpblt bars (tools/win/vpblt/vpblt.cpp kBarColours) and their 10-bit studio BT.709 codes as
// vpblt encodes them, with vpblt's reference: the codes decoded exactly as 10-bit codes.
static void P010Checks() {
  struct Bar { double r, g, b; };
  const Bar bars[8] = { {1, 1, 1}, {.75, .75, 0}, {0, .75, .75}, {0, .75, 0}, {.75, 0, .75}, {1, 0, 0}, {0, 0, .75}, {0, 0, 0} };
  const double kr = 0.2126, kb = 0.0722, kg = 1.0 - kr - kb;
  auto code = [] (double v) { v = std::floor(v + 0.5); return v < 0 ? 0.0 : (v > 1023 ? 1023.0 : v); };

  D3D11VideoCscSpace p010 = Space(true, true, true);
  p010.bits = 10; p010.storageBits = 16;
  const auto rgbFull = Space(false, false, false);
  float fixedM[3][4], oldM[3][4];
  dxvk::D3D11VideoCscMatrix(p010, rgbFull, fixedM);
  dxvk::D3D11VideoCscMatrix(Space(true, true, true), rgbFull, oldM);

  double worst = 0.0, oldBar2B = -1.0, fixedBar2B = -1.0;
  for (int i = 0; i < 8; i++) {
    const double y = kr * bars[i].r + kg * bars[i].g + kb * bars[i].b;
    const double cb = (bars[i].b - y) / (2.0 * (1.0 - kb)), cr = (bars[i].r - y) / (2.0 * (1.0 - kr));
    const double yc = code((16.0 + 219.0 * y) * 4.0), cbc = code(512.0 + 896.0 * cb), crc = code(512.0 + 896.0 * cr);
    // vpblt's reference: the stored codes decoded as 10-bit codes.
    const double y2 = (yc / 4.0 - 16.0) / 219.0, cb2 = (cbc - 512.0) / 896.0, cr2 = (crc - 512.0) / 896.0;
    const double wr = y2 + 2.0 * (1.0 - kr) * cr2, wb = y2 + 2.0 * (1.0 - kb) * cb2, wg = (y2 - kr * wr - kb * wb) / kg;
    const Texel in = YCbCr10(yc, cbc, crc);
    const Texel got = Apply(fixedM, in), old = Apply(oldM, in);
    const double want[3] = { wr, wg, wb }, gotv[3] = { got.r, got.g, got.b };
    for (int c = 0; c < 3; c++) {
      const double w = (want[c] < 0 ? 0 : (want[c] > 1 ? 1 : want[c])) * 255.0;
      const double e = std::fabs(Round8(gotv[c]) - w);
      if (e > worst) worst = e;
    }
    if (i == 2) { oldBar2B = Round8(old.b); fixedBar2B = Round8(got.b); }
  }
  char what[160];
  std::snprintf(what, sizeof(what), "P010 709 studio bars -> RGB within 0.5 codes of vpblt's reference (max %.2f)", worst);
  Check(worst <= 0.5 + 1e-9, what);
  std::snprintf(what, sizeof(what), "P010 bar 2 B is 191 (got %.0f)", fixedBar2B);
  Check(fixedBar2B == 191.0, what);
  std::snprintf(what, sizeof(what), "control: the 8-bit reading of the P010 sample gives the lab value 189 (got %.0f)", oldBar2B);
  Check(oldBar2B == 189.0, what);

  // RGB -> P010: BT.709 red is codes 250, 409, 960 (studio); the stored value of each channel is
  // within half a 10-bit code of code << 6.
  float enc[3][4];
  dxvk::D3D11VideoCscMatrix(rgbFull, p010, enc);
  const Texel red = Apply(enc, Rgb(255, 0, 0)), want = YCbCr10(250, 409, 960);
  const double half = 0.5 * 64.0 / 65535.0;
  Check(std::fabs(red.r - want.r) <= half && std::fabs(red.g - want.g) <= half && std::fabs(red.b - want.b) <= half,
        "RGB red -> P010 709 studio codes 250, 409, 960");
  float dec[3][4];
  dxvk::D3D11VideoCscMatrix(p010, rgbFull, dec);
  const Texel in = Rgb(40, 200, 90);
  Check(Near(Apply(dec, Apply(enc, in)), in), "RGB -> P010 709 studio -> RGB");
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE c = { };
  c.YCbCr_Matrix = 1;
  const auto s = dxvk::D3D11VideoCscSpaceOf(c, true, DXGI_FORMAT_P010);
  Check(s.bits == 10 && s.storageBits == 16, "P010 views decode 10-bit codes in 16-bit channels");
  const auto n = dxvk::D3D11VideoCscSpaceOf(c, true, DXGI_FORMAT_NV12);
  Check(n.bits == 8 && n.storageBits == 8, "NV12 views decode 8-bit codes");
}

int main() {
  RunChecks(dxvk::D3D11VideoCscMatrix, true);
  P010Checks();
  // Legacy colour space words.
  {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE c = { };
    c.YCbCr_Matrix = 1; c.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_UNDEFINED;
    auto s = dxvk::D3D11VideoCscSpaceOf(c, true);
    Check(s.ycbcr && s.bt709 && s.studio, "YCbCr undefined range is studio range");
    c.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    s = dxvk::D3D11VideoCscSpaceOf(c, true);
    Check(s.ycbcr && !s.studio, "YCbCr 0-255 is full range");
    c.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    Check(dxvk::D3D11VideoCscSpaceOf(c, true).studio, "YCbCr 16-235 is studio range");
    c.RGB_Range = 1;
    Check(dxvk::D3D11VideoCscSpaceOf(c, false).studio && !dxvk::D3D11VideoCscSpaceOf(c, false).ycbcr, "RGB_Range 1 is studio RGB");
    c.RGB_Range = 0;
    Check(!dxvk::D3D11VideoCscSpaceOf(c, false).studio, "RGB_Range 0 is full RGB");
    // Negative control: the old reading (Nominal_Range nonzero = studio) gets 0-255 wrong.
    c.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    Check((c.Nominal_Range != 0) != dxvk::D3D11VideoCscSpaceOf(c, true).studio, "control: nonzero range is not studio range");
  }

  // Each mutant must fail at least one check.
  {
    const int saved = g_failures;
    const int swapped = RunChecks(Swapped, false);
    const int chroma = RunChecks(NoChromaRange, false);
    g_failures = saved;
    Check(swapped > 0, "control: 601 and 709 exchanged fails");
    Check(chroma > 0, "control: chroma range left out fails");
  }

  if (g_failures)
    std::printf("FAILED: %d failure(s)\n", g_failures);
  else
    std::printf("PASSED: 0 failure(s)\n");
  return g_failures ? 1 : 0;
}
