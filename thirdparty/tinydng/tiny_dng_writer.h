//
// TinyDNGWriter, single header only DNG writer in C++11.
//

/*
The MIT License (MIT)

Copyright (c) 2016 - 2020 Syoyo Fujita.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef TINY_DNG_WRITER_H_
#define TINY_DNG_WRITER_H_

#include <sstream>
#include <vector>
#include <swift/bridging>

#ifndef ROL32
#define ROL32(v,a) ((v) << (a) | (v) >> (32-(a)))
#endif

#ifndef ROL16
#define ROL16(v,a) ((v) << (a) | (v) >> (16-(a)))
#endif

// low-level read and write functions
#ifdef _MSC_VER
# include <io.h>
#else
# include <unistd.h>
//extern "C" {
//    int write (int fd, const char* buf, int num);
//    int read (int fd, char* buf, int num);
//}
#endif


// BEGIN namespace BOOST
namespace boost {


/************************************************************
 * fdostream
 * - a stream that writes on a file descriptor
 ************************************************************/


class fdoutbuf : public std::streambuf {
  protected:
    int fd;    // file descriptor
  public:
    // constructor
    fdoutbuf (int _fd) : fd(_fd) {
    }
  protected:
    // write one character
    virtual int_type overflow (int_type c) {
        if (c != EOF) {
            char z = c;
            if (write (fd, &z, 1) != 1) {
                return EOF;
            }
        }
        return c;
    }
    // write multiple characters
    virtual
    std::streamsize xsputn (const char* s,
                            std::streamsize num) {
        return write(fd,s,num);
    }
};

class fdostream : public std::ostream {
  protected:
    fdoutbuf buf;
  public:
    fdostream (int fd) : std::ostream(0), buf(fd) {
        rdbuf(&buf);
    }
};

} // END namespace boost

namespace tinydngwriter {

typedef enum {
  TIFFTAG_SUB_FILETYPE = 254,
  TIFFTAG_IMAGE_WIDTH = 256,
  TIFFTAG_IMAGE_LENGTH = 257,
  TIFFTAG_BITS_PER_SAMPLE = 258,
  TIFFTAG_COMPRESSION = 259,
  TIFFTAG_PHOTOMETRIC = 262,
  TIFFTAG_IMAGEDESCRIPTION = 270,
  TIFFTAG_STRIP_OFFSET = 273,
  TIFFTAG_SAMPLES_PER_PIXEL = 277,
  TIFFTAG_ROWS_PER_STRIP = 278,
  TIFFTAG_STRIP_BYTE_COUNTS = 279,
  TIFFTAG_PLANAR_CONFIG = 284,
  TIFFTAG_ORIENTATION = 274,

  TIFFTAG_XRESOLUTION = 282,  // rational
  TIFFTAG_YRESOLUTION = 283,  // rational
  TIFFTAG_RESOLUTION_UNIT = 296,

  TIFFTAG_SOFTWARE = 305,

  TIFFTAG_SAMPLEFORMAT = 339,

  // DNG extension
  TIFFTAG_CFA_REPEAT_PATTERN_DIM = 33421,
  TIFFTAG_CFA_PATTERN = 33422,

  TIFFTAG_CAMERA_EXPOSURE_TIME = 33434,
  TIFFTAG_CAMERA_ISO = 34855,

  TIFFTAG_CFA_LAYOUT = 50711,

  TIFFTAG_DNG_VERSION = 50706,
  TIFFTAG_DNG_BACKWARD_VERSION = 50707,
  TIFFTAG_UNIQUE_CAMERA_MODEL = 50708,
  TIFFTAG_CHRROMA_BLUR_RADIUS = 50703,
  TIFFTAG_BLACK_LEVEL_REPEAT_DIM = 50713,
  TIFFTAG_BLACK_LEVEL = 50714,
  TIFFTAG_WHITE_LEVEL = 50717,
  TIFFTAG_COLOR_MATRIX1 = 50721,
  TIFFTAG_COLOR_MATRIX2 = 50722,
  TIFFTAG_CAMERA_CALIBRATION1 = 50723,
  TIFFTAG_CAMERA_CALIBRATION2 = 50724,
  TIFFTAG_ANALOG_BALANCE = 50727,
  TIFFTAG_AS_SHOT_NEUTRAL = 50728,
  TIFFTAG_AS_SHOT_WHITE_XY = 50729,
  TIFFTAG_CALIBRATION_ILLUMINANT1 = 50778,
  TIFFTAG_CALIBRATION_ILLUMINANT2 = 50779,
  TIFFTAG_EXTRA_CAMERA_PROFILES = 50933,
  TIFFTAG_PROFILE_NAME = 50936,
  TIFFTAG_AS_SHOT_PROFILE_NAME = 50934,
  TIFFTAG_DEFAULT_BLACK_RENDER = 51110,
  TIFFTAG_ACTIVE_AREA = 50829,
  TIFFTAG_FORWARD_MATRIX1 = 50964,
  TIFFTAG_FORWARD_MATRIX2 = 50965,

  // CinemaDNG specific
  TIFFTAG_TIMECODE = 51043,
  TIFFTAG_FPS = 51044
} Tag;

// SUBFILETYPE(bit field)
static const int FILETYPE_REDUCEDIMAGE = 1;
static const int FILETYPE_PAGE = 2;
static const int FILETYPE_MASK = 4;

// PLANARCONFIG
static const int PLANARCONFIG_CONTIG = 1;
static const int PLANARCONFIG_SEPARATE = 2;

// COMPRESSION
// TODO(syoyo) more compressin types.
static const int COMPRESSION_NONE = 1;

// ORIENTATION
static const int ORIENTATION_TOPLEFT = 1;
static const int ORIENTATION_TOPRIGHT = 2;
static const int ORIENTATION_BOTRIGHT = 3;
static const int ORIENTATION_BOTLEFT = 4;
static const int ORIENTATION_LEFTTOP = 5;
static const int ORIENTATION_RIGHTTOP = 6;
static const int ORIENTATION_RIGHTBOT = 7;
static const int ORIENTATION_LEFTBOT = 8;

// RESOLUTIONUNIT
static const int RESUNIT_NONE = 1;
static const int RESUNIT_INCH = 2;
static const int RESUNIT_CENTIMETER = 2;

// PHOTOMETRIC
// TODO(syoyo): more photometric types.
static const int PHOTOMETRIC_WHITE_IS_ZERO = 0;  // For bilevel and grayscale
static const int PHOTOMETRIC_BLACK_IS_ZERO = 1;  // For bilevel and grayscale
static const int PHOTOMETRIC_RGB = 2;            // Default
static const int PHOTOMETRIC_CFA = 32803;        // DNG ext
static const int PHOTOMETRIC_LINEARRAW = 34892;  // DNG ext

// Sample format
static const int SAMPLEFORMAT_UINT = 1;  // Default
static const int SAMPLEFORMAT_INT = 2;
static const int SAMPLEFORMAT_IEEEFP = 3;  // floating point

struct IFDTag {
  unsigned short tag;
  unsigned short type;
  unsigned int count;
  unsigned int offset_or_value;
};
// 12 bytes.

class DNGImage {
 public:
  DNGImage();

