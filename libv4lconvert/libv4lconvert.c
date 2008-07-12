/*
#             (C) 2008 Hans de Goede <j.w.r.degoede@hhs.nl>

# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2.1 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <unistd.h>
#include "libv4lconvert.h"
#include "libv4lconvert-priv.h"

#define MIN(a,b) (((a)<(b))?(a):(b))
#define ARRAY_SIZE(x) ((int)sizeof(x)/(int)sizeof((x)[0]))

/* Note for proper functioning of v4lconvert_enum_fmt the first entries in
  supported_src_pixfmts must match with the entries in supported_dst_pixfmts */
#define SUPPORTED_DST_PIXFMTS \
  V4L2_PIX_FMT_BGR24, \
  V4L2_PIX_FMT_YUV420

static const unsigned int supported_src_pixfmts[] = {
  SUPPORTED_DST_PIXFMTS,
  V4L2_PIX_FMT_MJPEG,
  V4L2_PIX_FMT_JPEG,
  V4L2_PIX_FMT_SBGGR8,
  V4L2_PIX_FMT_SGBRG8,
  V4L2_PIX_FMT_SGRBG8,
  V4L2_PIX_FMT_SRGGB8,
  V4L2_PIX_FMT_SPCA501,
  V4L2_PIX_FMT_SPCA561,
  V4L2_PIX_FMT_SN9C10X,
  V4L2_PIX_FMT_PAC207,
};

static const unsigned int supported_dst_pixfmts[] = {
  SUPPORTED_DST_PIXFMTS
};


struct v4lconvert_data *v4lconvert_create(int fd)
{
  int i, j;
  struct v4lconvert_data *data = calloc(1, sizeof(struct v4lconvert_data));

  if (!data)
    return NULL;

  data->fd = fd;
  data->jdec = NULL;

  /* Check supported formats */
  for (i = 0; ; i++) {
    struct v4l2_fmtdesc fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };

    fmt.index = i;

    if (syscall(SYS_ioctl, fd, VIDIOC_ENUM_FMT, &fmt))
      break;

    for (j = 0; j < ARRAY_SIZE(supported_src_pixfmts); j++)
      if (fmt.pixelformat == supported_src_pixfmts[j]) {
	data->supported_src_formats |= 1 << j;
	break;
      }
  }

  data->no_formats = i;

  return data;
}

void v4lconvert_destroy(struct v4lconvert_data *data)
{
  if (data->jdec) {
    unsigned char *comps[3] = { NULL, NULL, NULL };
    tinyjpeg_set_components(data->jdec, comps, 3);
    tinyjpeg_free(data->jdec);
  }
  free(data);
}

/* See libv4lconvert.h for description of in / out parameters */
int v4lconvert_enum_fmt(struct v4lconvert_data *data, struct v4l2_fmtdesc *fmt)
{
  int i, no_faked_fmts = 0;
  unsigned int faked_fmts[ARRAY_SIZE(supported_dst_pixfmts)];

  if (fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
      fmt->index < data->no_formats ||
      !data->supported_src_formats)
    return syscall(SYS_ioctl, data->fd, VIDIOC_ENUM_FMT, fmt);

  for (i = 0; i < ARRAY_SIZE(supported_dst_pixfmts); i++)
    if (!(data->supported_src_formats & (1 << i))) {
      faked_fmts[no_faked_fmts] = supported_dst_pixfmts[i];
      no_faked_fmts++;
    }

  i = fmt->index - data->no_formats;
  if (i >= no_faked_fmts) {
    errno = EINVAL;
    return -1;
  }

  fmt->flags = 0;
  fmt->pixelformat = faked_fmts[i];
  fmt->description[0] = faked_fmts[i] & 0xff;
  fmt->description[1] = (faked_fmts[i] >> 8) & 0xff;
  fmt->description[2] = (faked_fmts[i] >> 16) & 0xff;
  fmt->description[3] = faked_fmts[i] >> 24;
  fmt->description[4] = '\0';
  memset(fmt->reserved, 0, 4);

  return 0;
}

/* See libv4lconvert.h for description of in / out parameters */
int v4lconvert_try_format(struct v4lconvert_data *data,
  struct v4l2_format *dest_fmt, struct v4l2_format *src_fmt)
{
  int i;
  unsigned int closest_fmt_size_diff = -1;
  unsigned int desired_pixfmt = dest_fmt->fmt.pix.pixelformat;
  struct v4l2_format try_fmt, closest_fmt = { .type = 0 };

  for (i = 0; i < ARRAY_SIZE(supported_dst_pixfmts); i++)
    if (supported_dst_pixfmts[i] == desired_pixfmt)
      break;

  /* Can we do conversion to the requested format & type? */
  if (i == ARRAY_SIZE(supported_dst_pixfmts) ||
      dest_fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
    int ret = syscall(SYS_ioctl, data->fd, VIDIOC_TRY_FMT, dest_fmt);
    if (src_fmt)
      *src_fmt = *dest_fmt;
    return ret;
  }

