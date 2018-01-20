/*
 * heatsim.c
 *
 *  Created on: 2011-11-17
 *      Author: francis
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>
#include <math.h>
#include <limits.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

#include "config.h"
#include "part.h"
#include "grid.h"
#include "cart.h"
#include "image.h"
#include "heat.h"
#include "memory.h"
#include "util.h"

#define PROGNAME "heatsim"
#define DEFAULT_OUTPUT_PPM "heatsim.png"
#define DEFAULT_DIMX 1
#define DEFAULT_DIMY 1
#define DEFAULT_ITER 100
#define MAX_TEMP 1000.0
#define DIM_2D 2

typedef struct ctx {
	cart2d_t *cart;
	grid_t *global_grid;
	grid_t *curr_grid;
	grid_t *next_grid;
	grid_t *heat_grid;
	int numprocs;
	int rank;
	MPI_Comm comm2d;
	FILE *log;
	int verbose;
	int dims[DIM_2D];
	int isperiodic[DIM_2D];
	int coords[DIM_2D];
	int reorder;
	int north_peer;
	int south_peer;
	int east_peer;
	int west_peer;
	MPI_Datatype vector;
} ctx_t;

typedef struct command_opts {
	int dimx;
	int dimy;
	int iter;
	char *input;
	char *output;
	int verbose;
} opts_t;

static opts_t *global_opts = NULL;

__attribute__((noreturn))
static void usage(void) {
	fprintf(stderr, PROGNAME " " VERSION " " PACKAGE_NAME "\n");
	fprintf(stderr, "Usage: " PROGNAME " [OPTIONS] [COMMAND]\n");
	fprintf(stderr, "\nOptions:\n");
	fprintf(stderr, "  --help	this help\n");
	fprintf(stderr, "  --iter	number of iterations to perform\n");
	fprintf(stderr, "  --dimx	2d decomposition in x dimension\n");
	fprintf(stderr, "  --dimy	2d decomposition in y dimension\n");
	fprintf(stderr, "  --input  png input file\n");
	fprintf(stderr, "  --output ppm output file\n");
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

static void dump_opts(struct command_opts *opts) {
	printf("%10s %s\n", "option", "value");
	printf("%10s %d\n", "dimx", opts->dimx);
	printf("%10s %d\n", "dimy", opts->dimy);
	printf("%10s %d\n", "iter", opts->iter);
	printf("%10s %s\n", "input", opts->input);
	printf("%10s %s\n", "output", opts->output);
	printf("%10s %d\n", "verbose", opts->verbose);
}

void default_int_value(int *val, int def) {
	if (*val == 0)
		*val = def;
}

static int parse_opts(int argc, char **argv, struct command_opts *opts) {
	int idx;
	int opt;
	int ret = 0;

	struct option options[] = { { "help", 0, 0, 'h' },
			{ "iter", 1, 0, 'r' }, { "dimx", 1, 0, 'x' }, { "dimy",
					1, 0, 'y' }, { "input", 1, 0, 'i' }, {
					"output", 1, 0, 'o' }, { "verbose", 0,
					0, 'v' }, { 0, 0, 0, 0 } };

	memset(opts, 0, sizeof(struct command_opts));

	while ((opt = getopt_long(argc, argv, "hvx:y:l:", options, &idx)) != -1) {
		switch (opt) {
		case 'r':
			opts->iter = atoi(optarg);
			break;
		case 'y':
			opts->dimy = atoi(optarg);
			break;
		case 'x':
			opts->dimx = atoi(optarg);
			break;
		case 'i':
			if (asprintf(&opts->input, "%s", optarg) < 0)
				goto err;
			break;
		case 'o':
			if (asprintf(&opts->output, "%s", optarg) < 0)
				goto err;
			break;
		case 'h':
			usage();
			break;
		case 'v':
			opts->verbose = 1;
			break;
		default:
			printf("unknown option %c\n", opt);
			ret = -1;
			break;
		}
	}

	/* default values*/
	default_int_value(&opts->iter, DEFAULT_ITER);
	default_int_value(&opts->dimx, DEFAULT_DIMX);
	default_int_value(&opts->dimy, DEFAULT_DIMY);
	if (opts->output == NULL)
		if (asprintf(&opts->output, "%s", DEFAULT_OUTPUT_PPM) < 0)
			goto err;
	if (opts->input == NULL) {
		fprintf(stderr, "missing input file");
		goto err;
	}

	if (opts->dimx == 0 || opts->dimy == 0) {
		fprintf(stderr,
				"argument error: dimx and dimy must be greater than 0\n");
		ret = -1;
	}

	if (opts->verbose)
		dump_opts(opts);
	global_opts = opts;
	return ret;
	err:
	FREE(opts->input);
	FREE(opts->output);
	return -1;
}

