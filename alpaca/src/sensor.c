#include "sensor.h"
#include "unpack.h"
#include "v4l2_capture.h"

#include <stdio.h>
#include <string.h>

#define v4l2_fourcc(a, b, c, d) \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
	 ((uint32_t)(d) << 24))

/* Values below verified live against real hardware, 2026-09-15:
 *   v4l2-ctl -d /dev/v4l-subdev2 --list-ctrls
 *   v4l2-ctl -d /dev/video0 --list-formats-ext
 * exposure 1..1352 (varies with vertical_blanking), analogue_gain
 * 128..99614 (128 = 1x per grab.py), vertical_blanking 64..31471,
 * horizontal_blanking fixed 496, pixel_rate fixed 102000000. Row time is
 * computed at detect time from the live horizontal_blanking/pixel_rate
 * controls rather than hardcoded, since those could differ per revision. */
static const sensor_desc_t known_sensors[] = {
	{
		.match_substr = "sc3336",
		.display_name = "SC3336",
		.subdev_path = "/dev/v4l-subdev2",
		.video_path = "/dev/video0",
		.width = 2304,
		.height = 1296,
		.v4l2_fourcc = v4l2_fourcc('B', 'G', '1', '0'), /* 10-bit Bayer BGGR/GRGR */
		.bayer = BAYER_BGGR,
		.max_adu = 1023,
		.pixel_size_um_x = 2.0,
		.pixel_size_um_y = 2.0,
		.row_time_us = 0.0, /* filled in by sensor_detect */
		.ctrl_exposure = "exposure",
		.ctrl_gain = "analogue_gain",
		.ctrl_vblank = "vertical_blanking",
		.unpack = unpack_bits_lsb_transposed,
	},
	{
		/* Production target -- values are placeholders (marked TODO) until
		 * the real IMX290 is wired up and verified on hardware. Do not
		 * trust these for anything beyond compiling; sensor_detect() will
		 * only select this entry once a subdev actually reports "imx290". */
		.match_substr = "imx290",
		.display_name = "IMX290 (TODO: unverified placeholder values)",
		.subdev_path = "/dev/v4l-subdev2",
		.video_path = "/dev/video0",
		.width = 1920,  /* TODO: confirm against real hardware */
		.height = 1080, /* TODO: confirm against real hardware */
		.v4l2_fourcc = v4l2_fourcc('B', 'G', '1', '0'), /* TODO: confirm */
		.bayer = BAYER_NONE, /* mono variant assumed for this project */
		.max_adu = 1023,
		.pixel_size_um_x = 2.9, /* TODO: confirm against datasheet */
		.pixel_size_um_y = 2.9,
		.row_time_us = 0.0,
		.ctrl_exposure = "exposure",
		.ctrl_gain = "analogue_gain",
		.ctrl_vblank = "vertical_blanking",
		.unpack = unpack_bits_lsb_transposed, /* TODO: confirm packing once hardware exists */
	},
};

/* Used only when no known sensor matches -- keeps the server usable (it
 * will report obviously-wrong capability numbers rather than crash) so an
 * unrecognised sensor is a visible Alpaca-side problem, not a dead server. */
static const sensor_desc_t fallback_sensor = {
	.match_substr = NULL,
	.display_name = "Unknown (unrecognised subdev, using placeholder values)",
	.subdev_path = "/dev/v4l-subdev2",
	.video_path = "/dev/video0",
	.width = 640,
	.height = 480,
	.v4l2_fourcc = 0,
	.bayer = BAYER_NONE,
	.max_adu = 255,
	.pixel_size_um_x = 1.0,
	.pixel_size_um_y = 1.0,
	.row_time_us = 1.0,
	.ctrl_exposure = "exposure",
	.ctrl_gain = "analogue_gain",
	.ctrl_vblank = "vertical_blanking",
	.unpack = unpack_bits_lsb_transposed,
};

static int read_subdev_name(char *out, size_t outlen) {
	FILE *f = fopen("/sys/class/video4linux/v4l-subdev2/name", "r");
	if (f == NULL)
		return -1;
	char *r = fgets(out, (int)outlen, f);
	fclose(f);
	if (r == NULL)
		return -1;
	out[strcspn(out, "\n")] = '\0';
	return 0;
}

const sensor_desc_t *sensor_detect(void) {
	static sensor_desc_t resolved;
	char name[128] = {0};
	const sensor_desc_t *match = NULL;

	if (read_subdev_name(name, sizeof(name)) == 0) {
		for (size_t i = 0; i < sizeof(known_sensors) / sizeof(known_sensors[0]);
		     i++) {
			if (strstr(name, known_sensors[i].match_substr) != NULL) {
				match = &known_sensors[i];
				break;
			}
		}
	}

	if (match == NULL) {
		fprintf(stderr,
		        "sensor_detect: no known sensor matched subdev name '%s' -- "
		        "falling back to placeholder values, Alpaca capability "
		        "properties will be wrong until a real entry is added\n",
		        name[0] ? name : "(unreadable)");
		return &fallback_sensor;
	}

	resolved = *match;

	int64_t hblank = 0, pixel_rate = 0;
	if (v4l2_ctrl_get(resolved.subdev_path, "horizontal_blanking", &hblank) == 0 &&
	    v4l2_ctrl_get(resolved.subdev_path, "pixel_rate", &pixel_rate) == 0 &&
	    pixel_rate > 0) {
		resolved.row_time_us =
		    ((double)resolved.width + (double)hblank) / (double)pixel_rate * 1e6;
	} else {
		fprintf(stderr,
		        "sensor_detect: could not read horizontal_blanking/pixel_rate "
		        "from %s, row_time_us left at 0 -- exposure duration<->rows "
		        "conversion will be wrong\n",
		        resolved.subdev_path);
	}

	return &resolved;
}
