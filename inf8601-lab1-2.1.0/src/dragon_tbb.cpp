/*
 * dragon_tbb.c
 *
 *  Created on: 2011-08-17
 *      Author: Francis Giraldeau <francis.giraldeau@gmail.com>
 */

#include <iostream>

extern "C" {
#include "dragon.h"
#include "color.h"
#include "utils.h"
}
#include "dragon_tbb.h"
#include "tbb/tbb.h"
#include "TidMap.h"

using namespace std;
using namespace tbb;

class DragonLimits {
public:
	DragonLimits() {
		piece_init(&aPiece);
	}

	DragonLimits(const DragonLimits& dl, split) {
		piece_init(&aPiece);
	}

	void operator()(const blocked_range<uint64_t>& range) {
		piece_limit(range.begin(), range.end(), &aPiece);
	}

	void join(const DragonLimits& dl) {
		piece_merge(&aPiece, dl.mGetPiece());
	}

	piece_t mGetPiece() const {
		return aPiece;
	}

private:
	piece_t aPiece;
};

class DragonDraw {
public:
	DragonDraw(struct draw_data *draw) {
		aDrawData = draw;
		aTidMap = new TidMap(aDrawData->nb_thread);
	}

	DragonDraw(const DragonDraw& dd, split) {
		aDrawData = dd.mGetDrawData();
		aTidMap = new TidMap(aDrawData->nb_thread);
	}


	void operator()(const blocked_range<uint64_t>& range) const {
		printf("DragonDraw id :=  %i, tid := %i , begin := %lu, end := %lu\n",
				aDrawData->id, aTidMap->getIdFromTid(gettid()), range.begin(),
				range.end());
		dragon_draw_raw(range.begin(), range.end(), aDrawData->dragon,
				aDrawData->dragon_width, aDrawData->dragon_height,
				aDrawData->limits, aDrawData->id);
	}

	struct draw_data* mGetDrawData() const {
		return aDrawData;
	}

private:
	TidMap* aTidMap;
	struct draw_data *aDrawData;
};

class DragonRender {
public:
	DragonRender(struct draw_data *data) {
		aDrawData = data;
	}

	DragonRender(const DragonRender& dr, split) {
		aDrawData = dr.mGetDrawData();
	}

	void operator()(const blocked_range<int>& r) const {
		scale_dragon(r.begin(), r.end(), aDrawData->image,
				aDrawData->image_width, aDrawData->image_height,
				aDrawData->dragon, aDrawData->dragon_width,
				aDrawData->dragon_height, aDrawData->palette);
	}

	struct draw_data* mGetDrawData() const {
		return aDrawData;
	}

private:
	struct draw_data *aDrawData;
};

class DragonClear {
public:
	DragonClear(struct draw_data *data) {
		aDrawData = data;
	}

	DragonClear(DragonClear& dc, split) {
		aDrawData = dc.mGetDrawData();
	}

	void operator()(const blocked_range<int>& range) const {
		init_canvas(range.begin(), range.end(), aDrawData->dragon, -1);
	}

	struct draw_data* mGetDrawData() const {
		return aDrawData;
	}

private:
	struct draw_data *aDrawData;
};

int dragon_draw_tbb(char **canvas, struct rgb *image, int width, int height,
		uint64_t size, int nb_thread) {
	struct draw_data data;
	limits_t limits;
	char *dragon = NULL;
	int dragon_width;
	int dragon_height;
	int dragon_surface;
	int scale_x;
	int scale_y;
	int scale;
	int deltaJ;
	int deltaI;

	struct palette *palette = init_palette(nb_thread);
	if (palette == NULL)
		return -1;

	/* 1. Calculer les limites du dragon */
	dragon_limits_tbb(&limits, size, nb_thread);

	dragon_width = limits.maximums.x - limits.minimums.x;
	dragon_height = limits.maximums.y - limits.minimums.y;
	dragon_surface = dragon_width * dragon_height;
	scale_x = dragon_width / width + 1;
	scale_y = dragon_height / height + 1;
	scale = (scale_x > scale_y ? scale_x : scale_y);
	deltaJ = (scale * width - dragon_width) / 2;
	deltaI = (scale * height - dragon_height) / 2;

	dragon = (char *) malloc(dragon_surface);
	if (dragon == NULL) {
		free_palette(palette);
		*canvas = NULL;
		return -1;
	}

	data.nb_thread = nb_thread;
	data.dragon = dragon;
	data.image = image;
	data.size = size;
	data.image_height = height;
	data.image_width = width;
	data.dragon_width = dragon_width;
	data.dragon_height = dragon_height;
	data.limits = limits;
	data.scale = scale;
	data.deltaI = deltaI;
	data.deltaJ = deltaJ;
	data.palette = palette;

	task_scheduler_init init(nb_thread);

	/* 2. Initialiser la surface : DragonClear */
	size_t grainsize = dragon_surface / nb_thread;
	DragonClear dc(&data);
	parallel_for(blocked_range<int>(0, dragon_surface, grainsize), dc);

	/* 3. Dessiner le dragon : DragonDraw */
	grainsize = data.size / (nb_thread * nb_thread);
	DragonDraw dd(&data);
	for (int i = 0; i < nb_thread; ++i) {
		data.id = i;
		uint64_t start = i * data.size / nb_thread;
		uint64_t end = (i + 1) * data.size / nb_thread;
		parallel_for(blocked_range<uint64_t>(start, end, grainsize), dd);
	}

	/* 4. Effectuer le rendu final : DragonRender */
	grainsize = data.image_height / nb_thread;
	DragonRender dr(&data);
	parallel_for(blocked_range<int>(0, data.image_height, grainsize), dr);

	init.terminate();
	free_palette(palette);
	*canvas = dragon;
	return 0;
}

/*
 * Calcule les limites en terme de largeur et de hauteur de
 * la forme du dragon. Requis pour allouer la matrice de dessin.
 */
int dragon_limits_tbb(limits_t *limits, uint64_t size, int nb_thread) {
	DragonLimits lim;
	size_t grainsize = size / nb_thread;
	parallel_reduce(blocked_range<uint64_t>(0, size, grainsize), lim);
	piece_t piece = lim.mGetPiece();
	*limits = piece.limits;
	return 0;
}