FILE *open_logfile(int rank) {
	char str[255];
	sprintf(str, "out-%d", rank);
	FILE *f = fopen(str, "w+");
	return f;
}

ctx_t *make_ctx() {
	ctx_t *ctx = (ctx_t *) calloc(1, sizeof(ctx_t));
	return ctx;
}

void free_ctx(ctx_t *ctx) {
	if (ctx == NULL)
		return;
	free_grid(ctx->global_grid);
	free_grid(ctx->curr_grid);
	free_grid(ctx->next_grid);
	free_grid(ctx->heat_grid);
	free_cart2d(ctx->cart);
	if (ctx->log != NULL) {
		fflush(ctx->log);
		fclose(ctx->log);
	}
	FREE(ctx);
}

int init_ctx(ctx_t *ctx, opts_t *opts) {
	//TODO("lab3");
	int ret = 0;
	MPI_Request *req = NULL;
	MPI_Status *status = NULL;

	ERR_THROW(MPI_SUCCESS, MPI_Comm_size(MPI_COMM_WORLD, &ctx->numprocs), "init_ctx - MPI_Comm_size failed\n");
	ERR_THROW(MPI_SUCCESS, MPI_Comm_rank(MPI_COMM_WORLD, &ctx->rank), "init_ctx - MPI_Comm_rank failed\n");

	if (opts->dimx * opts->dimy != ctx->numprocs) {
		fprintf(stderr, "2D decomposition blocks must equal number of process\n");
		goto error;
	}
	ctx->log = open_logfile(ctx->rank);
	ctx->verbose = opts->verbose;
	ctx->dims[0] = opts->dimx;
	ctx->dims[1] = opts->dimy;
	ctx->isperiodic[0] = 1;
	ctx->isperiodic[1] = 1;
	ctx->reorder = 0;
	grid_t *new_grid = NULL;

	/* FIXME: create 2D cartesian communicator */
	ERR_THROW(MPI_SUCCESS, MPI_Cart_create(MPI_COMM_WORLD, DIM_2D, ctx->dims, ctx->isperiodic, ctx->reorder, &ctx->comm2d), "init_ctx - MPI_Cart_create failed\n");
	ERR_THROW(MPI_SUCCESS, MPI_Cart_shift(ctx->comm2d, 1, 1, &ctx->north_peer, &ctx->south_peer), "init_ctx - MPI_Cart_shift (x) failed\n");
	ERR_THROW(MPI_SUCCESS, MPI_Cart_shift(ctx->comm2d, 0, 1, &ctx->west_peer, &ctx->east_peer), "init_ctx - MPI_Cart_shift (y) failed\n");
	ERR_THROW(MPI_SUCCESS, MPI_Cart_coords(ctx->comm2d, ctx->rank, DIM_2D, ctx->coords), "init_ctx - MPI_Cart_coords failed\n");

	/*
	 * FIXME: le processus rank=0 charge l'image du disque
	 * et transfert chaque section aux autres processus
	 */
	req = (MPI_Request *)malloc(4*ctx->numprocs * sizeof(MPI_Request));
	ERR_NOMEM(req);

	status = (MPI_Status *)malloc(4*ctx->numprocs * sizeof(MPI_Status));
	ERR_NOMEM(status);

	if(ctx->rank == 0) {
		/* load input image */
		image_t *image = load_png(opts->input);
		if (image == NULL)
			goto error;

		/* select the red channel as the heat source */
		ctx->global_grid = grid_from_image(image, CHAN_RED);

		/* grid is normalized to one, multiply by MAX_TEMP */
		grid_multiply(ctx->global_grid, MAX_TEMP);

		/* 2D decomposition */
		ctx->cart = make_cart2d(ctx->global_grid->width,
				ctx->global_grid->height, opts->dimx, opts->dimy);
		cart2d_grid_split(ctx->cart, ctx->global_grid);

		/*
		 * FIXME: send grid dimensions and data
		 * Comment traiter le cas de rank=0 ?
		 */
		int coords[DIM_2D];
		int rank;
		for(rank = 1; rank < ctx->numprocs; ++rank) {
			ERR_THROW(MPI_SUCCESS, MPI_Cart_coords(ctx->comm2d, rank, DIM_2D, coords), "init_ctx - MPI_Cart_coords - coords(1)\n");

			grid_t *grid = cart2d_get_grid(ctx->cart, coords[0], coords[1]);
			ERR_NOMEM(grid);

			ERR_THROW(MPI_SUCCESS, MPI_Isend(&grid->width, 1, MPI_INTEGER, rank, rank*4+0, ctx->comm2d, &req[(rank-1)*4]), "init_ctx - MPI_Isend - width\n");
			ERR_THROW(MPI_SUCCESS, MPI_Isend(&grid->height, 1, MPI_INTEGER, rank, rank*4+1, ctx->comm2d, &req[(rank-1)*4+1]), "init_ctx - MPI_Isend - height\n");
			ERR_THROW(MPI_SUCCESS, MPI_Isend(&grid->padding, 1, MPI_INTEGER, rank, rank*4+2, ctx->comm2d, &req[(rank-1)*4+2]), "init_ctx - MPI_Isend - padding\n");
			ERR_THROW(MPI_SUCCESS, MPI_Isend(grid->dbl, grid->pw * grid->ph, MPI_DOUBLE, rank, rank*4+3, ctx->comm2d, &req[(rank-1)*4+3]), "init_ctx - MPI_Isend - dbl\n");
		}

		ERR_THROW(MPI_SUCCESS, MPI_Waitall(4*(ctx->numprocs-1), req, status), "init_ctx - MPI_Waitall - width, height, padding dbl\n");
		ERR_THROW(MPI_SUCCESS, MPI_Cart_coords(ctx->comm2d, ctx->rank, DIM_2D, coords), "init_ctx - MPI_Cart_coords - coords(2)\n");

		new_grid = cart2d_get_grid(ctx->cart, coords[0], coords[1]);
		ERR_NOMEM(new_grid);
	}
	else {
		/*
		 * FIXME: receive dimensions of the grid
		 * store into new_grid
		 */
		int width, height, padding;
		ERR_THROW(MPI_SUCCESS, MPI_Irecv(&width, 1, MPI_INTEGER, 0, ctx->rank*4+0, ctx->comm2d, &req[0]), "init_ctx - MPI_Irecv - width\n");
		ERR_THROW(MPI_SUCCESS, MPI_Irecv(&height, 1, MPI_INTEGER, 0, ctx->rank*4+1, ctx->comm2d, &req[1]), "init_ctx - MPI_Irecv - height\n");
		ERR_THROW(MPI_SUCCESS, MPI_Irecv(&padding, 1, MPI_INTEGER, 0, ctx->rank*4+2, ctx->comm2d, &req[2]), "init_ctx - MPI_Irecv - padding\n");

		ERR_THROW(MPI_SUCCESS, MPI_Waitall(3, req, status), "init_ctx - MPI_Waitall - width, height, padding\n");

		new_grid = make_grid(width, height, padding);
		ERR_NOMEM(new_grid);

		ERR_THROW(MPI_SUCCESS, MPI_Irecv(new_grid->dbl, new_grid->pw*new_grid->ph, MPI_DOUBLE, 0, ctx->rank*4+3, ctx->comm2d, &req[0]), "MPI_Irecv - dbl\n");

		ERR_THROW(MPI_SUCCESS, MPI_Waitall(1, req, status), "MPI_Waitall - dbl\n");
	}

	/* Utilisation temporaire de global_grid */
	//new_grid = ctx->global_grid;

	if (new_grid == NULL)
		goto error;
	/* set padding required for Runge-Kutta */
	ctx->curr_grid = grid_padding(new_grid, 1);
	ctx->next_grid = grid_padding(new_grid, 1);
	ctx->heat_grid = grid_padding(new_grid, 1);
	//free_grid(new_grid);

	/* FIXME: create type vector to exchange columns */
	ERR_THROW(MPI_SUCCESS, MPI_Type_vector(ctx->curr_grid->height, 1, ctx->curr_grid->pw, MPI_DOUBLE, &ctx->vector), "init_ctx - MPI_Type_vector\n");
	ERR_THROW(MPI_SUCCESS, MPI_Type_commit(&ctx->vector), "init_ctx - MPI_Type_commit\n");

	done:
		free(req);
		free(status);
		//free_grid(new_grid);
		return ret;
	error:
		ret = -1;
		goto done;
}

