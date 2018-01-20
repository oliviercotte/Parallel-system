/*
 * sinoscope_openmp.c
 *
 *  Created on: 2011-10-14
 *      Author: francis
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "sinoscope.h"
#include "color.h"
#include "util.h"

int sinoscope_image_openmp(sinoscope_t *ptr) {
	//TODO("sinoscope_image_openmp");

	if (ptr == NULL)
	{
		return -1;
	}

	sinoscope_t b = *ptr;
	int x, y, index, taylor;
	struct rgb c;
	float val, px, py;

	#pragma omp parallel for private(x, y, c, val, px, py, taylor, index) shared(b)
	for (x = 1; x < b.height - 1; ++x)
	{
		for (y = 1; y < b.width - 1; ++y)
		{
			val = 0.0f;
			px = b.dx * y - 2 * M_PI;
			py = b.dy * x - 2 * M_PI;

			for (taylor = 1; taylor <= b.taylor; taylor += 2)
			{
				val += sin(px * taylor * b.phase1 + b.time) / taylor + cos(py * taylor * b.phase0) / taylor;
			}

			val = (atan(1.0 * val) - atan(-1.0 * val)) / (M_PI);
			val = (val + 1) * 100;
			value_color(&c, val, b.interval, b.interval_inv);

			index = (y * 3) + (x * 3) * b.width;
			b.buf[index + 0] = c.r;
			b.buf[index + 1] = c.g;
			b.buf[index + 2] = c.b;
		}
	}

	return 0;
}
