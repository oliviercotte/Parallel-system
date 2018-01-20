/*
 * sinoscope_opencl.c
 *
 *  Created on: 2011-10-14
 *      Author: francis
 */

#include <iostream>

extern "C" {
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "sinoscope.h"
#include "color.h"
#include "memory.h"
#include "util.h"
}

#include "CL/opencl.h"
#include "sinoscope_opencl.h"

using namespace std;

#define BUF_SIZE 1024
#define MAGIC "!@#~"
extern char __ocl_code_start, __ocl_code_end;
static cl_command_queue queue = NULL;
static cl_context context = NULL;
static cl_program prog = NULL;
static cl_kernel kernel = NULL;

/*
 * TODO: Declarer un espace memoire pour le rendu de l'image de type cl_mem
 */
static cl_mem output = NULL;

typedef struct kernel_args kernel_args_t;

struct kernel_args {
	int width;
	int interval;
	int taylor;
	float interval_inv;
	float time;
	float phase0;
	float phase1;
	float dx;
	float dy;
};

int get_opencl_queue()
{
    cl_int ret, i;
    cl_uint num_dev;
    cl_device_id device;
    cl_platform_id *platform_ids = NULL;
    cl_uint num_platforms;
    cl_uint info;
    char vendor[BUF_SIZE];
    char name[BUF_SIZE];

    ret = clGetPlatformIDs(0, NULL, &num_platforms);
    ERR_THROW(CL_SUCCESS, ret, "failed to get number of platforms");
    ERR_ASSERT(num_platforms > 0, "no opencl platform found");

    platform_ids = (cl_platform_id *) malloc(num_platforms * sizeof(cl_platform_id));
    ERR_NOMEM(platform_ids);

    ret = clGetPlatformIDs(num_platforms, platform_ids, NULL);
    ERR_THROW(CL_SUCCESS, ret, "failed to get plateform ids");

    for (i = 0; i < num_platforms; i++) {
        ret = clGetPlatformInfo(platform_ids[i], CL_PLATFORM_VENDOR, BUF_SIZE, vendor, NULL);
        ERR_THROW(CL_SUCCESS, ret, "failed to get plateform info");
        ret = clGetPlatformInfo(platform_ids[i], CL_PLATFORM_NAME, BUF_SIZE, name, NULL);
        ERR_THROW(CL_SUCCESS, ret, "failed to get plateform info");
        cout << vendor << " " << name << "\n";
        ret = clGetDeviceIDs(platform_ids[0], CL_DEVICE_TYPE_GPU, 1, &device, &num_dev);
        if (CL_SUCCESS == ret) {
            break;
        }
        ret = clGetDeviceIDs(platform_ids[0], CL_DEVICE_TYPE_CPU, 1, &device, &num_dev);
        if (CL_SUCCESS == ret) {
            break;
        }
    }
    ERR_THROW(CL_SUCCESS, ret, "failed to find a device\n");

    ret = clGetDeviceInfo(device, CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof(cl_uint), &info, NULL);
    ERR_THROW(CL_SUCCESS, ret, "failed to get device info");
    cout << "clock frequency " << info << "\n";

    ret = clGetDeviceInfo(device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cl_uint), &info, NULL);
    ERR_THROW(CL_SUCCESS, ret, "failed to get device info");
    cout << "compute units " << info << "\n";

    context = clCreateContext(0, 1, &device, NULL, NULL, &ret);
    ERR_THROW(CL_SUCCESS, ret, "failed to create context");

    queue = clCreateCommandQueue(context, device, 0, &ret);
    ERR_THROW(CL_SUCCESS, ret, "failed to create queue");

    ret = 0;

done:
    FREE(platform_ids);
    return ret;
error:
    ret = -1;
    goto done;
}

int load_kernel_code(char **code, size_t *length)
{
    int ret = 0;
    int found = 0;
    size_t kernel_size = 0;
    size_t code_size = 0;
    char *kernel_code = NULL;
    char *ptr = NULL;
    char *code_start = NULL, *code_end = NULL;

    code_start = &__ocl_code_start;
    code_end = &__ocl_code_end;
    code_size = code_end - code_start;
    kernel_code = (char *) malloc(code_size);
    ERR_NOMEM(kernel_code);
    for (ptr = code_start; ptr < code_end - 4; ptr++) {
        if (ptr[0] == '!' && ptr[1] == '@' && ptr[2] == '#' && ptr[3] == '~') {
            found = 1;
            break;
        }
    }
    if (found == 0)
        goto error;
    kernel_size = ptr - code_start;
    strncpy(kernel_code, code_start, kernel_size);
    *code = kernel_code;
    *length = kernel_size;
done:
    return ret;
error:
    ret = -1;
    FREE(kernel_code);
    goto done;
}