  for (i = 0; i < ARRAY_SIZE(supported_src_pixfmts); i++) {
    /* is this format supported? */
    if (!(data->supported_src_formats & (1 << i)))
      continue;

    try_fmt = *dest_fmt;
    try_fmt.fmt.pix.pixelformat = supported_src_pixfmts[i];

    if (!syscall(SYS_ioctl, data->fd, VIDIOC_TRY_FMT, &try_fmt))
    {
      if (try_fmt.fmt.pix.pixelformat == supported_src_pixfmts[i]) {
	int size_x_diff = abs((int)try_fmt.fmt.pix.width -
			      (int)dest_fmt->fmt.pix.width);
	int size_y_diff = abs((int)try_fmt.fmt.pix.height -
			      (int)dest_fmt->fmt.pix.height);
	unsigned int size_diff = size_x_diff * size_x_diff +
				 size_y_diff * size_y_diff;
	if (size_diff < closest_fmt_size_diff) {
	  closest_fmt_size_diff = size_diff;
	  closest_fmt = try_fmt;
	}
      }
    }
  }

  if (closest_fmt.type == 0) {
    int ret = syscall(SYS_ioctl, data->fd, VIDIOC_TRY_FMT, dest_fmt);
    if (src_fmt)
      *src_fmt = *dest_fmt;
    return ret;
  }

  *dest_fmt = closest_fmt;

  /* Are we converting? */
  if (closest_fmt.fmt.pix.pixelformat != desired_pixfmt) {
    dest_fmt->fmt.pix.pixelformat = desired_pixfmt;
    switch (dest_fmt->fmt.pix.pixelformat) {
      case V4L2_PIX_FMT_BGR24:
	dest_fmt->fmt.pix.bytesperline = dest_fmt->fmt.pix.width * 3;
	dest_fmt->fmt.pix.sizeimage = dest_fmt->fmt.pix.width *
				      dest_fmt->fmt.pix.height * 3;
	break;
      case V4L2_PIX_FMT_YUV420:
	dest_fmt->fmt.pix.bytesperline = dest_fmt->fmt.pix.width;
	dest_fmt->fmt.pix.sizeimage = (dest_fmt->fmt.pix.width *
				       dest_fmt->fmt.pix.height * 3) / 2;
	break;
    }
  }

  if (src_fmt)
    *src_fmt = closest_fmt;

  return 0;
}