void dump_ctx(ctx_t *ctx) {
	fprintf(ctx->log, "*** CONTEXT ***\n");
	fprintf(ctx->log, "rank=%d\n", ctx->rank);
	fprintf(ctx->log, "north=%d south=%d west=%d east=%d \n",
			ctx->north_peer, ctx->south_peer,
			ctx->east_peer, ctx->west_peer);
	fprintf(ctx->log, "***************\n");
}

void exchng2d(ctx_t *ctx) {
	/*
	 *  FIXME: Echanger les bordures avec les voisins
	 * 4 echanges doivent etre effectues
	 */
	//TODO("lab3");
	grid_t *grid = ctx->curr_grid;
	ERR_NOMEM(grid);

	int width = grid->width;
	int pw = grid->pw;
	int ph = grid->ph;
	int padding = grid->padding;
	MPI_Comm comm = ctx->comm2d;
	MPI_Request req[8];
	MPI_Status status[8];

	double *to_north = grid->dbl + pw * padding + padding;
	double *to_south = grid->dbl + (ph - padding - 1) * pw + padding;
	double *to_east = to_north + width - 1;
	double *to_west = to_north;

	double *from_north = to_north - pw;
	double *from_south = to_south + pw;
	double *from_east = to_east + 1;
	double *from_west = to_west - 1;

	MPI_Irecv(from_north, width, MPI_DOUBLE, ctx->north_peer, 11, comm, &req[0]);
	MPI_Irecv(from_south, width, MPI_DOUBLE, ctx->south_peer, 10, comm, &req[1]);
	MPI_Irecv(from_east, 1, ctx->vector, ctx->east_peer, 13, comm, &req[2]);
	MPI_Irecv(from_west, 1, ctx->vector, ctx->west_peer, 12, comm, &req[3]);

	MPI_Isend(to_north, width, MPI_DOUBLE, ctx->north_peer, 10, comm, &req[4]);
	MPI_Isend(to_south, width, MPI_DOUBLE, ctx->south_peer, 11, comm, &req[5]);
	MPI_Isend(to_east, 1, ctx->vector, ctx->east_peer, 12, comm, &req[6]);
	MPI_Isend(to_west, 1, ctx->vector, ctx->west_peer, 13, comm, &req[7]);

	MPI_Waitall(8, req, status);

	error:
		return;
}

