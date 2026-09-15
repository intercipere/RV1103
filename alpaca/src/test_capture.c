/* Standalone hardware test for sensor detection + V4L2 capture + RAW10
 * unpack, run manually via adb -- not part of the alpacad binary. */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "sensor.h"
#include "v4l2_capture.h"

int main(int argc, char **argv) {
	const sensor_desc_t *s = sensor_detect();
	printf("detected: %s (%dx%d, row_time_us=%.3f)\n", s->display_name,
	       s->width, s->height, s->row_time_us);

	int64_t emin = 0, emax = 0;
	if (v4l2_ctrl_get_range(s->subdev_path, s->ctrl_exposure, &emin, &emax) == 0)
		printf("exposure range: %" PRId64 "..%" PRId64 " rows\n", emin, emax);

	v4l2_frame_t frame;
	if (v4l2_capture_frame(s, &frame) != 0) {
		fprintf(stderr, "capture failed\n");
		return 1;
	}
	printf("captured %dx%d\n", frame.width, frame.height);

	uint32_t sum = 0;
	uint16_t vmin = 65535, vmax = 0;
	long n = (long)frame.width * frame.height;
	for (long i = 0; i < n; i++) {
		uint16_t v = frame.pixels[i];
		if (v < vmin) vmin = v;
		if (v > vmax) vmax = v;
		sum += v;
	}
	printf("min=%u max=%u mean=%.1f\n", vmin, vmax, (double)sum / (double)n);

	if (argc > 1) {
		FILE *f = fopen(argv[1], "wb");
		if (f) {
			fwrite(frame.pixels, sizeof(uint16_t), n, f);
			fclose(f);
			printf("wrote raw uint16 dump to %s\n", argv[1]);
		}
	}

	free(frame.pixels);
	return 0;
}