  ///
  /// Optional: Explicitly specify endian.
  /// Must be called before calling other Set methods.
  ///
  void SetBigEndian(bool big_endian);

  ///
  /// Default = 0
  ///
  bool SetSubfileType(bool reduced_image = false, bool page = false,
                      bool mask = false);

  bool SetImageWidth(unsigned int value);
  bool SetImageLength(unsigned int value);
  bool SetRowsPerStrip(unsigned int value);
  bool SetSamplesPerPixel(unsigned short value);
  // Set bits for each samples
  bool SetBitsPerSample();
  bool SetPhotometric(unsigned short value);
  bool SetPlanarConfig(unsigned short value);
  bool SetOrientation(unsigned short value);
  bool SetCompression(unsigned short value);
  bool SetSampleFormat(const unsigned int num_samples,
                       const unsigned short *values);
  bool SetXResolution(float value);
  bool SetYResolution(float value);
  bool SetResolutionUnit(const unsigned short value);

  bool SetFrameRate(float value);
  bool SetTimeCode(unsigned char timecode[8]);
  bool SetExposureTime(float exposureSecs);
  bool SetIso(unsigned short iso);

  ///
  /// Set arbitrary string for image description.
  /// Currently we limit to 1024*1024 chars at max.
  ///
  bool SetImageDescription(const std::string &ascii);

  ///
  /// Set arbitrary string for unique camera model name (not localized!).
  /// Currently we limit to 1024*1024 chars at max.
  ///
  bool SetUniqueCameraModel(const std::string &ascii);

  ///
  /// Set software description(string).
  /// Currently we limit to 4095 chars at max.
  ///
  bool SetSoftware(const std::string &ascii);

  bool SetActiveArea(std::vector<uint32_t> values);

  bool SetChromaBlurRadius(float value);