int gather_result(ctx_t *ctx, opts_t *opts) {
	//TODO("lab3");

	int ret = 0;
	grid_t *local_grid = NULL;
	MPI_Request *req = NULL;
	MPI_Status *status = NULL;

	local_grid = grid_padding(ctx->next_grid, 0);
	ERR_NOMEM(local_grid);

	/*
	 * FIXME: transfer simulation results from all process to rank=0
	 * use grid for this purpose
	 */
	req = (MPI_Request *)malloc(ctx->numprocs * sizeof(MPI_Request));
	ERR_NOMEM(req);

	status = (MPI_Status *)malloc(ctx->numprocs * sizeof(MPI_Status));
	ERR_NOMEM(status);

	if(ctx->rank == 0) {
		// rank 0
		int coords[DIM_2D];
		ERR_THROW(MPI_SUCCESS, MPI_Cart_coords(ctx->comm2d, 0, DIM_2D, coords), "gather_result - MPI_Cart_coords(1)\n");

		grid_t *grid = cart2d_get_grid(ctx->cart, coords[0], coords[1]);
		ERR_NOMEM(grid);

		grid_copy(local_grid, grid);

		// Other ranks
		int rank;
		for(rank = 1; rank < ctx->numprocs; ++rank) {
			ERR_THROW(MPI_SUCCESS, MPI_Cart_coords(ctx->comm2d, rank, DIM_2D, coords), "gather_result - MPI_Cart_coords(2)\n");

			local_grid = cart2d_get_grid(ctx->cart, coords[0], coords[1]);
			ERR_NOMEM(local_grid);

			// Receiving remote grid asynchronously
			ERR_THROW(MPI_SUCCESS, MPI_Irecv(local_grid->dbl, local_grid->height * local_grid->width, MPI_DOUBLE, rank, 3, ctx->comm2d, &req[rank-1]), "gather_result - MPI_Irecv\n");
		}

		// Waiting to receive all grids from all ranks
		ERR_THROW(MPI_SUCCESS, MPI_Waitall(ctx->numprocs-1, req, status), "gather_result - MPI_Waitall(if)\n");
	}
	else {
		// Sends results to rank 0
		ERR_THROW(MPI_SUCCESS, MPI_Isend(local_grid->dbl, local_grid->height * local_grid->width, MPI_DOUBLE, 0, 3, ctx->comm2d, &req[0]), "gather_result - MPI_Isend\n");
		ERR_THROW(MPI_SUCCESS, MPI_Waitall(1, req, status), "gather_result - MPI_Waitall(else)\n");
	}

	/* now we can merge all data blocks, reuse global_grid */
	cart2d_grid_merge(ctx->cart, ctx->global_grid);
	/* temporairement copie de next_grid */
	//grid_copy(ctx->next_grid, ctx->global_grid);

done:
	free(req);
	free(status);
	//free_grid(local_grid);
	return ret;
error:
	ret = -1;
	goto done;
}