int create_buffer(int width, int height)
{
    /*
     * TODO: initialiser la memoire requise avec clCreateBuffer()
     */
    cl_int ret = 0;
    output = clCreateBuffer(context, CL_MEM_READ_WRITE, width * height * 3 * sizeof(unsigned char), NULL, &ret);
    ERR_THROW(CL_SUCCESS, ret, "clCreateBuffer failed");

done:
    return ret;
error:
    ret = -1;
    goto done;
}

int opencl_init(int width, int height)
{
    cl_int err;
    char *code = NULL;
    size_t length = 0;

    get_opencl_queue();
    if (queue == NULL)
        return -1;
    load_kernel_code(&code, &length);
    if (code == NULL)
        return -1;

    /*
     * Initialisation du programme
     */
    prog = clCreateProgramWithSource(context, 1, (const char **) &code, &length, &err);
    ERR_THROW(CL_SUCCESS, err, "clCreateProgramWithSource failed");
    err = clBuildProgram(prog, 0, NULL, "-cl-fast-relaxed-math", NULL, NULL);
    ERR_THROW(CL_SUCCESS, err, "clBuildProgram failed");
    kernel = clCreateKernel(prog, "sinoscope_kernel", &err);
    ERR_THROW(CL_SUCCESS, err, "clCreateKernel failed");
    err = create_buffer(width, height);
    ERR_THROW(CL_SUCCESS, err, "create_buffer failed");

    free(code);
    return 0;
error:
    return -1;
}


void opencl_shutdown()
{
    if (queue) 	clReleaseCommandQueue(queue);
    if (context)	clReleaseContext(context);

    /*
     * TODO: liberer les ressources allouees
     */
	if (output)
	{
		clReleaseMemObject(output);
	}

	if (kernel)
	{
		clReleaseKernel(kernel);
	}

	if (prog)
	{
		clReleaseProgram(prog);
	}
}

int sinoscope_image_opencl(sinoscope_t *ptr)
{
    /*
     * TODO: Executer le noyau avec la fonction run_kernel().
     *
     *       Utilisez ERR_THROW partout pour gerer systematiquement les exceptions
     */

	if (ptr == NULL)
	{
		goto error;
	}

    cl_int ret;

    int width, height;
    size_t size;
    size_t global_work_size[2];

    ret = 0;

    width = ptr->width;
    height = ptr->height;

    kernel_args args;
    args.width = width;
    args.interval = ptr->interval;
	args.taylor = ptr->taylor;
	args.interval_inv = ptr->interval_inv;
	args.time = ptr->time;
	args.phase0 = ptr->phase0;
	args.phase1 = ptr->phase1;
	args.dx = ptr->dx;
	args.dy = ptr->dy;

    size = width * height * sizeof(unsigned char) * 3;
    global_work_size[0] = width;
    global_work_size[1] = height;

    // 1. Passer les arguments au noyau avec clSetKernelArg(). Si des
    // arguments sont passees par un tampon, copier les valeurs avec
    // clEnqueueWriteBuffer() de maniere synchrone.
    ERR_THROW(CL_SUCCESS, clSetKernelArg(kernel, 0, sizeof(cl_mem), &output), "clSetKernelArg : passing output failed");
    ERR_THROW(CL_SUCCESS, clSetKernelArg(kernel, 1, sizeof(kernel_args), &args), "clSetKernelArg : passing kernel arguments failed");

    // 2. Appeller le noyau avec clEnqueueNDRangeKernel(). L'argument
    // work_dim de clEnqueueNDRangeKernel() est un tableau size_t
    // avec les dimensions width et height.
    ERR_THROW(CL_SUCCESS, clEnqueueNDRangeKernel(queue, kernel, 2, 0, global_work_size, NULL, 0, NULL, NULL), "clEnqueueNDRangeKernel failed");

    // 3. Attendre que le noyau termine avec clFinish()
    ERR_THROW(CL_SUCCESS, clFinish(queue), "clFinish failed");

    // 4. Copier le resultat dans la structure sinoscope_t avec clEnqueueReadBuffer() de maniere synchrone
    ERR_THROW(CL_SUCCESS, clEnqueueReadBuffer(queue, output, CL_TRUE, 0, size, ptr->buf, 0 , NULL, NULL), "clEnqueueReadBuffer failed");

done:
    return ret;
error:
    ret = -1;
    goto done;
}