  /// Specify black level per sample.
  bool SetBlackLevel(const unsigned int num_samples, const unsigned short *values);

  /// Specify black level per sample (as rational values).
  bool SetBlackLevelRational(unsigned int num_samples, const float *values);

  /// Specify white level per sample.
  bool SetWhiteLevel(const short value);
  bool SetWhiteLevelRational(unsigned int num_samples, const float *values);

  /// Specify analog white balance from camera for raw values.
  bool SetAnalogBalance(const unsigned int plane_count, const float *matrix_values);

  /// Specify CFA repeating pattern dimensions.
  bool SetCFARepeatPatternDim(const unsigned short width, const unsigned short height);

  /// Specify black level repeating pattern dimensions.
  bool SetBlackLevelRepeatDim(const unsigned short width, const unsigned short height);

  bool SetCalibrationIlluminant1(const unsigned short value);
  bool SetCalibrationIlluminant2(const unsigned short value);

  /// Specify DNG version.
  bool SetDNGVersion(const unsigned char a, const unsigned char b, const unsigned char c, const unsigned char d);
  bool SetDNGBackwardVersion(const unsigned char a, const unsigned char b, const unsigned char c, const unsigned char d);

  /// Specify transformation matrix (XYZ to reference camera native color space values, under the first calibration illuminant).
  bool SetColorMatrix1(const unsigned int plane_count, const float *matrix_values);

  /// Specify transformation matrix (XYZ to reference camera native color space values, under the second calibration illuminant).
  bool SetColorMatrix2(const unsigned int plane_count, const float *matrix_values);

  bool SetForwardMatrix1(const unsigned int plane_count, const float *matrix_values);
  bool SetForwardMatrix2(const unsigned int plane_count, const float *matrix_values);

  bool SetCameraCalibration1(const unsigned int plane_count, const float *matrix_values);
  bool SetCameraCalibration2(const unsigned int plane_count, const float *matrix_values);

  /// Specify CFA geometric pattern (left-to-right, top-to-bottom).
  bool SetCFAPattern(const unsigned int num_components, std::vector<uint8_t> values);
  bool SetCFALayout(const unsigned short value);
  
  /// Specify the selected white balance at time of capture, encoded as the coordinates of a perfectly neutral color in linear reference space values.
  bool SetAsShotNeutral(const unsigned int plane_count, const float *matrix_values);

  /// Specify the the selected white balance at time of capture, encoded as x-y chromaticity coordinates.
  bool SetAsShotWhiteXY(const float x, const float y);

  /// Set image data.
  bool SetImageData(const std::vector<uint8_t> *imageData);

  /// Set custom field.
  bool SetCustomFieldLong(const unsigned short tag, const int value);
  bool SetCustomFieldULong(const unsigned short tag, const unsigned int value);

  size_t GetDataSize() const { return data_os_.str().length(); }

  size_t GetStripOffset() const { return data_strip_offset_; }
  size_t GetStripBytes() const { return data_strip_bytes_; }

  /// Write aux IFD data and strip image data to stream.
  bool WriteDataToStream(std::ostream *ofs) const;

  ///
  /// Write IFD to stream.
  ///
  /// @param[in] data_base_offset : Byte offset to data
  /// @param[in] strip_offset : Byte offset to image strip data
  ///
  /// TODO(syoyo): Support multiple strips
  ///
  bool WriteIFDToStream(const unsigned int data_base_offset,
                        const unsigned int strip_offset, std::ostream *ofs) const;

  std::string Error() const { return err_; }

 private:
  std::ostringstream data_os_;
  bool swap_endian_;
  bool dng_big_endian_;
  unsigned short num_fields_;
  unsigned int samples_per_pixels_;
  std::vector<unsigned short> bits_per_samples_;

  // TODO(syoyo): Support multiple strips
  size_t data_strip_offset_{0};
  size_t data_strip_bytes_{0};

  mutable std::string err_;  // Error message

  std::vector<IFDTag> ifd_tags_;
};

class DNGWriter {
 public:
  // TODO(syoyo): Use same endian setting with DNGImage.
  DNGWriter(bool big_endian);

    /// Write DNG to a file.
    /// Return error string to `err` when Write() returns false.
    /// Returns true upon success.
    const char* WriteToFile(DNGImage *image,
                                       std::string  *err,
                            unsigned long *count) const SWIFT_RETURNS_INDEPENDENT_VALUE;

 private:
  bool swap_endian_;
  bool dng_big_endian_;  // Endianness of DNG file.
};

}  // namespace tinydngwriter

#endif  // TINY_DNG_WRITER_H_