int main(int argc, char **argv) {
	ctx_t *ctx = NULL;
	int rep, ret;
	opts_t opts;

	if (parse_opts(argc, argv, &opts) < 0) {
		printf("Error while parsing arguments\n");
		usage();
	}
	if (opts.verbose)
		dump_opts(&opts);

	MPI_Init(&argc, &argv);

	ctx = make_ctx();
	if (init_ctx(ctx, &opts) < 0)
		goto err;
	if (opts.verbose)
		dump_ctx(ctx);

	if (ctx->verbose) {
		fprintf(ctx->log, "heat grid\n");
		fdump_grid(ctx->heat_grid, ctx->log);
	}

	for (rep = 0; rep < opts.iter; rep++) {
		if (ctx->verbose) {
			fprintf(ctx->log, "iter %d\n", rep);
			fprintf(ctx->log, "start\n");
			fdump_grid(ctx->curr_grid, ctx->log);
		}

		grid_set_min(ctx->heat_grid, ctx->curr_grid);
		if (ctx->verbose) {
			fprintf(ctx->log, "grid_set_min\n");
			fdump_grid(ctx->curr_grid, ctx->log);
		}

		exchng2d(ctx);
		if (ctx->verbose) {
			fprintf(ctx->log, "exchng2d\n");
			fdump_grid(ctx->curr_grid, ctx->log);
		}

		heat_diffuse(ctx->curr_grid, ctx->next_grid);
		if (ctx->verbose) {
			fprintf(ctx->log, "heat_diffuse\n");
			fdump_grid(ctx->next_grid, ctx->log);
		}
		SWAP(ctx->curr_grid, ctx->next_grid);
	}

	MPI_Barrier(MPI_COMM_WORLD);
	if (gather_result(ctx, &opts) < 0)
		goto err;

	if (ctx->rank == 0) {
		printf("saving...\n");
		if (save_grid_png(ctx->global_grid, opts.output) < 0) {
			printf("saving failed\n");
			goto err;
		}
	}

	MPI_Barrier(MPI_COMM_WORLD);
	ret = EXIT_SUCCESS;
done:
	free_ctx(ctx);
	MPI_Finalize();
	FREE(opts.input);
	FREE(opts.output);
	return ret;
err:
	MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
	ret = EXIT_FAILURE;
	goto done;
}
