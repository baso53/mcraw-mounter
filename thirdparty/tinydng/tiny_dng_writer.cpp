#include "tiny_dng_writer.h"

//
// TIFF format resources.
//
// http://c0de517e.blogspot.jp/2013/07/tiny-hdr-writer.html
// http://paulbourke.net/dataformats/tiff/ and
// http://partners.adobe.com/public/developer/en/tiff/TIFF6.pdf
//

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <swift/bridging>

namespace tinydngwriter {

#ifdef __clang__
#pragma clang diagnostic push
#if __has_warning("-Wzero-as-null-pointer-constant")
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif
#endif

//
// TinyDNGWriter stores IFD table in the end of file so that offset to
// image data can be easily computed.
//
// +----------------------+
// |    header            |
// +----------------------+
// |                      |
// |  image & meta 0      |
// |                      |
// +----------------------+
// |                      |
// |  image & meta 1      |
// |                      |
// +----------------------+
//    ...
// +----------------------+
// |                      |
// |  image & meta N      |
// |                      |
// +----------------------+
// |                      |
// |  IFD 0               |
// |                      |
// +----------------------+
// |                      |
// |  IFD 1               |
// |                      |
// +----------------------+
//    ...
// +----------------------+
// |                      |
// |  IFD 2               |
// |                      |
// +----------------------+
//

// From tiff.h
typedef enum {
  TIFF_NOTYPE = 0,     /* placeholder */
  TIFF_BYTE = 1,       /* 8-bit unsigned integer */
  TIFF_ASCII = 2,      /* 8-bit bytes w/ last byte null */
  TIFF_SHORT = 3,      /* 16-bit unsigned integer */
  TIFF_LONG = 4,       /* 32-bit unsigned integer */
  TIFF_RATIONAL = 5,   /* 64-bit unsigned fraction */
  TIFF_SBYTE = 6,      /* !8-bit signed integer */
  TIFF_UNDEFINED = 7,  /* !8-bit untyped data */
  TIFF_SSHORT = 8,     /* !16-bit signed integer */
  TIFF_SLONG = 9,      /* !32-bit signed integer */
  TIFF_SRATIONAL = 10, /* !64-bit signed fraction */
  TIFF_FLOAT = 11,     /* !32-bit IEEE floating point */
  TIFF_DOUBLE = 12,    /* !64-bit IEEE floating point */
  TIFF_IFD = 13,       /* %32-bit unsigned integer (offset) */
  TIFF_LONG8 = 16,     /* BigTIFF 64-bit unsigned integer */
  TIFF_SLONG8 = 17,    /* BigTIFF 64-bit signed integer */
  TIFF_IFD8 = 18       /* BigTIFF 64-bit unsigned integer (offset) */
} DataType;

const static int kHeaderSize = 8;  // TIFF header size.

// floating point to integer rational value conversion
// https://stackoverflow.com/questions/51142275/exact-value-of-a-floating-point-number-as-a-rational
//
// Return error flag
static int FloatToRational(float x, float *numerator, float *denominator) {
  if (!std::isfinite(x)) {
    *numerator = *denominator = 0.0f;
    if (x > 0.0f) *numerator = 1.0f;
    if (x < 0.0f) *numerator = -1.0f;
    return 1;
  }

  // TIFF Rational use two uint32's, so reduce the bits
  int bdigits = FLT_MANT_DIG;
  int expo;
  *denominator = 1.0f;
  *numerator = std::frexp(x, &expo) * std::pow(2.0f, bdigits);
  expo -= bdigits;
  if (expo > 0) {
    *numerator *= std::pow(2.0f, expo);
  } else if (expo < 0) {
    expo = -expo;
    if (expo >= FLT_MAX_EXP - 1) {
      *numerator /= std::pow(2.0f, expo - (FLT_MAX_EXP - 1));
      *denominator *= std::pow(2.0f, FLT_MAX_EXP - 1);
      return fabs(*numerator) < 1.0f;
    } else {
      *denominator *= std::pow(2.0f, expo);
    }
  }

  while ((std::fabs(*numerator) > 0.0f) &&
         (std::fabs(std::fmod(*numerator, 2)) <
          std::numeric_limits<float>::epsilon()) &&
         (std::fabs(std::fmod(*denominator, 2)) <
          std::numeric_limits<float>::epsilon())) {
    *numerator /= 2.0f;
    *denominator /= 2.0f;
  }
  return 0;
}

static inline bool IsBigEndian() {
  unsigned int i = 0x01020304;
  char c[4];
  memcpy(c, &i, 4);
  return (c[0] == 1);
}

static void swap2(unsigned short *val) {
  unsigned short tmp = *val;
  unsigned char *dst = reinterpret_cast<unsigned char *>(val);
  unsigned char *src = reinterpret_cast<unsigned char *>(&tmp);

  dst[0] = src[1];
  dst[1] = src[0];
}

static void swap4(int *val) {
  unsigned int tmp = *val;
  unsigned char *dst = reinterpret_cast<unsigned char *>(val);
  unsigned char *src = reinterpret_cast<unsigned char *>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static void swap4(unsigned int *val) {
  unsigned int tmp = *val;
  unsigned char *dst = reinterpret_cast<unsigned char *>(val);
  unsigned char *src = reinterpret_cast<unsigned char *>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static void swap8(uint64_t *val) {
  uint64_t tmp = *val;
  unsigned char *dst = reinterpret_cast<unsigned char *>(val);
  unsigned char *src = reinterpret_cast<unsigned char *>(&tmp);

  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];
}

static void Write1(const unsigned char c, std::ostringstream *out) {
  unsigned char value = c;
  out->write(reinterpret_cast<const char *>(&value), 1);
}

static void Write2(const unsigned short c, std::ostringstream *out,
                   const bool swap_endian) {
  unsigned short value = c;
  if (swap_endian) {
    swap2(&value);
  }

  out->write(reinterpret_cast<const char *>(&value), 2);
}

static void Write4(const unsigned int c, std::ostringstream *out,
                   const bool swap_endian) {
  unsigned int value = c;
  if (swap_endian) {
    swap4(&value);
  }

  out->write(reinterpret_cast<const char *>(&value), 4);
}

static bool WriteTIFFTag(const unsigned short tag, const unsigned short type,
                         const unsigned int count, const unsigned char *data,
                         std::vector<IFDTag> *tags_out,
                         std::ostringstream *data_out) {
  assert(sizeof(IFDTag) ==
         12);  // FIXME(syoyo): Use static_assert for C++11 compiler

  IFDTag ifd;
  ifd.tag = tag;
  ifd.type = type;
  ifd.count = count;

  size_t typesize_table[] = {1, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};

  size_t len = count * (typesize_table[(type) < 14 ? (type) : 0]);
  if (len > 4) {
    assert(data_out);
    if (!data_out) {
      return false;
    }

    // Store offset value.

    unsigned int offset =
        static_cast<unsigned int>(data_out->tellp()) + kHeaderSize;
    ifd.offset_or_value = offset;

    data_out->write(reinterpret_cast<const char *>(data),
                    static_cast<std::streamsize>(len));

  } else {
    ifd.offset_or_value = 0;

    // less than 4 bytes = store data itself.
    if (len == 1) {
      unsigned char value = *(data);
      memcpy(&(ifd.offset_or_value), &value, sizeof(unsigned char));
    } else if (len == 2) {
      unsigned short value = *(reinterpret_cast<const unsigned short *>(data));
      memcpy(&(ifd.offset_or_value), &value, sizeof(unsigned short));
    } else if (len == 4) {
      unsigned int value = *(reinterpret_cast<const unsigned int *>(data));
      ifd.offset_or_value = value;
    } else {
      assert(0);
    }
  }

  tags_out->push_back(ifd);

  return true;
}

static bool WriteTIFFVersionHeader(std::ostringstream *out, bool big_endian) {
  // TODO(syoyo): Support BigTIFF?

  // 4d 4d = Big endian. 49 49 = Little endian.
  if (big_endian) {
    Write1(0x4d, out);
    Write1(0x4d, out);
    Write1(0x0, out);
    Write1(0x2a, out);  // Tiff version ID
  } else {
    Write1(0x49, out);
    Write1(0x49, out);
    Write1(0x2a, out);  // Tiff version ID
    Write1(0x0, out);
  }

  return true;
}

DNGImage::DNGImage()
    : dng_big_endian_(true),
      num_fields_(0),
      samples_per_pixels_(0),
      data_strip_offset_{0},
      data_strip_bytes_{0} {
  swap_endian_ = (IsBigEndian() != dng_big_endian_);
}

void DNGImage::SetBigEndian(bool big_endian) {
  dng_big_endian_ = big_endian;
  swap_endian_ = (IsBigEndian() != dng_big_endian_);
}

bool DNGImage::SetSubfileType(bool reduced_image, bool page, bool mask) {
  unsigned int count = 1;

  unsigned int bits = 0;
  if (reduced_image) {
    bits |= FILETYPE_REDUCEDIMAGE;
  }
  if (page) {
    bits |= FILETYPE_PAGE;
  }
  if (mask) {
    bits |= FILETYPE_MASK;
  }

  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_SUB_FILETYPE), TIFF_LONG, count,
      reinterpret_cast<const unsigned char *>(&bits), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetImageWidth(const unsigned int width) {
  unsigned int count = 1;

  unsigned int data = width;
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_IMAGE_WIDTH), TIFF_LONG, count,
      reinterpret_cast<const unsigned char *>(&data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetImageLength(const unsigned int length) {
  unsigned int count = 1;

  const unsigned int data = length;
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_IMAGE_LENGTH), TIFF_LONG, count,
      reinterpret_cast<const unsigned char *>(&data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetRowsPerStrip(const unsigned int rows) {
  if (rows == 0) {
    return false;
  }

  unsigned int count = 1;

  const unsigned int data = rows;
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_ROWS_PER_STRIP), TIFF_LONG, count,
      reinterpret_cast<const unsigned char *>(&data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetSamplesPerPixel(const unsigned short value) {
  if (value > 4) {
    {
      std::stringstream ss;
      ss << "Samples per pixel must be less than or equal to 4, but got " << value << ".\n";
      err_ += ss.str();
    }
    return false;
  }

  unsigned int count = 1;

  const unsigned short data = value;
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_SAMPLES_PER_PIXEL), TIFF_SHORT, count,
      reinterpret_cast<const unsigned char *>(&data), &ifd_tags_, &data_os_);

  if (!ret) {
    err_ += "Failed to write `TIFFTAG_SAMPLES_PER_PIXEL` tag.\n";
    return false;
  }

  samples_per_pixels_ = value;  // Store SPP for later use.

  num_fields_++;
  return true;
}

bool DNGImage::SetBitsPerSample() {
  // `SetSamplesPerPixel()` must be called in advance and SPP shoud be equal to
  // `num_samples`.
    const unsigned int num_samples = 1;
    const short values[1] = {16};
    

  if (samples_per_pixels_ == 0) {
    err_ += "SetSamplesPerPixel() must be called before SetBitsPerSample().\n";
    return false;
  }

  if ((num_samples == 0) || (num_samples > 4)) {
    std::stringstream ss;
    ss << "Invalid number of samples: " << num_samples << "\n";
    err_ += ss.str();
    return false;
  } else if (num_samples != samples_per_pixels_) {
    std::stringstream ss;
    ss << "Samples per pixel mismatch. " << num_samples << " is given for SetBitsPerSample(), but SamplesPerPixel is set to " << samples_per_pixels_ << "\n";
    err_ += ss.str();
    return false;
  } else {
    // ok
  }

  unsigned short bps = values[0];

  std::vector<unsigned short> vs(num_samples);
  for (size_t i = 0; i < vs.size(); i++) {
    // FIXME(syoyo): Currently bps must be same for all samples
    if (bps != values[i]) {
      err_ += "BitsPerSample must be same among samples at the moment.\n";
      return false;
    }

    vs[i] = values[i];

    // TODO(syoyo): Swap values when writing IFD tag, not here.
    if (swap_endian_) {
      swap2(&vs[i]);
    }
  }

  unsigned int count = num_samples;

  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_BITS_PER_SAMPLE),
                          TIFF_SHORT, count,
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  // Store BPS for later use.
  bits_per_samples_.resize(num_samples);
  for (size_t i = 0; i < num_samples; i++) {
    bits_per_samples_[i] = values[i];
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetPhotometric(const unsigned short value) {
  if ((value == PHOTOMETRIC_LINEARRAW) ||
      (value == PHOTOMETRIC_CFA) ||
      (value == PHOTOMETRIC_RGB) ||
      (value == PHOTOMETRIC_WHITE_IS_ZERO) ||
      (value == PHOTOMETRIC_BLACK_IS_ZERO)) {
    // OK
  } else {
    return false;
  }

  unsigned int count = 1;

  const unsigned short data = value;
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_PHOTOMETRIC), TIFF_SHORT, count,
      reinterpret_cast<const unsigned char *>(&data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetPlanarConfig(const unsigned short value) {
  unsigned int count = 1;

  if ((value == PLANARCONFIG_CONTIG) || (value == PLANARCONFIG_SEPARATE)) {
    // OK
  } else {
    return false;
  }

  const unsigned short data = value;
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_PLANAR_CONFIG), TIFF_SHORT, count,
      reinterpret_cast<const unsigned char *>(&data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetCompression(const unsigned short value) {
  unsigned int count = 1;

  if ((value == COMPRESSION_NONE)) {
    // OK
  } else {
    return false;
  }

  const unsigned short data = value;
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_COMPRESSION), TIFF_SHORT, count,
      reinterpret_cast<const unsigned char *>(&data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetSampleFormat(const unsigned int num_samples,
                               const unsigned short *values) {
  // `SetSamplesPerPixel()` must be called in advance
  if ((num_samples > 0) && (num_samples == samples_per_pixels_)) {
    // OK
  } else {
    err_ += "SetSamplesPerPixel() must be called before SetSampleFormat().\n";
    return false;
  }

  unsigned short format = values[0];

  std::vector<unsigned short> vs(num_samples);
  for (size_t i = 0; i < vs.size(); i++) {
    // FIXME(syoyo): Currently format must be same for all samples
    if (format != values[i]) {
      err_ += "SampleFormat must be same among samples at the moment.\n";
      return false;
    }

    if ((format == SAMPLEFORMAT_UINT) || (format == SAMPLEFORMAT_INT) ||
        (format == SAMPLEFORMAT_IEEEFP)) {
      // OK
    } else {
      err_ += "Invalid format value specified for SetSampleFormat().\n";
      return false;
    }

    vs[i] = values[i];

    // TODO(syoyo): Swap values when writing IFD tag, not here.
    if (swap_endian_) {
      swap2(&vs[i]);
    }
  }

  unsigned int count = num_samples;

  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_SAMPLEFORMAT),
                          TIFF_SHORT, count,
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetOrientation(const unsigned short value) {
  unsigned int count = 1;

  if ((value == ORIENTATION_TOPLEFT) || (value == ORIENTATION_TOPRIGHT) ||
      (value == ORIENTATION_BOTRIGHT) || (value == ORIENTATION_BOTLEFT) ||
      (value == ORIENTATION_LEFTTOP) || (value == ORIENTATION_RIGHTTOP) ||
      (value == ORIENTATION_RIGHTBOT) || (value == ORIENTATION_LEFTBOT)) {
    // OK
  } else {
    return false;
  }

  const unsigned int data = value;
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_ORIENTATION), TIFF_SHORT, count,
      reinterpret_cast<const unsigned char *>(&data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetBlackLevel(const unsigned int num_components,
                             const unsigned short *values) {
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_BLACK_LEVEL), TIFF_SHORT, num_components,
      reinterpret_cast<const unsigned char *>(values), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetBlackLevelRational(unsigned int num_samples,
                                     const float *values) {
  // `SetSamplesPerPixel()` must be called in advance and SPP shoud be equal to
  // `num_samples`.
  if ((num_samples > 0) && (num_samples == samples_per_pixels_)) {
    // OK
  } else {
    return false;
  }

  std::vector<unsigned int> vs(num_samples * 2);
  for (size_t i = 0; i * 2 < vs.size(); i++) {
    float numerator, denominator;
    if (FloatToRational(values[i], &numerator, &denominator) != 0) {
      // Couldn't represent fp value as integer rational value.
      return false;
    }

    vs[2 * i + 0] = static_cast<unsigned int>(numerator);
    vs[2 * i + 1] = static_cast<unsigned int>(denominator);

    // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
    if (swap_endian_) {
      swap4(&vs[2 * i + 0]);
      swap4(&vs[2 * i + 1]);
    }
  }

  unsigned int count = num_samples;

  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_BLACK_LEVEL),
                          TIFF_RATIONAL, count,
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetWhiteLevel(const short value) {
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_WHITE_LEVEL), TIFF_SHORT, 1,
      reinterpret_cast<const unsigned char *>(&value),
      &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetWhiteLevelRational(unsigned int num_samples,
                                     const float *values) {
  // `SetSamplesPerPixel()` must be called in advance and SPP shoud be equal to
  // `num_samples`.
  if ((num_samples > 0) && (num_samples == samples_per_pixels_)) {
    // OK
  } else {
    return false;
  }

  std::vector<unsigned int> vs(num_samples * 2);
  for (size_t i = 0; i * 2 < vs.size(); i++) {
    float numerator, denominator;
    if (FloatToRational(values[i], &numerator, &denominator) != 0) {
      // Couldn't represent fp value as integer rational value.
      return false;
    }

    vs[2 * i + 0] = static_cast<unsigned int>(numerator);
    vs[2 * i + 1] = static_cast<unsigned int>(denominator);

    // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
    if (swap_endian_) {
      swap4(&vs[2 * i + 0]);
      swap4(&vs[2 * i + 1]);
    }
  }

  unsigned int count = num_samples;

  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_WHITE_LEVEL),
                          TIFF_RATIONAL, count,
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetXResolution(const float value) {
  float numerator, denominator;
  if (FloatToRational(value, &numerator, &denominator) != 0) {
    // Couldn't represent fp value as integer rational value.
    return false;
  }

  unsigned int data[2];
  data[0] = static_cast<unsigned int>(numerator);
  data[1] = static_cast<unsigned int>(denominator);

  // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
  if (swap_endian_) {
    swap4(&data[0]);
    swap4(&data[1]);
  }

  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_XRESOLUTION), TIFF_RATIONAL, 1,
      reinterpret_cast<const unsigned char *>(data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetYResolution(const float value) {
  float numerator, denominator;
  if (FloatToRational(value, &numerator, &denominator) != 0) {
    // Couldn't represent fp value as integer rational value.
    return false;
  }

  unsigned int data[2];
  data[0] = static_cast<unsigned int>(numerator);
  data[1] = static_cast<unsigned int>(denominator);

  // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
  if (swap_endian_) {
    swap4(&data[0]);
    swap4(&data[1]);
  }

  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_YRESOLUTION), TIFF_RATIONAL, 1,
      reinterpret_cast<const unsigned char *>(data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetResolutionUnit(const unsigned short value) {
  unsigned int count = 1;

  if ((value == RESUNIT_NONE) || (value == RESUNIT_INCH) ||
      (value == RESUNIT_CENTIMETER)) {
    // OK
  } else {
    return false;
  }

  const unsigned short data = value;
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_RESOLUTION_UNIT), TIFF_SHORT, count,
      reinterpret_cast<const unsigned char *>(&data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetFrameRate(float value) {
  float numerator, denominator;
  if (FloatToRational(value, &numerator, &denominator) != 0) {
    // Couldn't represent fp value as integer rational value.
    return false;
  }

  unsigned int data[2];
  data[0] = static_cast<unsigned int>(numerator);
  data[1] = static_cast<unsigned int>(denominator);

  bool ret = WriteTIFFTag(
     static_cast<unsigned short>(TIFFTAG_FPS), TIFF_RATIONAL, 1,
      reinterpret_cast<const unsigned char *>(data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetTimeCode(unsigned char timecode[8]) {
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_TIMECODE), TIFF_BYTE, 8,
      reinterpret_cast<const unsigned char *>(timecode),
      &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetExposureTime(float exposureSecs) {
  float numerator, denominator;
  if (FloatToRational(exposureSecs, &numerator, &denominator) != 0) {
    // Couldn't represent fp value as integer rational value.
    return false;
  }

  unsigned int data[2];
  data[0] = static_cast<unsigned int>(numerator);
  data[1] = static_cast<unsigned int>(denominator);

  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_CAMERA_EXPOSURE_TIME), TIFF_RATIONAL, 1,
      reinterpret_cast<const unsigned char *>(data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetIso(unsigned short iso) {
  unsigned int count = 1;

  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_CAMERA_ISO), TIFF_SHORT, count,
      reinterpret_cast<const unsigned char *>(&iso), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetImageDescription(const std::string &ascii) {
  unsigned int count =
      static_cast<unsigned int>(ascii.length() + 1);  // +1 for '\0'

  if (count < 2) {
    // empty string
    return false;
  }

  if (count > (1024 * 1024)) {
    // too large
    return false;
  }

  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_IMAGEDESCRIPTION),
                          TIFF_ASCII, count,
                          reinterpret_cast<const unsigned char *>(ascii.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetUniqueCameraModel(const std::string &ascii) {
  unsigned int count =
      static_cast<unsigned int>(ascii.length() + 1);  // +1 for '\0'

  if (count < 2) {
    // empty string
    return false;
  }

  if (count > (1024 * 1024)) {
    // too large
    return false;
  }

  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_UNIQUE_CAMERA_MODEL),
                          TIFF_ASCII, count,
                          reinterpret_cast<const unsigned char *>(ascii.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetSoftware(const std::string &ascii) {
  unsigned int count =
      static_cast<unsigned int>(ascii.length() + 1);  // +1 for '\0'

  if (count < 2) {
    // empty string
    return false;
  }

  if (count > 4096) {
    // too large
    return false;
  }

  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_SOFTWARE),
                          TIFF_ASCII, count,
                          reinterpret_cast<const unsigned char *>(ascii.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}


bool DNGImage::SetActiveArea(std::vector<uint32_t> values) {
  unsigned int count = 4;
  const unsigned int *data = values.data();

  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_ACTIVE_AREA), TIFF_LONG, count,
      reinterpret_cast<const unsigned char *>(data), &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetDNGVersion(const unsigned char a,
                             const unsigned char b,
                             const unsigned char c,
                             const unsigned char d) {
  unsigned char data[4] = {a, b, c, d};

  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_DNG_VERSION), TIFF_BYTE, 4,
      reinterpret_cast<const unsigned char *>(data),
      &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetDNGBackwardVersion(const unsigned char a,
                                     const unsigned char b,
                                     const unsigned char c,
                                     const unsigned char d) {
  unsigned char data[4] = {a, b, c, d};

  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_DNG_BACKWARD_VERSION), TIFF_BYTE, 4,
      reinterpret_cast<const unsigned char *>(data),
      &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetColorMatrix1(const unsigned int plane_count,
                               const float *matrix_values) {
  std::vector<int> vs(plane_count * 3 * 2);
  for (size_t i = 0; i * 2 < vs.size(); i++) {
    float numerator, denominator;
    if (FloatToRational(matrix_values[i], &numerator, &denominator) != 0) {
      // Couldn't represent fp value as integer rational value.
      return false;
    }

    vs[2 * i + 0] = static_cast<int>(numerator);
    vs[2 * i + 1] = static_cast<int>(denominator);

    // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
    if (swap_endian_) {
      swap4(&vs[2 * i + 0]);
      swap4(&vs[2 * i + 1]);
    }
  }
  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_COLOR_MATRIX1),
                          TIFF_SRATIONAL, uint32_t(vs.size() / 2),
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetColorMatrix2(const unsigned int plane_count,
                               const float *matrix_values) {
  std::vector<int> vs(plane_count * 3 * 2);
  for (size_t i = 0; i * 2 < vs.size(); i++) {
    float numerator, denominator;
    if (FloatToRational(matrix_values[i], &numerator, &denominator) != 0) {
      // Couldn't represent fp value as integer rational value.
      return false;
    }

    vs[2 * i + 0] = static_cast<int>(numerator);
    vs[2 * i + 1] = static_cast<int>(denominator);

    // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
    if (swap_endian_) {
      swap4(&vs[2 * i + 0]);
      swap4(&vs[2 * i + 1]);
    }
  }
  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_COLOR_MATRIX2),
                          TIFF_SRATIONAL, uint32_t(vs.size() / 2),
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetForwardMatrix1(const unsigned int plane_count,
                                 const float *matrix_values) {
  std::vector<int> vs(plane_count * 3 * 2);
  for (size_t i = 0; i * 2 < vs.size(); i++) {
    float numerator, denominator;
    if (FloatToRational(matrix_values[i], &numerator, &denominator) != 0) {
      // Couldn't represent fp value as integer rational value.
      return false;
    }

    vs[2 * i + 0] = static_cast<int>(numerator);
    vs[2 * i + 1] = static_cast<int>(denominator);

    // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
    if (swap_endian_) {
      swap4(&vs[2 * i + 0]);
      swap4(&vs[2 * i + 1]);
    }
  }
  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_FORWARD_MATRIX1),
                          TIFF_SRATIONAL, uint32_t(vs.size() / 2),
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetForwardMatrix2(const unsigned int plane_count,
                                 const float *matrix_values) {
  std::vector<int> vs(plane_count * 3 * 2);
  for (size_t i = 0; i * 2 < vs.size(); i++) {
    float numerator, denominator;
    if (FloatToRational(matrix_values[i], &numerator, &denominator) != 0) {
      // Couldn't represent fp value as integer rational value.
      return false;
    }

    vs[2 * i + 0] = static_cast<int>(numerator);
    vs[2 * i + 1] = static_cast<int>(denominator);

    // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
    if (swap_endian_) {
      swap4(&vs[2 * i + 0]);
      swap4(&vs[2 * i + 1]);
    }
  }
  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_FORWARD_MATRIX2),
                          TIFF_SRATIONAL, uint32_t(vs.size() / 2),
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetCameraCalibration1(const unsigned int plane_count,
                                     const float *matrix_values) {
  std::vector<unsigned int> vs(plane_count * plane_count * 2);
  for (size_t i = 0; i * 2 < vs.size(); i++) {
    float numerator, denominator;
    if (FloatToRational(matrix_values[i], &numerator, &denominator) != 0) {
      // Couldn't represent fp value as integer rational value.
      return false;
    }

    vs[2 * i + 0] = static_cast<unsigned int>(numerator);
    vs[2 * i + 1] = static_cast<unsigned int>(denominator);

    // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
    if (swap_endian_) {
      swap4(&vs[2 * i + 0]);
      swap4(&vs[2 * i + 1]);
    }
  }
  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_CAMERA_CALIBRATION1),
                          TIFF_SRATIONAL, uint32_t(vs.size() / 2),
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetCameraCalibration2(const unsigned int plane_count,
                                     const float *matrix_values) {
  std::vector<unsigned int> vs(plane_count * plane_count * 2);
  for (size_t i = 0; i * 2 < vs.size(); i++) {
    float numerator, denominator;
    if (FloatToRational(matrix_values[i], &numerator, &denominator) != 0) {
      // Couldn't represent fp value as integer rational value.
      return false;
    }

    vs[2 * i + 0] = static_cast<unsigned int>(numerator);
    vs[2 * i + 1] = static_cast<unsigned int>(denominator);

    // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
    if (swap_endian_) {
      swap4(&vs[2 * i + 0]);
      swap4(&vs[2 * i + 1]);
    }
  }
  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_CAMERA_CALIBRATION2),
                          TIFF_SRATIONAL, uint32_t(vs.size() / 2),
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetAnalogBalance(const unsigned int plane_count,
                                const float *matrix_values) {
  std::vector<unsigned int> vs(plane_count * 2);
  for (size_t i = 0; i * 2 < vs.size(); i++) {
    float numerator, denominator;
    if (FloatToRational(matrix_values[i], &numerator, &denominator) != 0) {
      // Couldn't represent fp value as integer rational value.
      return false;
    }

    vs[2 * i + 0] = static_cast<unsigned int>(numerator);
    vs[2 * i + 1] = static_cast<unsigned int>(denominator);

    // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
    if (swap_endian_) {
      swap4(&vs[2 * i + 0]);
      swap4(&vs[2 * i + 1]);
    }
  }
  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_ANALOG_BALANCE),
                          TIFF_RATIONAL, uint32_t(vs.size() / 2),
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetCFARepeatPatternDim(const unsigned short width,
                                      const unsigned short height) {
  unsigned short data[2] = {width, height};

  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_CFA_REPEAT_PATTERN_DIM), TIFF_SHORT, 2,
      reinterpret_cast<const unsigned char *>(data),
      &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetBlackLevelRepeatDim(const unsigned short width,
                                      const unsigned short height) {
  unsigned short data[2] = {width, height};

  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_BLACK_LEVEL_REPEAT_DIM), TIFF_SHORT, 2,
      reinterpret_cast<const unsigned char *>(data),
      &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetCalibrationIlluminant1(const unsigned short value) {
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_CALIBRATION_ILLUMINANT1), TIFF_SHORT, 1,
      reinterpret_cast<const unsigned char *>(&value),
      &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetCalibrationIlluminant2(const unsigned short value) {
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_CALIBRATION_ILLUMINANT2), TIFF_SHORT, 1,
      reinterpret_cast<const unsigned char *>(&value),
      &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetCFAPattern(const unsigned int num_components,
                             std::vector<uint8_t> values2) {
    const unsigned char *values = values2.data();
  if ((values == NULL) || (num_components < 1)) {
    return false;
  }

  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_CFA_PATTERN), TIFF_BYTE, num_components,
      reinterpret_cast<const unsigned char *>(values),
      &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetCFALayout(const unsigned short value) {
  bool ret = WriteTIFFTag(
      static_cast<unsigned short>(TIFFTAG_CFA_LAYOUT), TIFF_SHORT, 1,
      reinterpret_cast<const unsigned char *>(&value),
      &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetAsShotNeutral(const unsigned int plane_count,
                                const float *matrix_values) {
  std::vector<unsigned int> vs(plane_count * 2);
  for (size_t i = 0; i * 2 < vs.size(); i++) {
    float numerator, denominator;
    if (FloatToRational(matrix_values[i], &numerator, &denominator) != 0) {
      // Couldn't represent fp value as integer rational value.
      return false;
    }

    vs[2 * i + 0] = static_cast<unsigned int>(numerator);
    vs[2 * i + 1] = static_cast<unsigned int>(denominator);

    // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
    if (swap_endian_) {
      swap4(&vs[2 * i + 0]);
      swap4(&vs[2 * i + 1]);
    }
  }
  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_AS_SHOT_NEUTRAL),
                          TIFF_RATIONAL, uint32_t(vs.size() / 2),
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetAsShotWhiteXY(const float x, const float y) {
  const float values[2] = {x, y};
  std::vector<unsigned int> vs(2 * 2);
  for (size_t i = 0; i * 2 < vs.size(); i++) {
    float numerator, denominator;
    if (FloatToRational(values[i], &numerator, &denominator) != 0) {
      // Couldn't represent fp value as integer rational value.
      return false;
    }

    vs[2 * i + 0] = static_cast<unsigned int>(numerator);
    vs[2 * i + 1] = static_cast<unsigned int>(denominator);

    // TODO(syoyo): Swap rational value(8 bytes) when writing IFD tag, not here.
    if (swap_endian_) {
      swap4(&vs[2 * i + 0]);
      swap4(&vs[2 * i + 1]);
    }
  }
  bool ret = WriteTIFFTag(static_cast<unsigned short>(TIFFTAG_AS_SHOT_WHITE_XY),
                          TIFF_RATIONAL, uint32_t(vs.size() / 2),
                          reinterpret_cast<const unsigned char *>(vs.data()),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetImageData(const std::vector<uint8_t> *imageData) {
  const unsigned char *data = reinterpret_cast<const unsigned char*>(imageData->data());
  const size_t data_len = imageData->size();
    
  if ((data == NULL) || (data_len < 1)) {
    return false;
  }

  data_strip_offset_ = size_t(data_os_.tellp());
  data_strip_bytes_ = data_len;

  data_os_.write(reinterpret_cast<const char *>(data),
                 static_cast<std::streamsize>(data_len));

  // NOTE: STRIP_OFFSET tag will be written at `WriteIFDToStream()`.

  {
    unsigned int count = 1;
    unsigned int bytes = static_cast<unsigned int>(data_len);

    bool ret = WriteTIFFTag(
        static_cast<unsigned short>(TIFFTAG_STRIP_BYTE_COUNTS), TIFF_LONG,
        count, reinterpret_cast<const unsigned char *>(&bytes), &ifd_tags_,
        NULL);

    if (!ret) {
      return false;
    }

    num_fields_++;
  }

  return true;
}

bool DNGImage::SetCustomFieldLong(const unsigned short tag, const int value) {
  unsigned int count = 1;

  // TODO(syoyo): Check if `tag` value does not conflict with existing TIFF tag
  // value.

  bool ret = WriteTIFFTag(tag, TIFF_SLONG, count,
                          reinterpret_cast<const unsigned char *>(&value),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

bool DNGImage::SetCustomFieldULong(const unsigned short tag,
                                   const unsigned int value) {
  unsigned int count = 1;

  // TODO(syoyo): Check if `tag` value does not conflict with existing TIFF tag
  // value.

  bool ret = WriteTIFFTag(tag, TIFF_LONG, count,
                          reinterpret_cast<const unsigned char *>(&value),
                          &ifd_tags_, &data_os_);

  if (!ret) {
    return false;
  }

  num_fields_++;
  return true;
}

static bool IFDComparator(const IFDTag &a, const IFDTag &b) {
  return (a.tag < b.tag);
}

bool DNGImage::WriteDataToStream(std::ostream *ofs) const {
  if ((data_os_.str().length() == 0)) {
    err_ += "Empty IFD data and image data.\n";
    return false;
  }

  if (bits_per_samples_.empty()) {
    err_ += "BitsPerSample is not set\n";
    return false;
  }

  for (size_t i = 0; i < bits_per_samples_.size(); i++) {
    if (bits_per_samples_[i] == 0) {
      err_ += std::to_string(i) + "'th BitsPerSample is zero";
      return false;
    }
  }

  if (samples_per_pixels_ == 0) {
    err_ += "SamplesPerPixels is not set or zero.";
    return false;
  }

  std::vector<uint8_t> data(data_os_.str().length());
  memcpy(data.data(), data_os_.str().data(), data.size());

  if (data_strip_bytes_ == 0) {
    // May ok?.
  } else {
    // FIXME(syoyo): Assume all channels use sample bps
    uint32_t bps = bits_per_samples_[0];

    // We may need to swap endian for pixel data.
    if (swap_endian_) {
      if (bps == 16) {
        size_t n = data_strip_bytes_ / sizeof(uint16_t);
        uint16_t *ptr =
            reinterpret_cast<uint16_t *>(data.data() + data_strip_offset_);

        for (size_t i = 0; i < n; i++) {
          swap2(&ptr[i]);
        }

      } else if (bps == 32) {
        size_t n = data_strip_bytes_ / sizeof(uint32_t);
        uint32_t *ptr =
            reinterpret_cast<uint32_t *>(data.data() + data_strip_offset_);

        for (size_t i = 0; i < n; i++) {
          swap4(&ptr[i]);
        }

      } else if (bps == 64) {
        size_t n = data_strip_bytes_ / sizeof(uint64_t);
        uint64_t *ptr =
            reinterpret_cast<uint64_t *>(data.data() + data_strip_offset_);

        for (size_t i = 0; i < n; i++) {
          swap8(&ptr[i]);
        }
      }
    }
  }

  ofs->write(reinterpret_cast<const char *>(data.data()),
             static_cast<std::streamsize>(data.size()));

  return true;
}

bool DNGImage::WriteIFDToStream(const unsigned int data_base_offset,
                                const unsigned int strip_offset,
                                std::ostream *ofs) const {
  if ((num_fields_ == 0) || (ifd_tags_.size() < 1)) {
    err_ += "No TIFF Tags.\n";
    return false;
  }

  // add STRIP_OFFSET tag and sort IFD tags.
  std::vector<IFDTag> tags = ifd_tags_;
  {
    // For STRIP_OFFSET we need the actual offset value to data(image),
    // thus write STRIP_OFFSET here.
    unsigned int offset = strip_offset + kHeaderSize;
    IFDTag ifd;
    ifd.tag = TIFFTAG_STRIP_OFFSET;
    ifd.type = TIFF_LONG;
    ifd.count = 1;
    ifd.offset_or_value = offset;
    tags.push_back(ifd);
  }

  // TIFF expects IFD tags are sorted.
  std::sort(tags.begin(), tags.end(), IFDComparator);

  std::ostringstream ifd_os;

  unsigned short num_fields = static_cast<unsigned short>(tags.size());

  Write2(num_fields, &ifd_os, swap_endian_);

  {
    size_t typesize_table[] = {1, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};

    for (size_t i = 0; i < tags.size(); i++) {
      const IFDTag &ifd = tags[i];
      Write2(ifd.tag, &ifd_os, swap_endian_);
      Write2(ifd.type, &ifd_os, swap_endian_);
      Write4(ifd.count, &ifd_os, swap_endian_);

      size_t len =
          ifd.count * (typesize_table[(ifd.type) < 14 ? (ifd.type) : 0]);
      if (len > 4) {
        // Store offset value.
        unsigned int ifd_offt = ifd.offset_or_value + data_base_offset;
        Write4(ifd_offt, &ifd_os, swap_endian_);
      } else {
        // less than 4 bytes = store data itself.

        if (len == 1) {
          const unsigned char value =
              *(reinterpret_cast<const unsigned char *>(&ifd.offset_or_value));
          Write1(value, &ifd_os);
          unsigned char pad = 0;
          Write1(pad, &ifd_os);
          Write1(pad, &ifd_os);
          Write1(pad, &ifd_os);
        } else if (len == 2) {
          const unsigned short value =
              *(reinterpret_cast<const unsigned short *>(&ifd.offset_or_value));
          Write2(value, &ifd_os, swap_endian_);
          const unsigned short pad = 0;
          Write2(pad, &ifd_os, swap_endian_);
        } else if (len == 4) {
          const unsigned int value =
              *(reinterpret_cast<const unsigned int *>(&ifd.offset_or_value));
          Write4(value, &ifd_os, swap_endian_);
        } else {
          assert(0);
        }
      }
    }

    ofs->write(ifd_os.str().c_str(),
               static_cast<std::streamsize>(ifd_os.str().length()));
  }

  return true;
}

// -------------------------------------------

DNGWriter::DNGWriter(bool big_endian) : dng_big_endian_(big_endian) {
  swap_endian_ = (IsBigEndian() != dng_big_endian_);
}

const char* DNGWriter::WriteToFile(DNGImage *image,
                                   std::string  *err,
                                   unsigned long *count) const SWIFT_RETURNS_INDEPENDENT_VALUE
{
  std::ostringstream ofs;
  std::ostringstream header;
  if (! WriteTIFFVersionHeader(&header, dng_big_endian_)) {
    if (err) *err = "Failed to write TIFF version header.\n";
    return nullptr;
  }

  const size_t  data_size    = image->GetDataSize();
  const size_t  strip_offset = image->GetStripOffset();
  const unsigned int ifd_offset =
    kHeaderSize + static_cast<unsigned int>(data_size);

  Write4(ifd_offset, &header, swap_endian_);

  assert(header.str().length() == kHeaderSize);

  ofs.write(header.str().c_str(),
            static_cast<std::streamsize>(header.str().length()));

  if (! image->WriteDataToStream(&ofs)) {
    if (err) {
      *err  = "Failed to write image data: ";
      *err += image->Error();
    }
    return nullptr;
  }

  const unsigned int data_offset = 0;
  if (! image->WriteIFDToStream(
         data_offset,
         static_cast<unsigned int>(strip_offset),
         &ofs)) {
    if (err) {
      *err  = "Failed to write IFD: ";
      *err += image->Error();
    }
    return nullptr;
  }

  {
    unsigned int zero = 0;
    if (swap_endian_) swap4(&zero);
    ofs.write(reinterpret_cast<const char*>(&zero), 4);
  }

  const std::string out_str = ofs.str();
  *count = static_cast<unsigned long>(out_str.size());
  char *out  = static_cast<char*>(std::malloc(out_str.size() + 1));
  std::memcpy(out, out_str.c_str(), out_str.size() + 1);

  return out;
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

}  // namespace tinydngwriter