int v4lconvert_convert(struct v4lconvert_data *data,
  const struct v4l2_format *src_fmt,  /* in */
  const struct v4l2_format *dest_fmt, /* in */
  unsigned char *src, int src_size, unsigned char *dest, int dest_size)
{
  unsigned int header_width, header_height;
  int result, needed;
  unsigned char *components[3];

  /* Special case when no conversion is needed */
  if(!memcmp(src_fmt, dest_fmt, sizeof(*src_fmt))) {
    int to_copy = MIN(dest_size, src_size);
    memcpy(dest, src, to_copy);
    return to_copy;
  }

  /* sanity check, is the dest buffer large enough? */
  switch (dest_fmt->fmt.pix.pixelformat) {
    case V4L2_PIX_FMT_BGR24:
      needed = dest_fmt->fmt.pix.width * dest_fmt->fmt.pix.height * 3;
      break;
    case V4L2_PIX_FMT_YUV420:
      needed = (dest_fmt->fmt.pix.width * dest_fmt->fmt.pix.height * 3) / 2;
      break;
    default:
      V4LCONVERT_ERR("Unknown dest format in conversion\n");
      errno = EINVAL;
      return -1;
  }

  if (dest_size < needed) {
    V4LCONVERT_ERR("destination buffer too small\n");
    errno = EFAULT;
    return -1;
  }

  switch (src_fmt->fmt.pix.pixelformat) {
    case V4L2_PIX_FMT_MJPEG:
    case V4L2_PIX_FMT_JPEG:
      if (!data->jdec) {
	data->jdec = tinyjpeg_init();
	if (!data->jdec) {
	  V4LCONVERT_ERR("out of memory!\n");
	  errno = ENOMEM;
	  return -1;
	}
      }
      tinyjpeg_set_flags(data->jdec,
			 (src_fmt->fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG)?
			 TINYJPEG_FLAGS_MJPEG_TABLE : 0);
      if (tinyjpeg_parse_header(data->jdec, src, src_size)) {
	V4LCONVERT_ERR("parsing JPEG header: %s\n",
	  tinyjpeg_get_errorstring(data->jdec));
	errno = EIO;
	return -1;
      }
      tinyjpeg_get_size(data->jdec, &header_width, &header_height);

      if (header_width != dest_fmt->fmt.pix.width || header_height != dest_fmt->fmt.pix.height) {
	V4LCONVERT_ERR("unexpected width / height in JPEG header\n");
	V4LCONVERT_ERR("expected: %dx%d, header: %ux%u\n",
	  dest_fmt->fmt.pix.width, dest_fmt->fmt.pix.height,
	  header_width, header_height);
	errno = EIO;
	return -1;
      }

      components[0] = dest;
      components[1] = components[0] + dest_fmt->fmt.pix.width *
				      dest_fmt->fmt.pix.height;
      components[2] = components[1] + (dest_fmt->fmt.pix.width *
				       dest_fmt->fmt.pix.height) / 4;

      if (dest_fmt->fmt.pix.pixelformat == V4L2_PIX_FMT_BGR24) {
	tinyjpeg_set_components(data->jdec, components, 1);
	result = tinyjpeg_decode(data->jdec, TINYJPEG_FMT_BGR24);
      } else {
	tinyjpeg_set_components(data->jdec, components, 3);
	result = tinyjpeg_decode(data->jdec, TINYJPEG_FMT_YUV420P);
      }

      /* If the JPEG header checked out ok and we get an error during actual
	 decompression, log the error, but don't return an errorcode to the
	 application, so that the user gets what we managed to decompress */
      if (result)
	fprintf(stderr, "libv4lconvert: Error decompressing JPEG: %s",
	  tinyjpeg_get_errorstring(data->jdec));

      break;

    case V4L2_PIX_FMT_SBGGR8:
    case V4L2_PIX_FMT_SGBRG8:
    case V4L2_PIX_FMT_SGRBG8:
    case V4L2_PIX_FMT_SRGGB8:
      if (dest_fmt->fmt.pix.pixelformat == V4L2_PIX_FMT_BGR24)
	v4lconvert_bayer_to_bgr24(src, dest, dest_fmt->fmt.pix.width,
		    dest_fmt->fmt.pix.height, src_fmt->fmt.pix.pixelformat);
      else
	v4lconvert_bayer_to_yuv420(src, dest, dest_fmt->fmt.pix.width,
		    dest_fmt->fmt.pix.height, src_fmt->fmt.pix.pixelformat);
      break;

    case V4L2_PIX_FMT_SPCA501:
      if (dest_fmt->fmt.pix.pixelformat == V4L2_PIX_FMT_BGR24)
	v4lconvert_spca501_to_bgr24(src, dest, dest_fmt->fmt.pix.width,
				    dest_fmt->fmt.pix.height);
      else
	v4lconvert_spca501_to_yuv420(src, dest, dest_fmt->fmt.pix.width,
				     dest_fmt->fmt.pix.height);
      break;

    /* compressed bayer formats */
    case V4L2_PIX_FMT_SPCA561:
    case V4L2_PIX_FMT_SN9C10X:
    case V4L2_PIX_FMT_PAC207:
    {
      unsigned char tmpbuf[dest_fmt->fmt.pix.width*dest_fmt->fmt.pix.height];
      unsigned int bayer_fmt = 0;

      switch (src_fmt->fmt.pix.pixelformat) {
	case V4L2_PIX_FMT_SPCA561:
	  v4lconvert_decode_spca561(src, tmpbuf, dest_fmt->fmt.pix.width,
				    dest_fmt->fmt.pix.height);
	  bayer_fmt = V4L2_PIX_FMT_SGBRG8;
	  break;
	case V4L2_PIX_FMT_SN9C10X:
	  v4lconvert_decode_sn9c10x(src, tmpbuf, dest_fmt->fmt.pix.width,
				    dest_fmt->fmt.pix.height);
	  bayer_fmt = V4L2_PIX_FMT_SGBRG8;
	  break;
	case V4L2_PIX_FMT_PAC207:
	  v4lconvert_decode_pac207(src, tmpbuf, dest_fmt->fmt.pix.width,
				    dest_fmt->fmt.pix.height);
	  bayer_fmt = V4L2_PIX_FMT_SBGGR8;
	  break;
      }

      if (dest_fmt->fmt.pix.pixelformat == V4L2_PIX_FMT_BGR24)
	v4lconvert_bayer_to_bgr24(tmpbuf, dest, dest_fmt->fmt.pix.width,
		    dest_fmt->fmt.pix.height, bayer_fmt);
      else
	v4lconvert_bayer_to_yuv420(tmpbuf, dest, dest_fmt->fmt.pix.width,
		    dest_fmt->fmt.pix.height, bayer_fmt);
      break;
    }

    case V4L2_PIX_FMT_BGR24:
      /* dest must be V4L2_PIX_FMT_YUV420 then */
      printf("FIXME add bgr24 -> yuv420 conversion\n");
      break;

    case V4L2_PIX_FMT_YUV420:
      /* dest must be V4L2_PIX_FMT_BGR24 then */
      v4lconvert_yuv420_to_bgr24(src, dest, dest_fmt->fmt.pix.width,
				 dest_fmt->fmt.pix.height);
      break;

    default:
      V4LCONVERT_ERR("Unknown src format in conversion\n");
      errno = EINVAL;
      return -1;
  }

  return needed;
}

const char *v4lconvert_get_error_message(struct v4lconvert_data *data)
{
  return data->error_msg;
}
